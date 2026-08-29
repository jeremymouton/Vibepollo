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
import type { StreamConfig } from '@/types/webrtc';

type Phase = 'connecting' | 'playing' | 'ended' | 'error';

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
  const dpr = window.devicePixelRatio || 1;
  const w = Math.round(window.screen.width * dpr);
  const h = Math.round(window.screen.height * dpr);
  const scale = Math.min(1, 1920 / Math.max(w, 1), 1080 / Math.max(h, 1));
  const even = (n: number) => Math.max(2, Math.round((n * scale) / 2) * 2);
  return {
    width: even(w),
    height: even(h),
    fps: 60,
    encoding: 'h264',
    audioChannels: 2,
    clientName: 'Guest',
  };
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

    <div
      v-if="phase === 'playing'"
      class="absolute top-3 right-3 flex gap-2 opacity-25 hover:opacity-100 transition"
    >
      <button class="rounded px-3 py-1.5 text-sm bg-black/60 text-white" @click="goFullscreen">
        Fullscreen
      </button>
      <button class="rounded px-3 py-1.5 text-sm bg-black/60 text-white" @click="leave">
        Leave
      </button>
    </div>
  </div>
</template>
