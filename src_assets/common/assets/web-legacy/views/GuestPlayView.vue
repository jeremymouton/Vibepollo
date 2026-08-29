<script setup lang="ts">
/**
 * The page a redeemed guest actually plays on.
 *
 * Deliberately not WebRtcClientView. That view is the owner's console — an app
 * picker, session history, encoder settings, a dozen owner-only API calls — and
 * none of it is reachable, or wanted, by someone holding an invite link. This
 * mounts the same streaming engine with nothing around it, so a guest never
 * loads the admin app and there is no admin surface for them to be refused by.
 *
 * The stream's permissions are not decided here. createWebRTCSession overwrites
 * the grant from the invite server-side, so anything this page asks for is
 * discarded — which is why it does not ask.
 */
import { onBeforeUnmount, onMounted, ref } from 'vue';

import { WebRtcHttpApi } from '@/services/webrtcApi';
import { attachInputCapture } from '@/utils/webrtc/input';
import { WebRtcClient } from '@/utils/webrtc/client';
import TouchGamepad from '@/components/TouchGamepad.vue';
import type { EncodingType, StreamConfig } from '@/types/webrtc';

type Phase = 'connecting' | 'playing' | 'ended' | 'error';

/// What this browser can actually decode. Offering AV1 to a device that cannot
/// decode it produces a black screen rather than an error, so the list is built
/// from the browser's own answer instead of assumed.
function decodableCodecs(): EncodingType[] {
  const wanted: Array<[EncodingType, string[]]> = [
    ['h264', ['video/h264']],
    ['hevc', ['video/h265', 'video/hevc']],
    ['av1', ['video/av1']],
  ];
  let available: string[] = [];
  try {
    available = (RTCRtpReceiver.getCapabilities?.('video')?.codecs ?? []).map((c) =>
      c.mimeType.toLowerCase(),
    );
  } catch {
    /* an engine that will not say; h264 is the safe floor */
  }
  const found = wanted
    .filter(([, mimes]) => mimes.some((m) => available.includes(m)))
    .map(([name]) => name);
  return found.length ? found : ['h264'];
}

const codecs = decodableCodecs();

interface Quality {
  codec: EncodingType;
  maxHeight: number;
  fps: number;
  bitrateKbps: number;
}

const QUALITY_KEY = 'vibepollo.guest.quality';

/// Best the browser can decode, preferring newer codecs — they hold up far better
/// at the bitrates a guest on mobile data actually gets.
function defaultQuality(): Quality {
  const best: EncodingType = codecs.includes('av1')
    ? 'av1'
    : codecs.includes('hevc')
      ? 'hevc'
      : 'h264';
  return { codec: best, maxHeight: 1080, fps: 60, bitrateKbps: 0 };
}

function loadQuality(): Quality {
  const fallback = defaultQuality();
  try {
    const raw = window.localStorage.getItem(QUALITY_KEY);
    if (!raw) return fallback;
    const saved = JSON.parse(raw) as Partial<Quality>;
    return {
      // A codec saved on another device, or one this browser has since lost, must
      // not strand the guest on a black screen.
      codec: saved.codec && codecs.includes(saved.codec) ? saved.codec : fallback.codec,
      maxHeight: Number(saved.maxHeight) || fallback.maxHeight,
      fps: Number(saved.fps) || fallback.fps,
      bitrateKbps: Number(saved.bitrateKbps) || 0,
    };
  } catch {
    return fallback;
  }
}

const quality = ref<Quality>(loadQuality());
const showSettings = ref(false);

const isTouch =
  typeof window !== 'undefined' && (navigator.maxTouchPoints > 0 || 'ontouchstart' in window);
const showTouchPad = ref(isTouch);

const phase = ref<Phase>('connecting');
const message = ref('Connecting to the host…');
const videoEl = ref<HTMLVideoElement>();
const stageEl = ref<HTMLDivElement>();

const client = new WebRtcClient(new WebRtcHttpApi());
const noop = (): void => undefined;
/// Detaches the current input capture. `noop` until one is attached, so there is
/// no absent state to model and nothing to guard at the call sites.
let detachInput: () => void = noop;

/// Ask for what this display can actually show, capped at 1080p60. A guest has no
/// settings screen, so this is the only chance to pick something sensible.
function streamConfig(): StreamConfig {
  const q = quality.value;
  const dpr = window.devicePixelRatio || 1;
  const w = Math.round(window.screen.width * dpr);
  const h = Math.round(window.screen.height * dpr);
  const cap = q.maxHeight;
  const scale = Math.min(1, (cap * 16) / 9 / Math.max(w, 1), cap / Math.max(h, 1));
  const even = (n: number) => Math.max(2, Math.round((n * scale) / 2) * 2);
  const config: StreamConfig = {
    width: even(w),
    height: even(h),
    fps: q.fps,
    encoding: q.codec,
    audioChannels: 2,
    clientName: 'Guest',
  };
  // Unset means "let the host decide and let congestion control move it"; a value
  // pins the ceiling for a guest who knows their line better than the estimator.
  if (q.bitrateKbps > 0) config.bitrateKbps = q.bitrateKbps;
  return config;
}

async function applyQuality(): Promise<void> {
  try {
    window.localStorage.setItem(QUALITY_KEY, JSON.stringify(quality.value));
  } catch {
    /* private browsing; the choice just will not persist */
  }
  showSettings.value = false;
  // The session's encoder is fixed when it is created, so a change means a new one.
  detachInput();
  detachInput = noop;
  await client.disconnect().catch(() => undefined);
  phase.value = 'connecting';
  message.value = 'Reconnecting…';
  await start();
}

function wireInput(): void {
  detachInput();
  detachInput = noop;
  if (!stageEl.value) return;
  detachInput = attachInputCapture(stageEl.value, (payload) => client.sendInput(payload), {
    video: videoEl.value ?? null,
  });
}

async function start(): Promise<void> {
  try {
    await client.connect(streamConfig(), {
      onRemoteStream: (stream) => {
        if (!videoEl.value) return;
        videoEl.value.srcObject = stream;
        // Autoplay with sound is refused until the guest interacts with the page.
        // Start muted so video appears immediately, and unmute on their first click.
        videoEl.value.muted = true;
        void videoEl.value.play().catch(() => undefined);
      },
      onConnectionState: (state) => {
        if (state === 'connected') {
          phase.value = 'playing';
          message.value = '';
          wireInput();
        } else if (state === 'failed' || state === 'closed') {
          phase.value = 'ended';
          message.value = 'The connection to the host ended.';
        }
      },
      onError: (error) => {
        phase.value = 'error';
        message.value = error.message || 'Could not start the stream.';
      },
      onWarning: (warning) => console.warn(warning),
    });
  } catch (err) {
    phase.value = 'error';
    message.value =
      err instanceof Error ? err.message : 'Could not start the stream. Ask for a fresh link.';
  }
}

function unmuteOnFirstClick(): void {
  if (videoEl.value?.muted) {
    videoEl.value.muted = false;
    void videoEl.value.play().catch(() => undefined);
  }
}

async function goFullscreen(): Promise<void> {
  try {
    await stageEl.value?.requestFullscreen?.();
  } catch {
    /* the browser refused; the stream is still playable windowed */
  }
}

async function leave(): Promise<void> {
  detachInput();
  detachInput = noop;
  await client.disconnect().catch(() => undefined);
  phase.value = 'ended';
  message.value = 'You have left the session.';
}

onMounted(start);
onBeforeUnmount(() => {
  detachInput();
  // keepalive so a reload does not strand the session on the host
  void client.disconnect({ keepalive: true }).catch(() => undefined);
});
</script>

<template>
  <div
    ref="stageEl"
    class="relative w-screen h-screen bg-black overflow-hidden"
    tabindex="0"
    @click="unmuteOnFirstClick"
  >
    <video
      ref="videoEl"
      class="w-full h-full object-contain bg-black"
      autoplay
      playsinline
      disablepictureinpicture
    ></video>

    <div
      v-if="phase !== 'playing'"
      class="absolute inset-0 flex flex-col items-center justify-center gap-4 text-center text-white/90 bg-black/80 px-6"
    >
      <img src="/images/logo-apollo-45.png" alt="" class="h-14 w-14 opacity-80" />
      <p class="text-lg">{{ message }}</p>
      <button
        v-if="phase === 'error' || phase === 'ended'"
        class="rounded px-4 py-2 bg-white/15 hover:bg-white/25 transition"
        @click="
          phase = 'connecting';
          message = 'Connecting to the host…';
          start();
        "
      >
        Try again
      </button>
    </div>

    <TouchGamepad v-if="phase === 'playing' && showTouchPad" />

    <div
      v-if="phase === 'playing'"
      class="absolute top-3 right-3 flex gap-2 opacity-25 hover:opacity-100 transition"
    >
      <button
        v-if="isTouch"
        class="rounded px-3 py-1.5 text-sm bg-black/60 text-white"
        @click="showTouchPad = !showTouchPad"
      >
        {{ showTouchPad ? 'Hide pad' : 'Show pad' }}
      </button>
      <button
        class="rounded px-3 py-1.5 text-sm bg-black/60 text-white"
        @click="showSettings = !showSettings"
      >
        Quality
      </button>
      <button class="rounded px-3 py-1.5 text-sm bg-black/60 text-white" @click="goFullscreen">
        Fullscreen
      </button>
      <button class="rounded px-3 py-1.5 text-sm bg-black/60 text-white" @click="leave">
        Leave
      </button>
    </div>

    <div
      v-if="showSettings"
      class="absolute top-16 right-3 w-64 rounded-lg bg-black/85 p-4 text-sm text-white space-y-3"
    >
      <label class="block">
        Codec
        <select v-model="quality.codec" class="mt-1 w-full rounded bg-white/10 p-1.5">
          <option v-for="c in codecs" :key="c" :value="c">
            {{ c === 'h264' ? 'H.264' : c === 'hevc' ? 'HEVC' : 'AV1' }}
          </option>
        </select>
        <span class="text-white/50 text-xs">Only codecs this browser can decode are listed.</span>
      </label>
      <label class="block">
        Resolution
        <select v-model.number="quality.maxHeight" class="mt-1 w-full rounded bg-white/10 p-1.5">
          <option :value="720">Up to 720p</option>
          <option :value="1080">Up to 1080p</option>
          <option :value="1440">Up to 1440p</option>
        </select>
      </label>
      <label class="block">
        Frame rate
        <select v-model.number="quality.fps" class="mt-1 w-full rounded bg-white/10 p-1.5">
          <option :value="30">30 fps</option>
          <option :value="60">60 fps</option>
        </select>
      </label>
      <label class="block">
        Bitrate
        <select v-model.number="quality.bitrateKbps" class="mt-1 w-full rounded bg-white/10 p-1.5">
          <option :value="0">Automatic</option>
          <option :value="5000">5 Mbps</option>
          <option :value="10000">10 Mbps</option>
          <option :value="20000">20 Mbps</option>
          <option :value="40000">40 Mbps</option>
        </select>
        <span class="text-white/50 text-xs">Automatic follows the connection.</span>
      </label>
      <button class="w-full rounded bg-white/20 py-1.5" @click="applyQuality">
        Apply and reconnect
      </button>
    </div>
  </div>
</template>
