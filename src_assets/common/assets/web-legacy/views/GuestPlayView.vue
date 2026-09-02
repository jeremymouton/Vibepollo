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
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue';

import { http } from '@/http';
import { WebRtcHttpApi } from '@/services/webrtcApi';
import {
  applyGamepadFeedback,
  attachInputCapture,
  requestKeyboardLock,
} from '@/utils/webrtc/input';
import { WebRtcClient } from '@/utils/webrtc/client';
import { attachVideoUpscaler } from '@/utils/webrtc/upscaler';
import { fetchIceServers, probeConnectivity } from '@/utils/webrtc/connectivity';
import type { ConnectivityReport } from '@/utils/webrtc/connectivity';
import TouchGamepad from '@/components/TouchGamepad.vue';
import type { EncodingType, StreamConfig, WebRtcStatsSnapshot } from '@/types/webrtc';

/// 'ready' exists so the guest picks before anything starts. A stream's encoder is
/// fixed when the session is created, so a choice made afterwards costs a reconnect —
/// and a guest on a phone or a poor line is exactly who most needs to choose first.
/// 'expired' is the guest's cookie being gone — eight hours up, the invite revoked,
/// or /play opened without coming through the link. Nothing on this page can fix
/// that, so unlike 'error' it offers no retry.
type Phase = 'ready' | 'connecting' | 'playing' | 'ended' | 'error' | 'expired';

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
/// Shown by default only where touch is the ONLY way to point: a phone or a tablet
/// held in the hands. A touch laptop or an iPad with a trackpad answers maxTouchPoints
/// too, and there the pad covered the stage and swallowed every mouse event.
const coarseOnly =
  typeof window !== 'undefined' &&
  typeof window.matchMedia === 'function' &&
  window.matchMedia('(pointer: coarse) and (hover: none)').matches;
const showTouchPad = ref(coarseOnly);

/// A real controller makes the on-screen one redundant, and both being reported at
/// once claims two host pads for one guest.
function physicalPadPresent(): boolean {
  try {
    return Array.from(navigator.getGamepads?.() ?? []).some((pad) => pad?.connected);
  } catch {
    return false;
  }
}
function onGamepadConnected(): void {
  showTouchPad.value = false;
}

/// Element fullscreen does not exist on iPhone Safari, so the button would be a
/// silent no-op there.
const fullscreenAvailable =
  typeof document !== 'undefined' && Boolean(document.fullscreenEnabled);

/// Shown on request. Without it a guest can only report "it was bad", which says
/// nothing about whether the link was slow, lossy, or the host was struggling —
/// three problems with three different answers.
const stats = ref<WebRtcStatsSnapshot>({});
const showStats = ref(false);

/// host = straight to the PC, srflx = through NAT but still direct, relay = via the
/// TURN server. Which one it landed on decides whether latency is the internet's
/// fault or a routing detour.
const pathKind = computed(() => {
  const local = stats.value.candidatePair?.localType ?? '';
  const remote = stats.value.candidatePair?.remoteType ?? '';
  if (local === 'relay' || remote === 'relay') return 'relayed';
  if (local === 'host' && remote === 'host') return 'direct (local)';
  return 'direct';
});

const phase = ref<Phase>('ready');
const message = ref('Connecting to the host…');
const videoEl = ref<HTMLVideoElement>();
const stageEl = ref<HTMLDivElement>();

/// Client-side FSR (EASU upscale + RCAS sharpen) between the decoded frame and the
/// screen. Pays off exactly when a guest streams below display resolution — 720p on
/// a constrained line shown on a 1440p screen — which is why it lives here and not
/// in the stream config: it costs nothing on the wire and needs no reconnect.
/// Connection check, offered before the guest starts anything.
///
/// A guest who cannot connect, or who connects badly, currently has no way to say
/// anything more useful than "it doesn't work" — and the owner has no way to tell
/// a blocked network from a relayed one from a busy host. This asks the ICE
/// servers directly. It starts no stream, so it costs the host no encoder and the
/// guest no invite use.
const connTesting = ref(false);
const connReport = ref<ConnectivityReport | null>(null);
const connError = ref('');

async function runConnectionTest(): Promise<void> {
  connTesting.value = true;
  connError.value = '';
  connReport.value = null;
  try {
    connReport.value = await probeConnectivity(await fetchIceServers());
  } catch (err) {
    connError.value = err instanceof Error ? err.message : 'Connection test failed';
  } finally {
    connTesting.value = false;
  }
}

/// Written for someone who did not set any of this up and cannot fix the host.
/// Each line says what it means for them, not what was measured.
const connSummary = computed(() => {
  const r = connReport.value;
  if (!r) return '';
  switch (r.verdict) {
    case 'good':
      return 'Looks good — a direct connection is available.';
    case 'relay-only':
      return 'Your browser is hiding your local network, often a VPN or privacy extension. It should still work, routed through the relay.';
    case 'degraded':
      return 'No relay is available. If you cannot reach the host directly, this will not connect.';
    default:
      return 'This network is blocking the connection. Try a different network, or turn off a VPN.';
  }
});

const ENHANCE_KEY = 'vibepollo.guest.enhance';
const enhance = ref<boolean>(
  (() => {
    try {
      return window.localStorage.getItem(ENHANCE_KEY) === '1';
    } catch {
      return false;
    }
  })(),
);
/// WebGL2 missing or the pipeline died: the checkbox greys out rather than lying.
const enhanceUnavailable = ref(false);
const fxCanvas = ref<HTMLCanvasElement>();
const noopDetach = (): void => undefined;
let detachUpscaler: () => void = noopDetach;

const upscalerActive = computed(
  () => enhance.value && !enhanceUnavailable.value && phase.value === 'playing',
);

function syncUpscaler(): void {
  detachUpscaler();
  detachUpscaler = noopDetach;
  if (!upscalerActive.value || !videoEl.value || !fxCanvas.value) return;
  const attached = attachVideoUpscaler(videoEl.value, fxCanvas.value, {
    onFailure: () => {
      enhanceUnavailable.value = true;
    },
  });
  if (!attached) {
    enhanceUnavailable.value = true;
    return;
  }
  detachUpscaler = () => attached.detach();
}

// flush: 'post' so the canvas exists in the DOM before an attach is attempted.
watch(upscalerActive, () => syncUpscaler(), { flush: 'post' });
watch(enhance, (on) => {
  try {
    window.localStorage.setItem(ENHANCE_KEY, on ? '1' : '0');
  } catch {
    /* private browsing; the choice just will not persist */
  }
});

const client = new WebRtcClient(new WebRtcHttpApi());
const sessionId = ref('');
const noop = (): void => undefined;
/// Detaches the current input capture. `noop` until one is attached, so there is
/// no absent state to model and nothing to guard at the call sites.
let detachInput: () => void = noop;

/// Ask for what this display can actually show, capped at 1080p60. A guest has no
/// settings screen, so this is the only chance to pick something sensible.
function streamConfig(): StreamConfig {
  const q = quality.value;
  const dpr = window.devicePixelRatio || 1;
  // Always ask for a landscape frame. A phone held upright reports 390x844, and the
  // host will faithfully stream a 390x844 desktop — a tall slice of a wide screen,
  // which is unreadable and looks like the stream is broken. The screen's longer
  // edge is the width whichever way the phone happens to be held.
  const longEdge = Math.round(Math.max(window.screen.width, window.screen.height) * dpr);
  const shortEdge = Math.round(Math.min(window.screen.width, window.screen.height) * dpr);
  // Never taller than 9:16 of the width either: a very tall phone would otherwise
  // still ask for a squarer desktop than the host actually has.
  const w = longEdge;
  const h = Math.min(shortEdge, Math.round((longEdge * 9) / 16));
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

/// Remembers the choice before connecting, so a returning guest is not asked twice.
async function startChosen(): Promise<void> {
  saveQuality();
  phase.value = 'connecting';
  message.value = 'Connecting to the host…';
  await start();
}

function saveQuality(): void {
  try {
    window.localStorage.setItem(QUALITY_KEY, JSON.stringify(quality.value));
  } catch {
    /* private browsing; the choice just will not persist */
  }
}

async function applyQuality(): Promise<void> {
  saveQuality();
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
    sessionId.value = await client.connect(streamConfig(), {
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
          stopTelemetry();
          telemetryTimer = [setInterval(() => void reportTelemetry(), 10_000)];
        } else if (state === 'disconnected') {
          // Media has stopped but the peer may still come back; the client waits
          // five seconds before giving up. Say so, rather than leaving a frozen
          // frame that looks like the game hung.
          phase.value = 'connecting';
          message.value = 'Connection interrupted. Trying to recover…';
          stopTelemetry();
        } else if (state === 'failed' || state === 'closed') {
          phase.value = 'ended';
          message.value = 'The connection to the host ended.';
          stopTelemetry();
        }
      },
      onError: (error) => {
        phase.value = 'error';
        message.value = error.message || 'Could not start the stream.';
        stopTelemetry();
      },
      onWarning: (warning) => console.warn(warning),
      onStats: (snapshot) => {
        stats.value = snapshot;
      },
      // Rumble. The host sends it down the same data channel the input goes up, and
      // without this a guest holding a controller that can shake simply never feels
      // anything — the messages were arriving and being dropped on the floor.
      onInputMessage: (message) => applyGamepadFeedback(message),
    });
  } catch (err) {
    stopTelemetry();
    const status = (err as { status?: number } | null)?.status;
    if (status === 401 || status === 403) {
      // The cookie is gone or refused. The raw "HTTP 401" text told the guest
      // nothing, and the retry it came with could only ever get the same answer.
      phase.value = 'expired';
      message.value = 'Your invite session has ended. Open your invite link again.';
      return;
    }
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
  // Take the system keys too, so Escape and the Windows key reach the game instead
  // of the browser. Only granted in fullscreen, and only in some browsers.
  void requestKeyboardLock();
  // Turn the phone for them. Only allowed while fullscreen, and refused outright on
  // desktop and on iOS, so it is attempted and ignored rather than depended upon.
  try {
    const orientation = screen.orientation as ScreenOrientation & {
      lock?: (to: string) => Promise<void>;
    };
    await orientation.lock?.('landscape');
  } catch {
    /* not permitted here; the frame is landscape regardless */
  }
}

async function leave(): Promise<void> {
  stopTelemetry();
  detachInput();
  detachInput = noop;
  await client.disconnect().catch(() => undefined);
  phase.value = 'ended';
  message.value = 'You have left the session.';
}

/// Reported every ten seconds while playing, so a session that went badly leaves a
/// trace in the host log the owner can actually read afterwards. Ten seconds is
/// often enough to see a stream degrade and rare enough to be invisible.
/// Held as a 0-or-1 list rather than a nullable, which this project's lint rules
/// discourage; clearInterval on an empty list is simply a no-op.
let telemetryTimer: ReturnType<typeof setInterval>[] = [];

function stopTelemetry(): void {
  telemetryTimer.forEach(clearInterval);
  telemetryTimer = [];
}

function candidateKind(): string {
  const local = stats.value.candidatePair?.localType ?? '';
  const remote = stats.value.candidatePair?.remoteType ?? '';
  if (local === 'relay' || remote === 'relay') return 'relay';
  if (local === 'host' && remote === 'host') return 'host';
  return 'srflx';
}

async function reportTelemetry(): Promise<void> {
  const id = sessionId.value;
  if (!id || phase.value !== 'playing') return;
  const s = stats.value;
  try {
    await http.post(
      `/api/webrtc/sessions/${encodeURIComponent(id)}/telemetry`,
      {
        path: candidateKind(),
        rtt: s.roundTripTimeMs ?? 0,
        jitter: s.videoJitterMs ?? 0,
        buffer: s.videoJitterBufferMs ?? 0,
        lost: s.packetsLost ?? 0,
        bitrate: s.videoBitrateKbps ?? 0,
        fps: s.videoFps ?? 0,
        decode: s.videoDecodeMs ?? 0,
        dropped: s.videoFramesDropped ?? 0,
      },
      // A guest is never "authenticated" in the auth store's sense, and without this
      // flag the request interceptor cancelled the call before it left the browser.
      // Nothing ever reached the host log.
      { validateStatus: () => true, __allowUnauthenticated: true } as Record<string, unknown>,
    );
  } catch {
    /* telemetry must never be the reason a stream stops */
  }
}

/// The connectivity heartbeat reloads the page after a network blip unless a
/// stream is marked active. A reload here is the end of the game.
watch(phase, (value) => {
  (window as Window & { __sunshine_webrtc_active?: boolean }).__sunshine_webrtc_active =
    value === 'playing';
});

/// onBeforeUnmount runs on SPA navigation only, and a guest never navigates within
/// the SPA — /join is a full page and so is Back. Reload, Back and closing the tab
/// all arrive as pagehide, and each used to strand the session on the host until
/// the silence reaper found it.
function onPageHide(): void {
  void client.disconnect({ keepalive: true }).catch(() => undefined);
}

onMounted(() => {
  message.value = '';
  if (physicalPadPresent()) showTouchPad.value = false;
  window.addEventListener('gamepadconnected', onGamepadConnected);
  window.addEventListener('pagehide', onPageHide);
});
onBeforeUnmount(() => {
  window.removeEventListener('gamepadconnected', onGamepadConnected);
  window.removeEventListener('pagehide', onPageHide);
  (window as Window & { __sunshine_webrtc_active?: boolean }).__sunshine_webrtc_active = false;
  stopTelemetry();
  detachInput();
  detachUpscaler();
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
      :class="{ 'opacity-0': upscalerActive }"
      autoplay
      playsinline
      disablepictureinpicture
    ></video>
    <!-- FSR output. The video stays mounted and playing underneath (it is the decode
         surface and audio sink); opacity keeps it compositing so frame callbacks
         still fire, unlike display:none. -->
    <canvas
      v-show="upscalerActive"
      ref="fxCanvas"
      class="absolute inset-0 w-full h-full pointer-events-none"
    ></canvas>

    <!-- Scrolls when it has to. A phone held sideways has barely any height, and
         centring alone clips the overflow in both directions — the Start button
         ended up below the fold with no way to reach it. min-h-full on the inner
         block keeps it centred whenever there IS room. -->
    <div v-if="phase !== 'playing'" class="absolute inset-0 overflow-y-auto bg-black/80">
      <div
        class="min-h-full flex flex-col items-center justify-center gap-4 px-6 py-8 text-center text-white/90"
      >
        <img src="/images/logo-apollo-45.png" alt="" class="h-14 w-14 opacity-80" />

        <div v-if="phase === 'ready'" class="w-full max-w-xs space-y-3 text-left text-sm">
          <p class="text-center text-base">Ready when you are.</p>
          <label class="block">
            Quality
            <select v-model.number="quality.maxHeight" class="mt-1 w-full rounded bg-white/10 p-2">
              <option :value="720">720p — best on a phone or a slow line</option>
              <option :value="1080">1080p</option>
              <option :value="1440">1440p — needs a fast connection</option>
            </select>
          </label>
          <label class="block">
            Bitrate
            <select
              v-model.number="quality.bitrateKbps"
              class="mt-1 w-full rounded bg-white/10 p-2"
            >
              <option :value="0">Automatic</option>
              <option :value="5000">5 Mbps</option>
              <option :value="10000">10 Mbps</option>
              <option :value="20000">20 Mbps</option>
              <option :value="40000">40 Mbps</option>
            </select>
          </label>
          <label class="block">
            Frame rate
            <select v-model.number="quality.fps" class="mt-1 w-full rounded bg-white/10 p-2">
              <option :value="30">30 fps</option>
              <option :value="60">60 fps</option>
            </select>
          </label>
          <p class="text-white/50 text-xs">
            Automatic follows your connection. You can change all of this while playing.
          </p>
          <button class="w-full rounded bg-white/25 py-2 text-base" @click="startChosen">
            Start streaming
          </button>

          <!-- Below Start on purpose: it is a diagnostic for when something is
               wrong, not a step everyone has to work through first. -->
          <button
            class="w-full rounded border border-white/25 py-2 text-sm disabled:opacity-50"
            :disabled="connTesting"
            @click="runConnectionTest"
          >
            {{ connTesting ? 'Checking your connection…' : 'Check my connection' }}
          </button>
          <p v-if="connError" class="text-xs text-red-300">{{ connError }}</p>
          <div v-if="connReport" class="space-y-1 rounded bg-white/10 p-2 text-xs">
            <p>{{ connSummary }}</p>
            <p class="text-white/50">
              Local network: {{ connReport.hasHost ? 'yes' : 'hidden' }} ·
              Relay: {{ connReport.relayOnlyWorks ? `${connReport.relayLatencyMs ?? '?'} ms` : 'unavailable' }}
            </p>
          </div>
        </div>

        <p v-else class="text-lg">{{ message }}</p>
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
    </div>

    <TouchGamepad v-if="phase === 'playing' && showTouchPad" />

    <!-- data-chrome: these are the page's controls, not game input. The capture
         layer leaves pointers on them alone, so a click lands on the button instead
         of being captured by the stage and forwarded to the host as a mouse press.
         Half-opaque at rest rather than a quarter: a phone has no hover to reveal it. -->
    <div
      v-if="phase === 'playing'"
      class="absolute top-3 right-3 flex gap-2 opacity-50 hover:opacity-100 focus-within:opacity-100 transition"
      data-chrome
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
        @click="showStats = !showStats"
      >
        Stats
      </button>
      <button
        class="rounded px-3 py-1.5 text-sm bg-black/60 text-white"
        @click="showSettings = !showSettings"
      >
        Quality
      </button>
      <button
        v-if="fullscreenAvailable"
        class="rounded px-3 py-1.5 text-sm bg-black/60 text-white"
        @click="goFullscreen"
      >
        Fullscreen
      </button>
      <button class="rounded px-3 py-1.5 text-sm bg-black/60 text-white" @click="leave">
        Leave
      </button>
    </div>

    <div
      v-if="showStats && phase === 'playing'"
      class="absolute top-3 left-3 rounded-lg bg-black/80 px-3 py-2 text-xs text-white/90 font-mono leading-5"
      data-chrome
    >
      <div>path {{ pathKind }} {{ stats.candidatePair?.protocol ?? '' }}</div>
      <div>rtt {{ Math.round(stats.roundTripTimeMs ?? 0) }} ms</div>
      <div>jitter {{ Math.round(stats.videoJitterMs ?? 0) }} ms</div>
      <div>buffer {{ Math.round(stats.videoJitterBufferMs ?? 0) }} ms</div>
      <div>lost {{ stats.packetsLost ?? 0 }}</div>
      <div>
        {{ Math.round(stats.videoBitrateKbps ?? 0) }} kbps ·
        {{ Math.round(stats.videoFps ?? 0) }} fps
      </div>
      <div>decode {{ Math.round(stats.videoDecodeMs ?? 0) }} ms · {{ stats.videoCodec ?? '' }}</div>
      <div>dropped {{ stats.videoFramesDropped ?? 0 }}</div>
    </div>

    <!-- Gated on playing as well: it sits later in the DOM than the ended/error
         overlay and would otherwise render on top of "You have left the session." -->
    <div
      v-if="showSettings && phase === 'playing'"
      class="absolute top-16 right-3 w-64 rounded-lg bg-black/85 p-4 text-sm text-white space-y-3"
      data-chrome
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
      <label class="flex items-start gap-2">
        <input
          v-model="enhance"
          type="checkbox"
          class="mt-1"
          :disabled="enhanceUnavailable"
        />
        <span>
          Sharpen upscaled video (FSR)
          <span class="block text-white/50 text-xs">
            {{
              enhanceUnavailable
                ? 'Not available on this device.'
                : 'Sharper picture when streaming below your screen resolution. Applies instantly.'
            }}
          </span>
        </span>
      </label>
      <button class="w-full rounded bg-white/20 py-1.5" @click="applyQuality">
        Apply and reconnect
      </button>
    </div>
  </div>
</template>
