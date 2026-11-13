<script setup lang="ts">
import { computed, onActivated, onBeforeUnmount, onDeactivated, onMounted, reactive, ref, watch } from 'vue';
import { useI18n } from 'vue-i18n';

import { apiGet, apiPost } from '@/api/client';
import {
  AppButton,
  ConfirmDialog,
  InlineAlert,
  LoadingSkeleton,
  PageHeader,
  StatusBadge,
  UiIcon,
  type StatusTone,
} from '@/components/ui';
import { appCoverUrl, appName, fetchApps, type AppRecord } from '@/services/apps';
import {
  BrowserWebRtcSession,
  detectBrowserVideoCapabilities,
  fetchWebRtcHostCapabilities,
  unavailableHostCapabilities,
  WebRtcConnectionCanceledError,
  type BrowserVideoCapabilities,
  type WebRtcHostCapabilities,
} from '@/services/webrtc';
import TouchGamepad from '@/components/TouchGamepad.vue';
import { useActiveStream } from '@/stores/activeStream';
import {
  applyGamepadFeedback,
  attachBrowserGamepadCapture,
  type BrowserGamepadCapture,
} from '@/utils/webrtc/browserGamepad';
import { encodeMouseMove, getGamepads } from '@/utils/webrtc/input';
import type { SessionStatus } from '@/types/sessions';
import { attachVideoUpscaler } from '@/utils/webrtc/upscaler';
import { fetchIceServers, probeConnectivity } from '@/utils/webrtc/connectivity';
import { assessLinkQuality } from '@/utils/webrtc/linkQuality';
import type { ConnectivityReport } from '@/utils/webrtc/connectivity';
import type { EncodingType, StreamConfig, WebRtcStatsSnapshot } from '@/types/webrtc';
import { formatBitrate } from '@/utils/format';

interface LaunchableApp {
  coverUrl: string;
  id: number;
  name: string;
}

interface StreamLaunchForm {
  appId: string;
  bitrateKbps: number;
  encoding: EncodingType;
  fps: number;
  hdr: boolean;
  height: number;
  muteHostAudio: boolean;
  videoMaxFrameAgeFrames: number;
  videoPacingMode: PacingMode;
  videoPacingSlackMs: number;
  volume: number;
  width: number;
}

interface PressedMouseButton {
  button: number;
  modifiers: Record<string, boolean>;
  x: number;
  y: number;
}

interface PressedKey {
  code: string;
  key: string;
  modifiers: Record<string, boolean>;
}

interface PointerPosition {
  x: number;
  y: number;
}

interface TouchPointerGesture {
  button: number;
  dragThresholdPx: number;
  dragging: boolean;
  lastPosition: PointerPosition;
  modifiers: Record<string, boolean>;
  startClientX: number;
  startClientY: number;
  startPosition: PointerPosition;
  startedAtMs: number;
}

interface MutationResponse {
  error?: string;
  status?: boolean;
}

interface WebKitFullscreenElement extends HTMLElement {
  webkitRequestFullScreen?: () => Promise<void> | void;
  webkitRequestFullscreen?: () => Promise<void> | void;
}

interface WebKitFullscreenVideoElement extends HTMLVideoElement {
  webkitDisplayingFullscreen?: boolean;
  webkitEnterFullScreen?: () => void;
  webkitEnterFullscreen?: () => void;
  webkitExitFullscreen?: () => void;
}

interface WebKitFullscreenDocument extends Document {
  webkitCancelFullScreen?: () => Promise<void> | void;
  webkitExitFullscreen?: () => Promise<void> | void;
}

interface KeyboardLockNavigator extends Navigator {
  keyboard?: {
    lock?: (keys?: string[]) => Promise<void>;
    unlock?: () => void;
  };
}

interface StandaloneNavigator extends Navigator {
  standalone?: boolean;
}

type PacingMode = 'latency' | 'balanced' | 'smoothness';

const { t } = useI18n();
const codecs: EncodingType[] = ['h264', 'hevc', 'av1'];
const browserSession = new BrowserWebRtcSession();

const pacingPresets: Record<PacingMode, { slackMs: number; maxAgeFrames: number }> = {
  latency: { slackMs: 0, maxAgeFrames: 1 },
  balanced: { slackMs: 2, maxAgeFrames: 1 },
  smoothness: { slackMs: 3, maxAgeFrames: 3 },
};
const minFrameAgeMs = 5;
const maxFrameAgeMs = 100;
const maxFrameAgeFrameCount = 10;

function maxAllowedFramesForFps(fps: number): number {
  const safeFps = Number.isFinite(fps) && fps > 0 ? fps : 60;
  return Math.max(1, Math.min(maxFrameAgeFrameCount, Math.floor((maxFrameAgeMs * safeFps) / 1000)));
}

function clampFrameAgeFrames(
  value: number | null | undefined,
  fps: number,
  mode: PacingMode,
): number {
  const preset = pacingPresets[mode].maxAgeFrames;
  const maxAllowed = maxAllowedFramesForFps(fps);
  if (value === null || value === undefined || !Number.isFinite(value)) {
    return Math.min(preset, maxAllowed);
  }
  return Math.min(maxAllowed, Math.max(1, Math.round(value)));
}

function frameAgeMsFromFrames(fps: number, frames: number): number {
  const safeFps = Number.isFinite(fps) && fps > 0 ? fps : 60;
  return Math.min(maxFrameAgeMs, Math.max(minFrameAgeMs, Math.round((1000 / safeFps) * frames)));
}

/// Stream settings persist per browser.
///
/// Resolution, fps, codec, bitrate and the rest used to reset on every visit, so
/// anyone who does not stream at the defaults re-entered the same five choices each
/// time — and had no way to see what they last used while adjusting. Saved per
/// browser rather than on the host: two people driving the same host from different
/// machines want their own answers, and the host already has its own defaults.
///
/// `encoding` is deliberately absent. It is derived from codecChoice, and storing
/// both invites them to disagree. `muteHostAudio` is absent too: it decides whether
/// the game comes out of the speakers of a PC that is usually in another room, so
/// the safe state is re-chosen each visit.
const STREAM_SETTINGS_KEY = 'sunshine.webrtc.session_config';
/// Where this page kept its settings before sharing upstream's key; read once as a
/// fallback so an existing browser keeps its choices.
const LEGACY_STREAM_SETTINGS_KEY = 'vibepollo.browser-stream.settings';

interface SavedStreamSettings {
  appId: string;
  autoFullscreen: boolean;
  bitrateKbps: number;
  bitrateTracksRecommendation: boolean;
  codecChoice: EncodingType | 'auto';
  enhance: boolean;
  fps: number;
  hdr: boolean;
  height: number;
  videoMaxFrameAgeFrames: number;
  videoPacingMode: PacingMode;
  videoPacingSlackMs: number;
  volume: number;
  width: number;
}

function defaultStreamSettings(): SavedStreamSettings {
  return {
    appId: '',
    autoFullscreen: true,
    bitrateKbps: 20_000,
    bitrateTracksRecommendation: true,
    codecChoice: 'auto',
    enhance: false,
    fps: 60,
    hdr: false,
    height: 1080,
    videoMaxFrameAgeFrames: pacingPresets.balanced.maxAgeFrames,
    videoPacingMode: 'balanced',
    videoPacingSlackMs: pacingPresets.balanced.slackMs,
    volume: 100,
    width: 1920,
  };
}

function isPacingMode(value: unknown): value is PacingMode {
  return value === 'latency' || value === 'balanced' || value === 'smoothness';
}

/// Every field is validated individually against the default. Storage is shared
/// across host versions and browsers, so a value saved by an older build — or edited
/// by hand — must never be able to launch a stream at 0x0 or a codec that does not
/// exist. Codec *availability* is not checked here: capabilities have not loaded yet
/// at module init, so that clamp happens in refresh().
function loadStreamSettings(): SavedStreamSettings {
  const fallback = defaultStreamSettings();
  try {
    const raw =
      window.localStorage.getItem(STREAM_SETTINGS_KEY) ??
      window.localStorage.getItem(LEGACY_STREAM_SETTINGS_KEY);
    if (!raw) return fallback;
    const saved = JSON.parse(raw) as Record<string, unknown> | null;
    if (!saved || typeof saved !== 'object') return fallback;
    const positive = (value: unknown, fromDefault: number): number =>
      Number.isFinite(Number(value)) && Number(value) > 0 ? Number(value) : fromDefault;
    const flag = (value: unknown, fromDefault: boolean): boolean =>
      typeof value === 'boolean' ? value : fromDefault;
    const fps = positive(saved.fps, fallback.fps);
    // `pacingMode` is the legacy key's name for the same choice.
    const savedMode = saved.videoPacingMode ?? saved.pacingMode;
    const videoPacingMode = isPacingMode(savedMode) ? savedMode : fallback.videoPacingMode;
    const frameCount =
      typeof saved.videoMaxFrameAgeFrames === 'number'
        ? saved.videoMaxFrameAgeFrames
        : typeof saved.videoMaxFrameAgeMs === 'number'
          ? Math.round((saved.videoMaxFrameAgeMs / 1000) * fps)
          : undefined;
    const codecChoice = saved.codecChoice;
    return {
      appId: typeof saved.appId === 'string' ? saved.appId : fallback.appId,
      autoFullscreen: flag(saved.autoFullscreen, fallback.autoFullscreen),
      // 0 is meaningful — it is "let the host decide" — so it cannot use `positive`.
      bitrateKbps:
        Number.isFinite(Number(saved.bitrateKbps)) && Number(saved.bitrateKbps) >= 0
          ? Number(saved.bitrateKbps)
          : fallback.bitrateKbps,
      bitrateTracksRecommendation: flag(
        saved.bitrateTracksRecommendation,
        fallback.bitrateTracksRecommendation,
      ),
      codecChoice:
        codecChoice === 'auto' || codecs.includes(codecChoice as EncodingType)
          ? (codecChoice as EncodingType | 'auto')
          : fallback.codecChoice,
      enhance: flag(saved.enhance, fallback.enhance),
      fps,
      hdr: flag(saved.hdr, fallback.hdr),
      height: positive(saved.height, fallback.height),
      videoMaxFrameAgeFrames: clampFrameAgeFrames(frameCount, fps, videoPacingMode),
      videoPacingMode,
      videoPacingSlackMs:
        typeof saved.videoPacingSlackMs === 'number' && Number.isFinite(saved.videoPacingSlackMs)
          ? Math.min(10, Math.max(0, Math.round(saved.videoPacingSlackMs)))
          : pacingPresets[videoPacingMode].slackMs,
      volume: Math.min(
        100,
        Math.max(
          0,
          Number.isFinite(Number(saved.volume)) ? Number(saved.volume) : fallback.volume,
        ),
      ),
      width: positive(saved.width, fallback.width),
    };
  } catch {
    // A private window, cleared site data, or a browser refusing storage entirely.
    // Falling back to defaults is always correct here; never let this break the page.
    return fallback;
  }
}

const savedStreamSettings = loadStreamSettings();

function persistStreamSettings(): void {
  const settings: SavedStreamSettings = {
    appId: form.appId,
    autoFullscreen: autoFullscreen.value,
    bitrateKbps: form.bitrateKbps,
    bitrateTracksRecommendation: bitrateTracksRecommendation.value,
    codecChoice: codecChoice.value,
    enhance: enhance.value,
    fps: form.fps,
    hdr: form.hdr,
    height: form.height,
    videoMaxFrameAgeFrames: form.videoMaxFrameAgeFrames,
    videoPacingMode: form.videoPacingMode,
    videoPacingSlackMs: form.videoPacingSlackMs,
    volume: form.volume,
    width: form.width,
  };
  try {
    window.localStorage.setItem(STREAM_SETTINGS_KEY, JSON.stringify(settings));
  } catch {
    // Storage full, or a browser that refuses it. Losing the preference is a far
    // better outcome than breaking the settings form.
  }
}

const appSearch = ref('');
const apps = ref<AppRecord[]>([]);
const browserCapabilities = ref<BrowserVideoCapabilities>({
  h264: { supported: false, hdr: false },
  hevc: { supported: false, hdr: false },
  av1: { supported: false, hdr: false },
});
const browserStats = ref<WebRtcStatsSnapshot>({});
const connectionState = ref<RTCPeerConnectionState | 'idle'>('idle');
const hostCapabilities = ref<WebRtcHostCapabilities>({ ...unavailableHostCapabilities });
const inputChannelState = ref<RTCDataChannelState>('closed');
const inputForwarding = ref(true);
const windowFocused = ref(true);
const showPerformanceOverlay = ref(false);
const fullscreenExitHoldActive = ref(false);
const installHelpOpen = ref(false);
const isConnecting = ref(false);
const loading = ref(true);
const nativeFullscreen = ref(false);
const nativeVideoFullscreen = ref(false);
const playbackBlocked = ref(false);
const pseudoFullscreen = ref(false);
const autoFullscreen = ref(savedStreamSettings.autoFullscreen);
const streamCollapsed = ref(false);
const refreshError = ref('');
const sessionActionError = ref('');
const sessionActionPending = ref(false);
const sessionStatus = ref<SessionStatus | null>(null);
const startAfterTerminate = ref(false);
const streamError = ref('');
const streamSurface = ref<HTMLElement | null>(null);
const standaloneWebApp = ref(false);
const terminateOpen = ref(false);
const audioEl = ref<HTMLAudioElement | null>(null);
const videoEl = ref<HTMLVideoElement | null>(null);
const failedAppCovers = ref(new Set<number>());
const pressedKeys = new Map<string, PressedKey>();
const pressedMouseButtons = new Map<number, PressedMouseButton>();
const touchPointerGestures = new Map<number, TouchPointerGesture>();
const fullscreenExitHoldMs = 3000;
const fullscreenExitSwipeThresholdPx = 120;
const videoLatencyResetCooldownMs = 4000;
const videoRenderDelayResetThresholdMs = 100;
const videoRenderDelaySustainMs = 900;
let audioPlaybackStream: MediaStream | null = null;
let fullscreenExitEscapePressed = false;
let fullscreenExitHoldTimer: number | undefined;
let fullscreenExitSwipe: { pointerId: number; startX: number; startY: number } | null = null;
let fullscreenKeyboardLockRequest = 0;
let pageOverflowBeforePseudoFullscreen: { body: string; root: string } | null = null;
let sessionStatusTimer: number | undefined;
let videoBufferOverloadedSince: number | null = null;
let videoFrameCallbackHandle: number | undefined;
let videoRenderOverloadedSince: number | null = null;
let videoLatencyResetAt: number | null = null;
let videoPlaybackStream: MediaStream | null = null;
/// Video and audio together, for the corner player (see activeStream.publish).
let publishedStream: MediaStream | null = null;

/// Connection test.
///
/// Answers the question the stats overlay can only answer once a stream is
/// already running: which route would this connection actually take? It talks to
/// the ICE servers only — no session, and so no encoder. That restraint is the
/// point: a session whose signalling stalls leaves an encoder running against a
/// queue nobody drains, which is what took the host down on 2026-09-01. A
/// reachability check must never be able to do that.
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
    connError.value = messageFromError(err, t('ui.browser_stream.conn.failed', 'Connection test failed'));
  } finally {
    connTesting.value = false;
  }
}

/// Phrased as what to do about it, not as what was measured — "relayed via the
/// TURN server" is actionable, "3 srflx candidates" is not.
const connSummary = computed(() => {
  const r = connReport.value;
  if (!r) return '';
  switch (r.verdict) {
    case 'good':
      return t('ui.browser_stream.conn.good', 'Direct connection available. STUN and TURN both reachable.');
    case 'relay-only':
      return t(
        'ui.browser_stream.conn.relay_only',
        'No local network candidates — this browser is hiding them, often a VPN or privacy extension. Traffic will be relayed.',
      );
    case 'degraded':
      return t(
        'ui.browser_stream.conn.degraded',
        'No TURN relay available. Anyone who cannot reach this host directly will fail to connect.',
      );
    default:
      return t(
        'ui.browser_stream.conn.blocked',
        'Neither STUN nor TURN answered. This network is blocking the connection.',
      );
  }
});

const connTone = computed<StatusTone>(() => {
  switch (connReport.value?.verdict) {
    case 'good':
      return 'success';
    case 'relay-only':
      return 'info';
    case 'degraded':
      return 'warning';
    case 'blocked':
      return 'danger';
    default:
      return 'neutral';
  }
});

/// Client-side FSR (EASU upscale + RCAS sharpen) between the decoded frame and the
/// screen, the same pipeline the guest page uses — shared from utils/webrtc so the
/// two surfaces cannot drift apart. It was guest-only at first, which repeated the
/// mistake rumble made: a feature added to one view and quietly missing from the
/// next. It pays off whenever the stream runs below the size it is displayed at,
/// costs nothing on the wire, and needs no reconnect.
const enhance = ref<boolean>(savedStreamSettings.enhance);
/// WebGL2 missing or the pipeline died: the checkbox greys out rather than lying.
const enhanceUnavailable = ref(false);
const fxCanvas = ref<HTMLCanvasElement>();
const noopDetach = (): void => undefined;
let detachUpscaler: () => void = noopDetach;

/// 'auto' is not an encoding the server understands — it is resolved to the best
/// codec both ends actually support, at the moment the stream starts, so the answer
/// reflects the adapter and browser in play rather than one chosen weeks ago.
const codecChoice = ref<EncodingType | 'auto'>(savedStreamSettings.codecChoice);

/// Named resolutions, because typing 2560 and 1440 into two boxes is a worse way to
/// say "1440p". Custom keeps the boxes for anything not on the list — an ultrawide,
/// or matching an unusual desktop exactly.
const RESOLUTIONS: Array<{ label: string; width: number; height: number }> = [
  { label: '720p', width: 1280, height: 720 },
  { label: '1080p', width: 1920, height: 1080 },
  { label: '1440p', width: 2560, height: 1440 },
  { label: '4K', width: 3840, height: 2160 },
];
const resolutionChoice = ref<string>('1080p');

/// The same overlay guests get. It publishes a Gamepad-shaped object through
/// setVirtualGamepad; the gamepad capture reads pads through input's getGamepads,
/// which seats that object beside the hardware, so the host sees it as one more pad.
const isTouchDevice =
  typeof window !== 'undefined' && (navigator.maxTouchPoints > 0 || 'ontouchstart' in window);
const showTouchPad = ref(false);

/// A small "Controller connected" chip so the owner can see, at a glance, whether
/// the gamepad they grabbed is actually talking to the browser. The gamepad capture
/// already reads it (see syncGamepadCapture), but on its own the page has no way to
/// show that — and "the controls did nothing" is the first thing the owner asks.
/// Polled because gamepadconnect / gamepaddisconnect events miss a controller that
/// is already paired when the page loads, and the count drops on the next disconnect
/// whether or not the event fires on this tab.
const connectedGamepads = ref(0);
let gamepadPollTimer: number | undefined;
function countGamepads(): number {
  if (typeof navigator === 'undefined' || typeof navigator.getGamepads !== 'function') return 0;
  return navigator.getGamepads().filter((pad) => pad !== null).length;
}
function refreshGamepadCount(): void {
  connectedGamepads.value = countGamepads();
}
function startGamepadPolling(): void {
  if (gamepadPollTimer !== undefined) return;
  refreshGamepadCount();
  gamepadPollTimer = window.setInterval(refreshGamepadCount, 1000);
}
function stopGamepadPolling(): void {
  if (gamepadPollTimer === undefined) return;
  window.clearInterval(gamepadPollTimer);
  gamepadPollTimer = undefined;
}
function onGamepadConnectionChange(): void {
  refreshGamepadCount();
}

/// Same numbers the guest page shows, for the same reason: "it looked bad" cannot
/// tell a slow link from a lossy one from a host that cannot keep up.
/// Publishes the running stream so the corner player can show it while the owner
/// is on another page. Publishing the video-only stream, not the raw one: the
/// audio stays on this page's own element, and two elements playing it would echo.
const activeStream = useActiveStream();

const showStats = ref(false);
const streamStats = ref<{
  bitrateKbps?: number;
  fps?: number;
  packetsLost?: number;
  framesDropped?: number;
  roundTripMs?: number;
  lossPercent?: number;
  jitterMs?: number;
  jitterBufferMs?: number;
  codec?: string;
  path?: string;
  remoteAddress?: string;
}>({});

function onResolutionChanged(): void {
  const preset = RESOLUTIONS.find((r) => r.label === resolutionChoice.value);
  if (!preset) return;
  form.width = preset.width;
  form.height = preset.height;
}

/// Matches the current width and height back to a preset name, so a session started
/// at 1920x1080 shows "1080p" rather than falling to Custom.
function syncResolutionChoice(): void {
  const match = RESOLUTIONS.find((r) => r.width === form.width && r.height === form.height);
  resolutionChoice.value = match ? match.label : 'custom';
}

const FRAME_RATES = [30, 60, 90, 120, 144];
const fpsChoice = ref<string>('60');

function onFpsChanged(): void {
  if (fpsChoice.value === 'custom') return;
  form.fps = Number(fpsChoice.value);
}

function syncFpsChoice(): void {
  fpsChoice.value = FRAME_RATES.includes(form.fps) ? String(form.fps) : 'custom';
}

/// 0 means "let the host decide", which a slider cannot express, so it is a separate
/// switch and the slider is disabled while it is on. Turning it back off returns to
/// the recommended value (and re-arms auto-tracking) rather than a hardcoded 20 Mbps.
/// The referenced helpers are declared further down; the setter only runs on user
/// interaction, long after module init.
const useHostBitrate = computed({
  get: () => form.bitrateKbps === 0,
  set: (on: boolean) => {
    if (on) {
      form.bitrateKbps = 0;
      bitrateTracksRecommendation.value = false;
      return;
    }
    const recommended = recommendedBitrateKbps();
    form.bitrateKbps = recommended > 0 ? recommended : 20_000;
    bitrateTracksRecommendation.value = recommended > 0;
  },
});

/// Where the bitrate handle sits within the host's allowed range, as a percentage,
/// for the filled part of the track. The bounds come from the host and the low one
/// is rarely 0, so the raw value is not a percentage of anything useful on its own.
const bitrateFillPercent = computed(() => {
  const min = Math.max(hostCapabilities.value.limits.min_bitrate_kbps, 1000);
  const max = hostCapabilities.value.limits.max_bitrate_kbps || 150_000;
  if (max <= min) return 0;
  const clamped = Math.min(max, Math.max(min, form.bitrateKbps));
  return ((clamped - min) / (max - min)) * 100;
});

const bitrateLabel = computed(() =>
  form.bitrateKbps === 0
    ? t('ui.browser_stream.settings.bitrate_host_default', 'Host default')
    : `${(form.bitrateKbps / 1000).toFixed(form.bitrateKbps % 1000 ? 1 : 0)} Mbps`,
);

/// Volume is stored as 0-100 in the form so the slider has predictable integer
/// steps. The audio element wants 0.0-1.0, so convert at the edge. Anything
/// outside the range gets clamped; HTMLMediaElement.volume silently does that,
/// but the slider would still let the user scrub past 1 if v-model.number fed
/// back a non-finite value.
function clampVolume(value: number): number {
  if (!Number.isFinite(value)) return 1;
  const clamped = Math.min(100, Math.max(0, value));
  return clamped / 100;
}

/// Milliseconds, or an em dash when the browser has not reported the figure.
/// Distinguishing "not measured yet" from "measured as zero" matters most for RTT,
/// where zero is not a value any real connection takes.
function statMs(value: number | undefined): string {
  return typeof value === 'number' && Number.isFinite(value) ? `${Math.round(value)} ms` : '—';
}

const volumeLabel = computed(() => `${Math.round(form.volume)}%`);

/// Newest first: at a given bitrate AV1 and HEVC hold detail that H.264 loses, and
/// the difference is largest exactly where it matters, on a constrained link.
function bestAvailableCodec(): EncodingType {
  const preference: EncodingType[] = ['av1', 'hevc', 'h264'];
  return preference.find((codec) => codecAvailable(codec)) ?? 'h264';
}
let gamepadCapture: BrowserGamepadCapture | null = null;
let autoFullscreenAttempt = 0;
let preserveAutoFullscreenDuringRestart = false;
let streamSurfaceFocusedBeforeCollapse = false;

const form = reactive<StreamLaunchForm>({
  appId: savedStreamSettings.appId,
  bitrateKbps: savedStreamSettings.bitrateKbps,
  // Derived from codecChoice in refresh(), once both ends have reported support.
  encoding: 'h264',
  fps: savedStreamSettings.fps,
  hdr: savedStreamSettings.hdr,
  height: savedStreamSettings.height,
  // Deliberately not restored. This decides whether the game comes out of the
  // speakers of a PC that is usually in another room, so the safe state has to be
  // re-chosen each visit rather than inherited from whatever was set once weeks
  // ago. Persisting it made a formerly per-visit default sticky.
  muteHostAudio: true,
  videoMaxFrameAgeFrames: savedStreamSettings.videoMaxFrameAgeFrames,
  videoPacingMode: savedStreamSettings.videoPacingMode,
  videoPacingSlackMs: savedStreamSettings.videoPacingSlackMs,
  volume: savedStreamSettings.volume,
  width: savedStreamSettings.width,
});

/// Moonlight's default-bitrate curve: Mbps at 30 fps for H.264, interpolated
/// linearly on pixel count between these anchors (1080p60 lands on the familiar
/// 20 Mbps default). Game streaming needs these rates — conferencing-style
/// bits-per-pixel constants recommend a quarter of this and smear in motion.
const BITRATE_ANCHORS: ReadonlyArray<readonly [pixels: number, mbpsAt30: number]> = [
  [640 * 360, 1],
  [854 * 480, 2],
  [1280 * 720, 5],
  [1920 * 1080, 10],
  [2560 * 1440, 20],
  [3840 * 2160, 40],
];

/// HEVC and AV1 hold the same quality at a lower rate, but the discount in
/// practice is nowhere near the 40-60% marketing figure at streaming latency.
const CODEC_BITRATE_SCALE: Record<EncodingType, number> = {
  h264: 1,
  hevc: 0.75,
  av1: 0.6,
};

/// Host-defined lower and upper bounds, used to clamp the recommendation so the
/// form never lands outside what the encoder actually accepts.
function bitrateBounds(): { min: number; max: number } {
  return {
    min: hostCapabilities.value.limits.min_bitrate_kbps,
    max: hostCapabilities.value.limits.max_bitrate_kbps,
  };
}

function roundToStep(value: number, step: number): number {
  return Math.round(value / step) * step;
}

/// What bitrate makes sense for the current resolution, fps, and codec. The
/// slider step is 1000 kbps, so the recommendation snaps to the nearest 1000
/// before clamping to the host's range. Returns 0 (host default) only if the
/// form is in custom / unset territory and the formula is undefined.
function recommendedBitrateKbps(): number {
  const pixels = form.width * form.height;
  if (!Number.isFinite(pixels) || pixels <= 0) return 0;
  if (!Number.isFinite(form.fps) || form.fps <= 0) return 0;

  // Interpolate the anchor table on pixel count; extrapolate proportionally
  // past either end so odd resolutions still land somewhere sensible.
  const first = BITRATE_ANCHORS[0];
  const last = BITRATE_ANCHORS[BITRATE_ANCHORS.length - 1];
  let mbpsAt30: number;
  if (pixels <= first[0]) {
    mbpsAt30 = (first[1] * pixels) / first[0];
  } else if (pixels >= last[0]) {
    mbpsAt30 = (last[1] * pixels) / last[0];
  } else {
    mbpsAt30 = last[1];
    for (let i = 1; i < BITRATE_ANCHORS.length; i++) {
      const [hiPx, hiMbps] = BITRATE_ANCHORS[i];
      if (pixels <= hiPx) {
        const [loPx, loMbps] = BITRATE_ANCHORS[i - 1];
        mbpsAt30 = loMbps + ((hiMbps - loMbps) * (pixels - loPx)) / (hiPx - loPx);
        break;
      }
    }
  }

  // Same shape moonlight uses: linear in fps up to 60, then square-root growth —
  // doubling the frame rate does not double the information in the scene.
  const fpsFactor = (form.fps <= 60 ? form.fps : Math.sqrt(form.fps / 60) * 60) / 30;
  const scale = CODEC_BITRATE_SCALE[form.encoding] ?? CODEC_BITRATE_SCALE.h264;
  const rawKbps = mbpsAt30 * fpsFactor * scale * 1000;
  const { min, max } = bitrateBounds();
  return Math.min(max, Math.max(min, roundToStep(rawKbps, 1000)));
}

/// Tracks whether the slider follows the recommended value. On for a browser that
/// has never streamed here, so a first visit starts on the recommendation and
/// follows it through resolution / fps / codec changes. Dragging the slider takes
/// the wheel and turns this off; "Use recommended" hands it back. Persisted with
/// the rest of the settings — restoring a hand-picked bitrate without also
/// restoring that the user picked it would let the next capabilities refresh
/// overwrite their choice.
const bitrateTracksRecommendation = ref(savedStreamSettings.bitrateTracksRecommendation);

function applyRecommendedBitrate(): void {
  const next = recommendedBitrateKbps();
  if (next > 0) {
    form.bitrateKbps = next;
    bitrateTracksRecommendation.value = true;
  }
}

/// Dragging the slider off the recommendation means the user is taking the
/// wheel, so stop auto-syncing until they ask again. A new resolution can
/// re-trigger the "Use recommended" hint without re-engaging auto-tracking.
watch(
  () => form.bitrateKbps,
  (value) => {
    if (value !== recommendedBitrateKbps()) bitrateTracksRecommendation.value = false;
  },
);

/// hostCapabilities is in the dependency list because the recommendation is
/// clamped to the host's limits: before the capabilities response lands the
/// defaults apply, and once it does the recommendation has to be re-clamped to
/// the real ones. The wholesale `hostCapabilities.value = ...` assignment on
/// refresh triggers this watcher, which is what snaps the slider to the
/// recommendation on page load.
watch(
  () => [form.width, form.height, form.fps, form.encoding, hostCapabilities.value] as const,
  () => {
    if (bitrateTracksRecommendation.value && !useHostBitrate.value) applyRecommendedBitrate();
  },
);

/// True when the slider currently matches the recommendation, so the template
/// can show the right copy. Recomputed each render.
const bitrateMatchesRecommendation = computed(() => {
  if (form.bitrateKbps === 0) return false;
  return form.bitrateKbps === recommendedBitrateKbps();
});

function unavailableCapabilities(reason: string): WebRtcHostCapabilities {
  return {
    ...unavailableHostCapabilities,
    availability: { state: 'unavailable', reason },
    codecs: {
      h264: { supported: false, hdr: false },
      hevc: { supported: false, hdr: false },
      av1: { supported: false, hdr: false },
    },
  };
}

function appIdFor(app: AppRecord): number | null {
  // The host resolves the app by its OWN id, not by where it happens to sit in the
  // list. Sending the index meant every launch asked for an app id that matched
  // nothing, and came back "Cannot find requested application". /api/apps sends
  // both fields; index is only useful for ordering.
  const raw = (app as Record<string, unknown>)['id'] ?? (app as Record<string, unknown>)['index'];
  const id = Number(raw);
  return Number.isInteger(id) && id > 0 ? id : null;
}

const launchableApps = computed<LaunchableApp[]>(() =>
  apps.value.flatMap((app) => {
    const id = appIdFor(app);
    if (id === null) return [];
    return [
      {
        coverUrl: appCoverUrl(app),
        id,
        name: appName(app) || t('ui.browser_stream.unnamed_application'),
      },
    ];
  }),
);

const filteredLaunchableApps = computed(() => {
  const query = appSearch.value.trim().toLocaleLowerCase();
  if (!query) return launchableApps.value;
  return launchableApps.value.filter((app) => app.name.toLocaleLowerCase().includes(query));
});

const selectedAppId = computed(() => {
  const id = Number(form.appId);
  return Number.isInteger(id) && id > 0 ? id : undefined;
});

const selectedAppName = computed(() => {
  const selected = launchableApps.value.find((app) => app.id === selectedAppId.value);
  if (selected?.name) return selected.name;
  if (sessionStatus.value?.appName && hasRunningSession.value) return sessionStatus.value.appName;
  return t('ui.browser_stream.desktop');
});

const hasRunningSession = computed(
  () =>
    Boolean(sessionStatus.value?.appRunning) ||
    Number(sessionStatus.value?.activeSessions ?? 0) > 0,
);

const resumeAvailable = computed(
  () =>
    selectedAppId.value === undefined &&
    (Number(sessionStatus.value?.activeSessions ?? 0) > 0 || sessionStatus.value?.paused === true),
);

const primaryActionLabel = computed(() =>
  resumeAvailable.value ? t('webrtc.resume') : t('ui.browser_stream.start'),
);

const terminateDescription = computed(() =>
  startAfterTerminate.value
    ? t('webrtc.terminate_confirm_message', {
        app: selectedAppName.value || t('webrtc.terminate_confirm_app_fallback'),
      })
    : t('webrtc.terminate_desc'),
);

const terminateConfirmLabel = computed(() =>
  startAfterTerminate.value ? t('webrtc.terminate_confirm_action') : t('webrtc.terminate'),
);

const fullscreenActive = computed(
  () => nativeFullscreen.value || nativeVideoFullscreen.value || pseudoFullscreen.value,
);

const fullscreenExitControlLabel = computed(() =>
  fullscreenExitHoldActive.value
    ? t('ui.browser_stream.exit_fullscreen_cancel_hint')
    : t('ui.browser_stream.exit_fullscreen'),
);

const showFullscreenSwipeExit = computed(() => fullscreenActive.value && isTouchSafariBrowser());

const showInstallWebAppAction = computed(() => isTouchSafariBrowser() && !standaloneWebApp.value);

function selectApp(appId?: number): void {
  form.appId = appId === undefined ? '' : String(appId);
}

function appSelected(appId?: number): boolean {
  return selectedAppId.value === appId;
}

function appCoverFailed(appId: number): boolean {
  return failedAppCovers.value.has(appId);
}

function markAppCoverFailed(appId: number): void {
  failedAppCovers.value = new Set(failedAppCovers.value).add(appId);
}

const hostReady = computed(
  () => hostCapabilities.value.enabled && hostCapabilities.value.availability.state === 'ready',
);
const hdrForcedOn = computed(() => hostCapabilities.value.hdr_policy === 'force_on');
const hdrForcedOff = computed(() => hostCapabilities.value.hdr_policy === 'force_off');
const effectiveHdr = computed(() =>
  hdrForcedOn.value ? true : hdrForcedOff.value ? false : form.hdr,
);
const isConnected = computed(() => connectionState.value === 'connected');

/// The stats overlay answers "is this link any good" only for someone who already
/// reads rtt and jitter. This is the same figures turned into a verdict, so the
/// answer is available without switching the overlay on and without knowing what
/// the numbers mean.
const linkQuality = computed(() =>
  assessLinkQuality({
    roundTripMs: streamStats.value.roundTripMs,
    jitterMs: streamStats.value.jitterMs,
    lossPercent: streamStats.value.lossPercent,
    relayed: streamStats.value.path === 'relayed',
  }),
);

const upscalerActive = computed(
  () => enhance.value && !enhanceUnavailable.value && isConnected.value,
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

// flush: 'post' so the canvas is in the DOM before an attach is attempted.
watch(upscalerActive, () => syncUpscaler(), { flush: 'post' });

const connectionPending = computed(
  () =>
    isConnecting.value || connectionState.value === 'new' || connectionState.value === 'connecting',
);
const inputReady = computed(
  () =>
    !streamCollapsed.value &&
    isConnected.value &&
    inputForwarding.value &&
    inputChannelState.value === 'open',
);

const inputStatusLabel = computed(() => {
  if (streamCollapsed.value) return t('ui.browser_stream.input_paused');
  return inputReady.value
    ? t('ui.browser_stream.input_ready')
    : t('ui.browser_stream.input_unavailable');
});

const connectionLabel = computed(() => {
  if (isConnected.value) return t('ui.browser_stream.status.connected');
  if (connectionPending.value) return t('ui.browser_stream.status.connecting');
  if (connectionState.value === 'failed') return t('ui.browser_stream.status.failed');
  if (connectionState.value === 'disconnected' || connectionState.value === 'closed') {
    return t('ui.browser_stream.status.disconnected');
  }
  return t('ui.browser_stream.status.ready');
});

const connectionTone = computed<StatusTone>(() => {
  if (isConnected.value) return 'success';
  if (connectionPending.value) return 'info';
  if (connectionState.value === 'failed') return 'danger';
  if (!hostReady.value) return 'warning';
  return 'neutral';
});

function codecLabel(codec: EncodingType): string {
  return t(`ui.browser_stream.codecs.${codec}`);
}

const pacingOptions = computed(() => [
  { label: t('webrtc.pacing_latency'), value: 'latency' as const },
  { label: t('webrtc.pacing_balanced'), value: 'balanced' as const },
  { label: t('webrtc.pacing_smooth'), value: 'smoothness' as const },
]);

const maxFrameAgeFrames = computed({
  get: () => clampFrameAgeFrames(form.videoMaxFrameAgeFrames, form.fps, form.videoPacingMode),
  set: (value: number) => {
    if (!Number.isFinite(value)) return;
    form.videoMaxFrameAgeFrames = clampFrameAgeFrames(value, form.fps, form.videoPacingMode);
  },
});

const maxFrameAgeMsLabel = computed(() => frameAgeMsFromFrames(form.fps, maxFrameAgeFrames.value));

function applyPacingPreset(mode: PacingMode): void {
  const preset = pacingPresets[mode];
  form.videoPacingMode = mode;
  form.videoPacingSlackMs = preset.slackMs;
  form.videoMaxFrameAgeFrames = clampFrameAgeFrames(preset.maxAgeFrames, form.fps, mode);
}

function baseCodecAvailable(codec: EncodingType): boolean {
  return (
    hostReady.value &&
    hostCapabilities.value.codecs[codec].supported &&
    browserCapabilities.value[codec].supported
  );
}

function hdrAvailable(codec: EncodingType): boolean {
  return (
    baseCodecAvailable(codec) &&
    hostCapabilities.value.hdr_policy_allows &&
    hostCapabilities.value.codecs[codec].hdr &&
    browserCapabilities.value[codec].hdr
  );
}

function codecAvailable(codec: EncodingType): boolean {
  return baseCodecAvailable(codec) && (!hdrForcedOn.value || hdrAvailable(codec));
}

function codecUnavailableReason(codec: EncodingType): string {
  if (!hostCapabilities.value.enabled) {
    return (
      hostCapabilities.value.availability.reason || t('ui.browser_stream.reasons.host_disabled')
    );
  }
  if (!hostReady.value) {
    return (
      hostCapabilities.value.availability.reason || t('ui.browser_stream.reasons.host_unverified')
    );
  }
  if (!hostCapabilities.value.codecs[codec].supported) {
    return t('ui.browser_stream.reasons.host_codec_unavailable', { codec: codecLabel(codec) });
  }
  if (!browserCapabilities.value[codec].supported) {
    return t('ui.browser_stream.reasons.browser_codec_unavailable', { codec: codecLabel(codec) });
  }
  if (hdrForcedOn.value && !hdrAvailable(codec)) {
    return t('ui.browser_stream.reasons.hdr_required');
  }
  return '';
}

const hdrUnavailableReason = computed(() => {
  if (!codecAvailable(form.encoding)) return codecUnavailableReason(form.encoding);
  if (!hostCapabilities.value.hdr_policy_allows)
    return t('ui.browser_stream.reasons.hdr_policy_disabled');
  if (!hostCapabilities.value.codecs[form.encoding].hdr) {
    return t('ui.browser_stream.reasons.host_hdr_unavailable', {
      codec: codecLabel(form.encoding),
    });
  }
  if (!browserCapabilities.value[form.encoding].hdr) {
    return t('ui.browser_stream.reasons.browser_hdr_unavailable', {
      codec: codecLabel(form.encoding),
    });
  }
  return '';
});

const hdrControlDisabled = computed(
  () => hdrForcedOn.value || hdrForcedOff.value || !hdrAvailable(form.encoding),
);

const hdrControlDescription = computed(() => {
  if (hdrForcedOn.value) return t('ui.browser_stream.settings.hdr_forced_on');
  if (hdrForcedOff.value) return t('ui.browser_stream.settings.hdr_forced_off');
  return hdrAvailable(form.encoding)
    ? t('ui.browser_stream.settings.hdr_help')
    : hdrUnavailableReason.value;
});

const validationError = computed(() => {
  if (!hostReady.value) {
    return (
      hostCapabilities.value.availability.reason || t('ui.browser_stream.reasons.host_unverified')
    );
  }
  if (!codecAvailable(form.encoding)) return codecUnavailableReason(form.encoding);
  if (effectiveHdr.value && !hdrAvailable(form.encoding)) return hdrUnavailableReason.value;

  const limits = hostCapabilities.value.limits;
  const dimensions = [form.width, form.height];
  if (
    dimensions.some(
      (value) =>
        !Number.isInteger(value) ||
        value < limits.min_dimension ||
        value > limits.max_dimension ||
        value % 2 !== 0,
    )
  ) {
    return t('ui.browser_stream.reasons.invalid_dimensions', {
      min: limits.min_dimension,
      max: limits.max_dimension,
    });
  }
  if (!Number.isInteger(form.fps) || form.fps < limits.min_fps || form.fps > limits.max_fps) {
    return t('ui.browser_stream.reasons.invalid_fps', {
      min: limits.min_fps,
      max: limits.max_fps,
    });
  }
  if (
    !Number.isInteger(form.bitrateKbps) ||
    form.bitrateKbps < limits.min_bitrate_kbps ||
    form.bitrateKbps > limits.max_bitrate_kbps
  ) {
    return t('ui.browser_stream.reasons.invalid_bitrate', {
      min: limits.min_bitrate_kbps,
      max: limits.max_bitrate_kbps,
    });
  }
  if (!['latency', 'balanced', 'smoothness'].includes(form.videoPacingMode)) {
    return t('ui.browser_stream.reasons.invalid_pacing_mode');
  }
  if (
    !Number.isInteger(form.videoPacingSlackMs) ||
    form.videoPacingSlackMs < 0 ||
    form.videoPacingSlackMs > 10
  ) {
    return t('ui.browser_stream.reasons.invalid_pacing_slack', { min: 0, max: 10 });
  }
  if (
    !Number.isInteger(form.videoMaxFrameAgeFrames) ||
    form.videoMaxFrameAgeFrames < 1 ||
    form.videoMaxFrameAgeFrames > maxAllowedFramesForFps(form.fps)
  ) {
    return t('ui.browser_stream.reasons.invalid_frame_age', {
      min: 1,
      max: maxAllowedFramesForFps(form.fps),
    });
  }
  return '';
});

const startDisabled = computed(
  () =>
    loading.value ||
    connectionPending.value ||
    isConnected.value ||
    sessionActionPending.value ||
    Boolean(validationError.value),
);

function messageFromError(error: unknown, fallback: string): string {
  return error instanceof Error && error.message ? error.message : fallback;
}

function stopSessionStatusPolling(): void {
  if (sessionStatusTimer === undefined) return;
  window.clearInterval(sessionStatusTimer);
  sessionStatusTimer = undefined;
}

async function fetchSessionStatus(): Promise<void> {
  if (connectionPending.value || isConnected.value) return;
  try {
    const status = await apiGet<SessionStatus>('/api/session/status');
    sessionStatus.value = status.status ? status : null;
  } catch {
    sessionStatus.value = null;
  }
}

function startSessionStatusPolling(): void {
  stopSessionStatusPolling();
  if (connectionPending.value || isConnected.value) return;
  void fetchSessionStatus();
  sessionStatusTimer = window.setInterval(fetchSessionStatus, 5000);
}

async function refresh(): Promise<void> {
  if (loading.value && apps.value.length) return;
  loading.value = true;
  refreshError.value = '';

  const [hostResult, browserResult, appResult] = await Promise.allSettled([
    fetchWebRtcHostCapabilities(),
    detectBrowserVideoCapabilities(),
    fetchApps(),
  ]);

  if (hostResult.status === 'fulfilled') {
    hostCapabilities.value = hostResult.value;
  } else {
    hostCapabilities.value = unavailableCapabilities(
      messageFromError(hostResult.reason, t('ui.browser_stream.reasons.host_unavailable')),
    );
  }
  if (browserResult.status === 'fulfilled') {
    browserCapabilities.value = browserResult.value;
  } else {
    browserCapabilities.value = {
      h264: { supported: false, hdr: false },
      hevc: { supported: false, hdr: false },
      av1: { supported: false, hdr: false },
    };
  }
  if (appResult.status === 'fulfilled') {
    apps.value = appResult.value;
  } else {
    refreshError.value = messageFromError(
      appResult.reason,
      t('ui.browser_stream.errors.load_apps'),
    );
  }

  // Now that both ends have reported what they support, 'auto' can mean something.
  // Resolving earlier would settle on the h264 fallback, since nothing is known to
  // be available before this point.
  syncResolutionChoice();
  syncFpsChoice();
  // A restored app may have been deleted since. '' is the desktop, which always
  // exists, so it is the safe landing place rather than a launch that 404s.
  if (form.appId && !apps.value.some((app) => String(app.id) === form.appId)) {
    form.appId = '';
  }
  // A codec saved on another machine, or one this browser has since lost, must not
  // strand the page on an encoding neither end can do.
  if (codecChoice.value !== 'auto' && !codecAvailable(codecChoice.value)) {
    codecChoice.value = 'auto';
  }
  // Unconditional now: a restored explicit codec still has to reach form.encoding,
  // which only ever holds the module-init default until this runs.
  applyCodecChoice();
  if (form.hdr && !hdrAvailable(form.encoding)) form.hdr = false;

  if (browserResult.status === 'rejected' && !refreshError.value) {
    refreshError.value = t('ui.browser_stream.errors.inspect_browser');
  }
  await fetchSessionStatus();
  loading.value = false;
}

/// Keeps form.encoding — the value actually sent — in step with the dropdown, and
/// re-resolves it when 'auto' is selected.
function applyCodecChoice(): void {
  form.encoding = codecChoice.value === 'auto' ? bestAvailableCodec() : codecChoice.value;
}

function onCodecChoiceChanged(): void {
  applyCodecChoice();
  onEncodingChanged();
}

function onEncodingChanged(): void {
  if (form.hdr && !hdrAvailable(form.encoding)) form.hdr = false;
}

function setHdr(event: Event): void {
  form.hdr = (event.target as HTMLInputElement).checked;
}

function replaceTracks(
  current: MediaStream | null,
  tracks: MediaStreamTrack[],
): MediaStream | null {
  if (!tracks.length || typeof MediaStream !== 'function') return current;
  const target = current ?? new MediaStream();
  for (const kind of new Set(tracks.map((track) => track.kind))) {
    const incoming = tracks.filter((track) => track.kind === kind);
    const existing = target.getTracks().filter((track) => track.kind === kind);
    if (
      existing.length === incoming.length &&
      incoming.every((track) => existing.some((currentTrack) => currentTrack.id === track.id))
    ) {
      continue;
    }
    for (const track of existing) target.removeTrack(track);
    for (const track of incoming) target.addTrack(track);
  }
  return target;
}

async function playAttachedMedia(): Promise<void> {
  const attempts: Promise<void>[] = [];
  if (videoEl.value?.srcObject) attempts.push(videoEl.value.play());
  if (audioEl.value?.srcObject) attempts.push(audioEl.value.play());
  const results = await Promise.allSettled(attempts);
  playbackBlocked.value = results.some((result) => result.status === 'rejected');
}

function isSafariBrowser(): boolean {
  try {
    const userAgent = navigator.userAgent ?? '';
    const vendor = navigator.vendor ?? '';
    return (
      /\bsafari\//i.test(userAgent) &&
      /apple/i.test(vendor) &&
      !/\b(chrome|chromium|crios|fxios|edgios|edg|opr|opera)\b/i.test(userAgent)
    );
  } catch {
    return false;
  }
}

function isTouchSafariBrowser(): boolean {
  try {
    const userAgent = navigator.userAgent ?? '';
    const platform = navigator.platform ?? '';
    return (
      /apple/i.test(navigator.vendor ?? '') &&
      navigator.maxTouchPoints > 1 &&
      (/\b(iPad|iPhone|iPod)\b/i.test(userAgent) || /MacIntel/i.test(platform))
    );
  } catch {
    return false;
  }
}

function runningAsStandaloneWebApp(): boolean {
  try {
    return (
      (navigator as StandaloneNavigator).standalone === true ||
      window.matchMedia('(display-mode: standalone)').matches ||
      window.matchMedia('(display-mode: fullscreen)').matches
    );
  } catch {
    return false;
  }
}

function resetVideoLatencyFence(): void {
  videoBufferOverloadedSince = null;
  videoRenderOverloadedSince = null;
}

function resetVideoElementForLatency(): void {
  const player = videoEl.value;
  const stream = videoPlaybackStream;
  if (!player || !stream || !stream.getVideoTracks().length) return;

  resetVideoLatencyFence();
  videoLatencyResetAt = performance.now();
  browserSession.requestLatencyResync();
  try {
    player.pause();
    player.srcObject = null;
    player.load();
  } catch {
    // Reattaching on the next frame is still worth attempting.
  }
  window.requestAnimationFrame(() => {
    if (videoEl.value !== player || videoPlaybackStream !== stream || !isConnected.value) return;
    player.muted = true;
    player.playbackRate = 1;
    player.srcObject = stream;
    void player.play().catch(() => {
      playbackBlocked.value = true;
    });
  });
}

function applyVideoLatencyFence(
  delayMs: number | undefined,
  thresholdMs: number,
  sustainMs: number,
  source: 'buffer' | 'render',
): void {
  const overloadedSince =
    source === 'buffer' ? videoBufferOverloadedSince : videoRenderOverloadedSince;
  if (
    !isConnected.value ||
    document.visibilityState !== 'visible' ||
    typeof delayMs !== 'number' ||
    !Number.isFinite(delayMs) ||
    delayMs < thresholdMs
  ) {
    if (source === 'buffer') videoBufferOverloadedSince = null;
    else videoRenderOverloadedSince = null;
    return;
  }

  const now = performance.now();
  const startedAt = overloadedSince ?? now;
  if (source === 'buffer') videoBufferOverloadedSince = startedAt;
  else videoRenderOverloadedSince = startedAt;
  if (now - startedAt < sustainMs) return;
  if (videoLatencyResetAt !== null && now - videoLatencyResetAt < videoLatencyResetCooldownMs)
    return;
  resetVideoElementForLatency();
}

function handleVideoPlayoutDelay(delayMs: number | undefined): void {
  applyVideoLatencyFence(
    delayMs,
    isSafariBrowser() ? 160 : 220,
    isSafariBrowser() ? 1500 : 900,
    'buffer',
  );
}

function stopVideoFrameLatencyMonitoring(): void {
  const player = videoEl.value;
  if (player && videoFrameCallbackHandle !== undefined) {
    player.cancelVideoFrameCallback(videoFrameCallbackHandle);
  }
  videoFrameCallbackHandle = undefined;
}

function startVideoFrameLatencyMonitoring(player: HTMLVideoElement): void {
  stopVideoFrameLatencyMonitoring();
  if (typeof player.requestVideoFrameCallback !== 'function') return;
  const onFrame = (now: number, metadata: VideoFrameCallbackMetadata): void => {
    if (videoEl.value !== player) return;
    const expected = metadata.expectedDisplayTime;
    const delayMs = Number.isFinite(expected) ? Math.max(0, now - expected) : undefined;
    applyVideoLatencyFence(
      delayMs,
      videoRenderDelayResetThresholdMs,
      videoRenderDelaySustainMs,
      'render',
    );
    videoFrameCallbackHandle = player.requestVideoFrameCallback(onFrame);
  };
  videoFrameCallbackHandle = player.requestVideoFrameCallback(onFrame);
}

function attachRemoteStream(stream: MediaStream): void {
  const player = videoEl.value;
  if (!player) return;

  if (typeof MediaStream !== 'function') {
    player.muted = false;
    player.srcObject = stream;
    void playAttachedMedia();
    return;
  }

  const videoTracks = stream.getVideoTracks();
  if (videoTracks.length) {
    videoPlaybackStream = replaceTracks(videoPlaybackStream, videoTracks);
    player.muted = true;
    player.srcObject = videoPlaybackStream;
    startVideoFrameLatencyMonitoring(player);
  }

  const audioTracks = stream.getAudioTracks();
  if (audioTracks.length && audioEl.value) {
    audioPlaybackStream = replaceTracks(audioPlaybackStream, audioTracks);
    audioEl.value.srcObject = audioPlaybackStream;
    // Apply the current slider value to the element. A fresh srcObject reset
    // clears the volume to 1, so reapply here whenever a new track lands.
    audioEl.value.volume = clampVolume(form.volume);
  }

  // The corner player gets both kinds in one stream. This page's own elements
  // pause whenever KeepAlive lifts it out of the document, so while the owner is
  // elsewhere the mini player is the only thing that can carry the game's sound.
  if (videoTracks.length || audioTracks.length) {
    publishedStream = replaceTracks(publishedStream, [...videoTracks, ...audioTracks]);
    activeStream.setVolume(clampVolume(form.volume));
    activeStream.publish(publishedStream ?? undefined, selectedAppName.value);
  }

  void playAttachedMedia();
}

async function connect(resume: boolean): Promise<void> {
  if (startDisabled.value) {
    streamError.value = validationError.value || t('ui.browser_stream.errors.unavailable');
    return;
  }

  stopSessionStatusPolling();
  sessionActionError.value = '';
  streamError.value = '';
  playbackBlocked.value = false;
  isConnecting.value = true;
  connectionState.value = 'connecting';
  const config: StreamConfig = {
    appId: selectedAppId.value,
    audioChannels: 2,
    audioCodec: 'opus',
    bitrateKbps: form.bitrateKbps,
    encoding: form.encoding,
    fps: form.fps,
    hdr: effectiveHdr.value,
    height: form.height,
    muteHostAudio: form.muteHostAudio,
    resume,
    videoMaxFrameAgeFrames: form.videoMaxFrameAgeFrames,
    videoPacingMode: form.videoPacingMode,
    videoPacingSlackMs: form.videoPacingSlackMs,
    width: form.width,
  };

  try {
    browserStats.value = {};
    await browserSession.connect(config, {
      onConnectionState: (state) => {
        connectionState.value = state;
        if (state === 'connected') stopSessionStatusPolling();
        if (state === 'failed') {
          streamError.value = t('ui.browser_stream.errors.connection_failed');
        }
        if (state === 'failed' || state === 'disconnected' || state === 'closed') {
          cancelAutoFullscreen();
          // A peer can disappear without delivering a data-channel close
          // first. Clear browser-side pressed state and controller state on
          // the connection transition as well.
          releaseForwardedInput();
          stopGamepadCapture();
          startSessionStatusPolling();
        }
        if (state === 'failed' || state === 'closed') {
          // The corner player keys off the published stream. A connection that has
          // died for good must take it down, or it keeps showing the last frame with
          // "Back to stream" under it. 'disconnected' is left alone: it can recover,
          // and nothing would republish the stream if it did.
          activeStream.publish();
        }
      },
      onStreamStats: (snapshot) => {
        streamStats.value = snapshot;
      },
      onInputState: (state) => {
        if (state !== 'open') releaseForwardedInput();
        inputChannelState.value = state;
        syncGamepadCapture();
      },
      // Rumble on its way back from the host.
      onInputMessage: applyGamepadFeedback,
      onRemoteStream: attachRemoteStream,
      onStats: (stats) => {
        browserStats.value = stats;
      },
      onVideoPlayoutDelay: handleVideoPlayoutDelay,
    });
  } catch (error) {
    if (error instanceof WebRtcConnectionCanceledError) {
      cancelAutoFullscreen();
      connectionState.value = 'idle';
      return;
    }
    cancelAutoFullscreen();
    connectionState.value = 'failed';
    streamError.value = messageFromError(error, t('ui.browser_stream.errors.connect'));
  } finally {
    isConnecting.value = false;
    if (!isConnected.value) startSessionStatusPolling();
  }
}

async function requestPrimaryAction(): Promise<void> {
  if (startDisabled.value) {
    streamError.value = validationError.value || t('ui.browser_stream.errors.unavailable');
    return;
  }
  if (selectedAppId.value !== undefined && hasRunningSession.value) {
    startAfterTerminate.value = true;
    terminateOpen.value = true;
    return;
  }
  requestAutoFullscreen();
  await connect(resumeAvailable.value);
}

function requestTerminate(): void {
  startAfterTerminate.value = false;
  sessionActionError.value = '';
  terminateOpen.value = true;
}

async function confirmTerminate(): Promise<void> {
  if (sessionActionPending.value) return;
  sessionActionPending.value = true;
  sessionActionError.value = '';
  const shouldStart = startAfterTerminate.value;
  if (shouldStart) {
    // The confirmation click is the last trusted activation before the close
    // request. Keep the resulting fullscreen state through the intentional
    // disconnect/reconnect handoff below.
    preserveAutoFullscreenDuringRestart = true;
    requestAutoFullscreen();
  }
  try {
    const response = await apiPost<MutationResponse>('/api/apps/close', {});
    if (response.status !== true) {
      throw new Error(response.error || t('webrtc.termination_failed_desc'));
    }
    terminateOpen.value = false;
    startAfterTerminate.value = false;
    await disconnect();
    await fetchSessionStatus();
    if (shouldStart) {
      sessionActionPending.value = false;
      preserveAutoFullscreenDuringRestart = false;
      await connect(false);
    }
  } catch (error) {
    if (shouldStart) {
      preserveAutoFullscreenDuringRestart = false;
      cancelAutoFullscreen();
    }
    sessionActionError.value = messageFromError(error, t('webrtc.termination_failed_desc'));
  } finally {
    preserveAutoFullscreenDuringRestart = false;
    sessionActionPending.value = false;
  }
}

async function disconnect(restartStatusPolling = true): Promise<void> {
  cancelAutoFullscreen();
  releaseForwardedInput();
  stopGamepadCapture();
  stopVideoFrameLatencyMonitoring();
  resetVideoLatencyFence();
  videoLatencyResetAt = null;
  isConnecting.value = false;
  inputChannelState.value = 'closed';
  await browserSession.disconnect();
  if (videoEl.value) videoEl.value.srcObject = null;
  if (audioEl.value) audioEl.value.srcObject = null;
  videoPlaybackStream = null;
  audioPlaybackStream = null;
  publishedStream = null;
  activeStream.publish();
  playbackBlocked.value = false;
  browserStats.value = {};
  connectionState.value = 'idle';
  if (restartStatusPolling) startSessionStatusPolling();
}

function resumePlayback(): void {
  void playAttachedMedia();
}

function formatMetric(value: number | undefined, suffix = ''): string {
  return typeof value === 'number' && Number.isFinite(value)
    ? `${Math.round(value * 10) / 10}${suffix}`
    : '--';
}

function formatBitrateMetric(value: number | undefined): string {
  return typeof value === 'number' && Number.isFinite(value) ? formatBitrate(value) : '--';
}

const overlayLines = computed(() => {
  const fps = formatMetric(browserStats.value.videoFps);
  const bitrate = formatBitrateMetric(browserStats.value.videoBitrateKbps);
  const latency = formatMetric(browserStats.value.roundTripTimeMs, ' ms RTT');
  const dropped = formatMetric(browserStats.value.videoFramesDropped);
  const codec = browserStats.value.videoCodec ?? '--';
  const size = `${form.width}×${form.height}`;
  return [
    `${t('sessions.fps')}: ${fps} | ${t('sessions.bitrate')}: ${bitrate}`,
    `${t('webrtc.metric_latency')}: ${latency} | ${t('webrtc.metric_dropped')}: ${dropped}`,
    `${t('webrtc.encoding')}: ${codec} | ${t('webrtc.resolution')}: ${size}`,
  ];
});

function modifiers(event: KeyboardEvent | MouseEvent | WheelEvent): Record<string, boolean> {
  return {
    alt: event.altKey,
    ctrl: event.ctrlKey,
    meta: event.metaKey,
    shift: event.shiftKey,
  };
}

function pointerPosition(
  event: PointerEvent | WheelEvent,
  clampOutside = false,
): PointerPosition | null {
  const video = videoEl.value;
  const surface = streamSurface.value;
  if (!video || !surface) return null;

  const bounds = video.getBoundingClientRect();
  const sourceWidth = video.videoWidth || bounds.width;
  const sourceHeight = video.videoHeight || bounds.height;
  if (bounds.width <= 0 || bounds.height <= 0 || sourceWidth <= 0 || sourceHeight <= 0) {
    return null;
  }
  const scale = Math.min(bounds.width / sourceWidth, bounds.height / sourceHeight);
  const contentWidth = sourceWidth * scale;
  const contentHeight = sourceHeight * scale;
  const left = bounds.left + (bounds.width - contentWidth) / 2;
  const top = bounds.top + (bounds.height - contentHeight) / 2;
  const normalizedX = (event.clientX - left) / contentWidth;
  const normalizedY = (event.clientY - top) / contentHeight;
  if (!clampOutside && (normalizedX < 0 || normalizedX > 1 || normalizedY < 0 || normalizedY > 1)) {
    return null;
  }
  return {
    x: Math.min(1, Math.max(0, normalizedX)),
    y: Math.min(1, Math.max(0, normalizedY)),
  };
}

function sendPointerMove(event: PointerEvent): void {
  if (!inputReady.value) return;
  if (event.pointerType === 'touch' && !touchPointerGestures.has(event.pointerId)) {
    event.preventDefault();
    return;
  }
  const touchGesture = touchPointerGestures.get(event.pointerId);
  if (touchGesture) {
    event.preventDefault();
    if (!touchGesture.dragging) {
      const distance = Math.hypot(
        event.clientX - touchGesture.startClientX,
        event.clientY - touchGesture.startClientY,
      );
      const elapsedMs = performance.now() - touchGesture.startedAtMs;
      const immediateDragThresholdPx = Math.max(48, touchGesture.dragThresholdPx * 2.5);
      if (
        distance < touchGesture.dragThresholdPx ||
        (elapsedMs < 140 && distance < immediateDragThresholdPx)
      ) {
        return;
      }
      touchGesture.dragging = true;
      const pressed = {
        button: touchGesture.button,
        modifiers: touchGesture.modifiers,
        ...touchGesture.startPosition,
      };
      pressedMouseButtons.set(touchGesture.button, pressed);
      browserSession.sendInput({ ...pressed, type: 'mouse_down' });
    }
  }

  const position = pointerPosition(event, event.buttons !== 0);
  if (!position) return;
  if (touchGesture) touchGesture.lastPosition = position;
  for (const pressed of pressedMouseButtons.values()) {
    pressed.x = position.x;
    pressed.y = position.y;
  }
  // Motion goes as the binary packet the desktop capture path already uses. The
  // host reads only x and y off a JSON move anyway, so `buttons` and `modifiers`
  // were bytes it threw away — and the binary form carries the sequence number
  // the host needs to discard a move that arrives out of order.
  browserSession.sendRawInput(encodeMouseMove(position.x, position.y));
}

/// Right-click belongs to the streamed desktop, not to this browser.
///
/// Button 2 already goes to the host with every pointerdown, so the remote menu
/// does open — the browser's own menu just draws on top of it and swallows the
/// next click. This view forwards pointer input with its own handlers, so nothing
/// else suppresses it.
///
/// Only while input is actually being forwarded. With no stream running this is
/// an ordinary page and should keep an ordinary right-click.
function suppressContextMenu(event: MouseEvent): void {
  if (!inputReady.value) return;
  event.preventDefault();
}

function sendPointerButton(event: PointerEvent, type: 'mouse_down' | 'mouse_up'): void {
  if (!inputReady.value) return;
  const surface = streamSurface.value;
  const touchLike = event.pointerType === 'touch' || event.pointerType === 'pen';
  if (touchLike) event.preventDefault();
  if (
    event.pointerType === 'touch' &&
    (!event.isPrimary || (type === 'mouse_down' && touchPointerGestures.size > 0))
  ) {
    return;
  }
  const touchGesture = touchPointerGestures.get(event.pointerId);
  if (event.pointerType === 'touch' && type === 'mouse_up' && !touchGesture) return;
  if (type === 'mouse_up' && !pressedMouseButtons.has(event.button) && !touchGesture) {
    // WebKit can emit a late pointerup after pointer capture was already lost.
    // Its stale coordinate must not reposition the host cursor a second time.
    return;
  }
  if (!touchLike && surface && document.activeElement !== surface) {
    try {
      surface.focus({ preventScroll: true });
    } catch {
      surface.focus();
    }
  }

  const position =
    type === 'mouse_up' && touchGesture && !touchGesture.dragging
      ? touchGesture.startPosition
      : (pointerPosition(event, type === 'mouse_up' && pressedMouseButtons.has(event.button)) ??
        touchGesture?.lastPosition);
  if (!position) return;

  if (type === 'mouse_down') {
    if (touchLike) {
      const contactRadius = Math.max(event.width || 0, event.height || 0) / 2;
      const gesture: TouchPointerGesture = {
        button: event.button,
        dragThresholdPx:
          event.pointerType === 'pen' ? 8 : Math.max(18, Math.min(32, contactRadius || 18)),
        dragging: false,
        lastPosition: position,
        modifiers: modifiers(event),
        startClientX: event.clientX,
        startClientY: event.clientY,
        startPosition: position,
        startedAtMs: performance.now(),
      };
      touchPointerGestures.set(event.pointerId, gesture);
      try {
        surface?.setPointerCapture(event.pointerId);
      } catch {
        // Pointer capture is optional; the pending tap remains frozen.
      }
      return;
    }
    pressedMouseButtons.set(event.button, {
      button: event.button,
      modifiers: modifiers(event),
      ...position,
    });
    try {
      surface?.setPointerCapture(event.pointerId);
    } catch {
      // Pointer capture is an optimization; the button event is still valid.
    }
  } else if (touchGesture) {
    const releasePosition = touchGesture.dragging
      ? (pointerPosition(event, true) ?? touchGesture.lastPosition)
      : touchGesture.startPosition;
    const release = {
      button: touchGesture.button,
      modifiers: touchGesture.modifiers,
      ...releasePosition,
    };
    if (!touchGesture.dragging) {
      browserSession.sendInput({ ...release, type: 'mouse_down' });
    }
    pressedMouseButtons.delete(touchGesture.button);
    touchPointerGestures.delete(event.pointerId);
    browserSession.sendInput({ ...release, type: 'mouse_up' });
    return;
  } else {
    pressedMouseButtons.delete(event.button);
  }
  browserSession.sendInput({
    ...position,
    button: event.button,
    modifiers: modifiers(event),
    type,
  });
}

function sendWheel(event: WheelEvent): void {
  if (!inputReady.value) return;
  const position = pointerPosition(event);
  if (!position) return;
  event.preventDefault();
  browserSession.sendInput({
    ...position,
    dx: event.deltaX / 100,
    dy: event.deltaY / 100,
    modifiers: modifiers(event),
    type: 'wheel',
  });
}

function cancelFullscreenExitHold(releaseEscape = true): void {
  if (fullscreenExitHoldTimer !== undefined) {
    window.clearTimeout(fullscreenExitHoldTimer);
    fullscreenExitHoldTimer = undefined;
  }
  fullscreenExitHoldActive.value = false;
  if (releaseEscape) fullscreenExitEscapePressed = false;
}

function handleFullscreenExitHold(event: KeyboardEvent, type: 'key_down' | 'key_up'): boolean {
  if (event.code !== 'Escape' || (!fullscreenActive.value && !fullscreenExitEscapePressed)) {
    return false;
  }

  event.preventDefault();
  event.stopPropagation();
  if (type === 'key_up') {
    cancelFullscreenExitHold();
    return true;
  }
  if (fullscreenExitEscapePressed) return true;

  fullscreenExitEscapePressed = true;
  fullscreenExitHoldActive.value = true;
  fullscreenExitHoldTimer = window.setTimeout(() => {
    fullscreenExitHoldTimer = undefined;
    fullscreenExitHoldActive.value = false;
    if (fullscreenActive.value) void exitFullscreen();
  }, fullscreenExitHoldMs);
  return true;
}

function sendKey(event: KeyboardEvent, type: 'key_down' | 'key_up'): void {
  if (handleFullscreenExitHold(event, type)) return;
  if (!inputReady.value) return;
  event.preventDefault();
  if (type === 'key_down') {
    pressedKeys.set(event.code, { code: event.code, key: event.key, modifiers: modifiers(event) });
  } else {
    pressedKeys.delete(event.code);
  }
  browserSession.sendInput({
    code: event.code,
    key: event.key,
    modifiers: modifiers(event),
    repeat: event.repeat,
    type,
  });
}

function startFullscreenExitSwipe(event: PointerEvent): void {
  if (event.pointerType !== 'touch' || !fullscreenActive.value) return;
  event.preventDefault();
  event.stopPropagation();
  fullscreenExitSwipe = {
    pointerId: event.pointerId,
    startX: event.clientX,
    startY: event.clientY,
  };
  try {
    (event.currentTarget as HTMLElement | null)?.setPointerCapture(event.pointerId);
  } catch {
    // The gesture remains usable without pointer capture.
  }
}

function updateFullscreenExitSwipe(event: PointerEvent): void {
  const swipe = fullscreenExitSwipe;
  if (!swipe || swipe.pointerId !== event.pointerId) return;
  event.preventDefault();
  event.stopPropagation();
  const dx = event.clientX - swipe.startX;
  const dy = event.clientY - swipe.startY;
  if (dy < fullscreenExitSwipeThresholdPx || dy < Math.abs(dx) * 1.4) return;
  fullscreenExitSwipe = null;
  void exitFullscreen();
}

function finishFullscreenExitSwipe(event?: PointerEvent): void {
  if (event && fullscreenExitSwipe?.pointerId !== event.pointerId) return;
  event?.preventDefault();
  event?.stopPropagation();
  fullscreenExitSwipe = null;
}

function onPointerCaptureEnd(event: PointerEvent): void {
  if (event.pointerType === 'touch' && !touchPointerGestures.has(event.pointerId)) return;
  releaseForwardedInput();
}

function releaseForwardedInput(): void {
  for (const pressed of pressedMouseButtons.values()) {
    browserSession.sendInput({ ...pressed, type: 'mouse_up' });
  }
  pressedMouseButtons.clear();
  touchPointerGestures.clear();

  for (const pressed of pressedKeys.values()) {
    browserSession.sendInput({ ...pressed, repeat: false, type: 'key_up' });
  }
  pressedKeys.clear();
}

function stopGamepadCapture(): void {
  if (gamepadCapture) {
    // Neutralize pressed controller state before the disconnect message. This
    // covers input being disabled or the data channel closing without a page
    // blur event.
    gamepadCapture.release();
    gamepadCapture.stop();
  }
  gamepadCapture = null;
}

function syncGamepadCapture(): void {
  if (inputReady.value && windowFocused.value) {
    if (!gamepadCapture) {
      gamepadCapture = attachBrowserGamepadCapture((payload) => browserSession.sendInput(payload), {
        // A hidden page must release controller state and wait for a fresh
        // connect/state handshake when it becomes visible again.
        enabled: () => inputReady.value && document.visibilityState === 'visible',
        getGamepads,
      });
    }
    return;
  }
  stopGamepadCapture();
}

function onWindowBlur(): void {
  windowFocused.value = false;
  cancelFullscreenExitHold();
  finishFullscreenExitSwipe();
  releaseForwardedInput();
  stopGamepadCapture();
}

function onWindowFocus(): void {
  windowFocused.value = true;
  syncGamepadCapture();
}

function onVisibilityChange(): void {
  if (document.visibilityState !== 'visible') {
    windowFocused.value = false;
    cancelFullscreenExitHold();
    finishFullscreenExitSwipe();
    releaseForwardedInput();
    stopGamepadCapture();
  } else {
    windowFocused.value = true;
    syncGamepadCapture();
  }
}

function requestAutoFullscreen(): void {
  if (!autoFullscreen.value || fullscreenActive.value) return;

  // The fullscreen request must start in the click/submit event. Clearing the
  // collapsed presentation synchronously keeps the surface available to the
  // browser before enterFullscreen reaches its first await.
  revealStreamSurfaceForFullscreen();
  const attempt = ++autoFullscreenAttempt;
  void enterFullscreen()
    .then(() => {
      // A failed or cancelled connection must not leave a pseudo/fullscreen
      // surface behind. The token check also handles a rejected native request
      // that resolves after disconnect has already started.
      if (
        !preserveAutoFullscreenDuringRestart &&
        (attempt !== autoFullscreenAttempt ||
          connectionState.value === 'failed' ||
          connectionState.value === 'disconnected' ||
          connectionState.value === 'closed' ||
          connectionState.value === 'idle')
      ) {
        return exitFullscreen();
      }
    })
    .catch(() => {
      // Fullscreen is an enhancement; connection startup still proceeds.
    });
}

function cancelAutoFullscreen(): void {
  if (preserveAutoFullscreenDuringRestart) return;
  autoFullscreenAttempt += 1;
  if (fullscreenActive.value) void exitFullscreen();
}

function toggleStreamCollapsed(): void {
  if (fullscreenActive.value) return;

  const collapsed = !streamCollapsed.value;
  if (collapsed) {
    streamSurfaceFocusedBeforeCollapse = document.activeElement === streamSurface.value;
    // Collapsing a live stream is a presentation change. Release all held
    // input and stop the gamepad poller while the surface is unavailable, but
    // leave the media elements and WebRTC session mounted and running.
    releaseForwardedInput();
    stopGamepadCapture();
    if (document.activeElement === streamSurface.value) streamSurface.value?.blur();
  }
  streamCollapsed.value = collapsed;

  if (!collapsed && streamSurfaceFocusedBeforeCollapse && isConnected.value) {
    window.requestAnimationFrame(() => {
      if (!streamCollapsed.value && inputReady.value) {
        try {
          streamSurface.value?.focus({ preventScroll: true });
        } catch {
          streamSurface.value?.focus();
        }
      }
    });
  }
}

function requestManualFullscreen(): void {
  if (fullscreenActive.value) return;
  // Keep the request in the button's trusted activation. Vue will apply the
  // v-show update after this handler, while the element is still requestable.
  revealStreamSurfaceForFullscreen();
  void enterFullscreen();
}

function revealStreamSurfaceForFullscreen(): void {
  streamCollapsed.value = false;
  // v-show applies its display update after the current event handler. Clear
  // the previous hidden inline style now so a manual fullscreen request made
  // from the collapsed state still sees a rendered element.
  if (streamSurface.value) streamSurface.value.style.display = '';
}

async function enterFullscreen(): Promise<void> {
  const surface = streamSurface.value;
  const video = videoEl.value;
  if (!surface) return;

  if (await requestElementFullscreen(surface)) {
    surface.focus();
    return;
  }
  if (video && (await requestElementFullscreen(video))) return;
  if (video && enterNativeVideoFullscreen(video)) return;

  enterPseudoFullscreen();
  surface.focus();
}

async function requestElementFullscreen(element: HTMLElement): Promise<boolean> {
  try {
    if (typeof element.requestFullscreen === 'function') {
      await element.requestFullscreen({ keyboardLock: 'browser' } as FullscreenOptions);
      void requestFullscreenKeyboardLock();
      return true;
    }
  } catch {
    // Try WebKit's prefixed API before falling back to video fullscreen.
  }

  const webkitElement = element as WebKitFullscreenElement;
  const request = webkitElement.webkitRequestFullscreen ?? webkitElement.webkitRequestFullScreen;
  if (typeof request !== 'function') return false;
  try {
    await request.call(webkitElement);
    return true;
  } catch {
    return false;
  }
}

function currentFullscreenElement(): Element | null {
  const webkitDocument = document as Document & { webkitFullscreenElement?: Element | null };
  return document.fullscreenElement ?? webkitDocument.webkitFullscreenElement ?? null;
}

async function requestFullscreenKeyboardLock(): Promise<void> {
  const keyboard = (navigator as KeyboardLockNavigator).keyboard;
  if (typeof keyboard?.lock !== 'function' || !window.isSecureContext) return;
  const request = ++fullscreenKeyboardLockRequest;
  try {
    await keyboard.lock();
    if (request !== fullscreenKeyboardLockRequest || !currentFullscreenElement()) {
      keyboard.unlock?.();
    }
  } catch {
    // Safari's fullscreen keyboardLock option remains the primary lock path.
  }
}

function releaseFullscreenKeyboardLock(): void {
  fullscreenKeyboardLockRequest += 1;
  try {
    (navigator as KeyboardLockNavigator).keyboard?.unlock?.();
  } catch {
    // Browsers also release keyboard lock automatically when fullscreen ends.
  }
}

function onFullscreenChange(): void {
  nativeFullscreen.value = Boolean(currentFullscreenElement());
  if (
    nativeFullscreen.value &&
    ['idle', 'failed', 'disconnected', 'closed'].includes(String(connectionState.value))
  ) {
    void exitFullscreen();
    return;
  }
  if (nativeFullscreen.value) {
    void requestFullscreenKeyboardLock();
  } else {
    cancelFullscreenExitHold(false);
    finishFullscreenExitSwipe();
    releaseFullscreenKeyboardLock();
  }
}

function onNativeVideoFullscreenBegin(): void {
  nativeVideoFullscreen.value = true;
  if (['idle', 'failed', 'disconnected', 'closed'].includes(String(connectionState.value))) {
    void exitFullscreen();
  }
}

function onNativeVideoFullscreenEnd(): void {
  nativeVideoFullscreen.value = false;
  cancelFullscreenExitHold(false);
  finishFullscreenExitSwipe();
}

function enterNativeVideoFullscreen(video: HTMLVideoElement): boolean {
  const webkitVideo = video as WebKitFullscreenVideoElement;
  const enter = webkitVideo.webkitEnterFullscreen ?? webkitVideo.webkitEnterFullScreen;
  if (typeof enter !== 'function') return false;
  try {
    enter.call(webkitVideo);
    return true;
  } catch {
    return false;
  }
}

function enterPseudoFullscreen(): void {
  if (pseudoFullscreen.value) return;
  pageOverflowBeforePseudoFullscreen = {
    body: document.body.style.overflow,
    root: document.documentElement.style.overflow,
  };
  document.body.style.overflow = 'hidden';
  document.documentElement.style.overflow = 'hidden';
  pseudoFullscreen.value = true;
}

function exitPseudoFullscreen(): void {
  if (!pseudoFullscreen.value) return;
  pseudoFullscreen.value = false;
  document.body.style.overflow = pageOverflowBeforePseudoFullscreen?.body ?? '';
  document.documentElement.style.overflow = pageOverflowBeforePseudoFullscreen?.root ?? '';
  pageOverflowBeforePseudoFullscreen = null;
}

async function exitFullscreen(): Promise<void> {
  releaseFullscreenKeyboardLock();

  if (currentFullscreenElement()) {
    const webkitDocument = document as WebKitFullscreenDocument;
    const exits = [
      document.exitFullscreen,
      webkitDocument.webkitExitFullscreen,
      webkitDocument.webkitCancelFullScreen,
    ];
    for (const exit of exits) {
      if (typeof exit !== 'function') continue;
      try {
        await exit.call(document);
        return;
      } catch {
        // Try the next browser-specific exit API.
      }
    }
  }

  const webkitVideo = videoEl.value as WebKitFullscreenVideoElement | null;
  if (webkitVideo?.webkitDisplayingFullscreen && webkitVideo.webkitExitFullscreen) {
    try {
      webkitVideo.webkitExitFullscreen();
      return;
    } catch {
      // The compatibility fallback may still be active.
    }
  }

  exitPseudoFullscreen();
}

watch(inputForwarding, (enabled, wasEnabled) => {
  if (!enabled && wasEnabled) releaseForwardedInput();
  syncGamepadCapture();
});

/// Apply the slider value to the audio element whenever it changes, even
/// mid-stream. Without this, the slider would only matter at the next attach
/// — the owner would scrub the bar, see the percent move, and hear nothing
/// change until the stream restarted.
watch(
  () => form.volume,
  (value) => {
    if (audioEl.value) audioEl.value.volume = clampVolume(value);
    activeStream.setVolume(clampVolume(value));
  },
);

watch(inputReady, syncGamepadCapture);
watch(
  () => form.fps,
  () => {
    form.videoMaxFrameAgeFrames = clampFrameAgeFrames(
      form.videoMaxFrameAgeFrames,
      form.fps,
      form.videoPacingMode,
    );
  },
);
watch(
  [form, autoFullscreen, enhance, codecChoice, bitrateTracksRecommendation],
  persistStreamSettings,
  { deep: true },
);

onMounted(() => {
  standaloneWebApp.value = runningAsStandaloneWebApp();
  void refresh();
  startSessionStatusPolling();
  startGamepadPolling();
  window.addEventListener('blur', onWindowBlur);
  window.addEventListener('gamepadconnected', onGamepadConnectionChange);
  window.addEventListener('gamepaddisconnected', onGamepadConnectionChange);
  window.addEventListener('focus', onWindowFocus);
  document.addEventListener('fullscreenchange', onFullscreenChange);
  document.addEventListener('webkitfullscreenchange', onFullscreenChange as EventListener);
  document.addEventListener('visibilitychange', onVisibilityChange);
  videoEl.value?.addEventListener('webkitbeginfullscreen', onNativeVideoFullscreenBegin);
  videoEl.value?.addEventListener('webkitendfullscreen', onNativeVideoFullscreenEnd);
});
onBeforeUnmount(() => {
  stopSessionStatusPolling();
  stopGamepadPolling();
  cancelFullscreenExitHold();
  finishFullscreenExitSwipe();
  exitPseudoFullscreen();
  releaseFullscreenKeyboardLock();
  window.removeEventListener('blur', onWindowBlur);
  window.removeEventListener('gamepadconnected', onGamepadConnectionChange);
  window.removeEventListener('gamepaddisconnected', onGamepadConnectionChange);
  window.removeEventListener('focus', onWindowFocus);
  document.removeEventListener('fullscreenchange', onFullscreenChange);
  document.removeEventListener('webkitfullscreenchange', onFullscreenChange as EventListener);
  document.removeEventListener('visibilitychange', onVisibilityChange);
  videoEl.value?.removeEventListener('webkitbeginfullscreen', onNativeVideoFullscreenBegin);
  videoEl.value?.removeEventListener('webkitendfullscreen', onNativeVideoFullscreenEnd);
  releaseForwardedInput();
  stopVideoFrameLatencyMonitoring();
  void disconnect(false);
});

// KeepAlive keeps this page mounted across navigation so the stream survives, but
// lifting its DOM out of the document pauses both media elements (that is what a
// media element does on removal), and nothing resumes a paused element on its own.
// Coming back showed the last frame, silent, under a badge that still said
// Connected. The pollers also ran on every other page for the life of the app.
onActivated(() => {
  if (isConnected.value) {
    void playAttachedMedia();
    if (videoEl.value) startVideoFrameLatencyMonitoring(videoEl.value);
  }
  startSessionStatusPolling();
  startGamepadPolling();
});
onDeactivated(() => {
  // Still mounted for the mini player's sake, but nothing here can be seen or
  // pressed: stop the pollers and let go of any input still held on the host.
  releaseForwardedInput();
  cancelFullscreenExitHold();
  stopSessionStatusPolling();
  stopGamepadPolling();
  stopVideoFrameLatencyMonitoring();
});
</script>

<template>
  <div class="vs-page vs-page--dashboard browser-stream-page">
    <PageHeader
      :title="t('ui.browser_stream.title')"
      :description="t('ui.browser_stream.description')"
    >
      <template #actions>
        <AppButton
          icon="refresh"
          :label="t('_common.refresh')"
          variant="secondary"
          :busy="loading"
          :busy-label="t('ui.browser_stream.refreshing')"
          @click="refresh"
        />
      </template>
    </PageHeader>

    <InlineAlert
      v-if="refreshError"
      tone="warning"
      :title="t('ui.browser_stream.errors.refresh')"
      announce="polite"
    >
      {{ refreshError }}
    </InlineAlert>

    <InlineAlert
      v-if="!loading && !hostReady"
      tone="warning"
      :title="t('ui.browser_stream.host_not_ready')"
    >
      {{ hostCapabilities.availability.reason || t('ui.browser_stream.reasons.host_unverified') }}
    </InlineAlert>

    <InlineAlert
      v-if="streamError"
      tone="danger"
      :title="t('ui.browser_stream.errors.connect')"
      announce="assertive"
    >
      {{ streamError }}
    </InlineAlert>

    <InlineAlert
      v-if="sessionActionError"
      tone="danger"
      :title="t('webrtc.termination_failed')"
      announce="assertive"
    >
      {{ sessionActionError }}
    </InlineAlert>

    <InlineAlert
      v-if="playbackBlocked"
      tone="warning"
      :title="t('ui.browser_stream.errors.playback')"
      announce="polite"
    >
      {{ t('ui.browser_stream.errors.playback_detail') }}
      <template #actions>
        <AppButton
          icon="play"
          :label="t('ui.browser_stream.resume_playback')"
          variant="secondary"
          @click="resumePlayback"
        />
      </template>
    </InlineAlert>

    <section class="app-picker panel" aria-labelledby="browser-stream-app-picker-title">
      <div class="panel__heading app-picker__heading">
        <div>
          <h2 id="browser-stream-app-picker-title">
            {{ t('ui.browser_stream.settings.application') }}
          </h2>
          <p>{{ t('ui.browser_stream.settings.application_help') }}</p>
        </div>
        <label v-if="launchableApps.length" class="app-picker__search">
          <span class="vs-sr-only">{{ t('webrtc.search_placeholder') }}</span>
          <UiIcon name="search" :size="17" />
          <input
            v-model="appSearch"
            class="vs-input"
            type="search"
            :placeholder="t('webrtc.search_placeholder')"
            :disabled="connectionPending || isConnected"
          />
        </label>
      </div>

      <LoadingSkeleton v-if="loading" variant="block" height="56px" aria-hidden="true" />
      <template v-else>
        <div
          class="app-picker__grid"
          role="listbox"
          :aria-label="t('ui.browser_stream.settings.application')"
        >
          <button
            class="app-picker__card"
            :class="{ 'app-picker__card--selected': appSelected() }"
            type="button"
            role="option"
            :aria-selected="appSelected()"
            :disabled="connectionPending || isConnected"
            :title="
              resumeAvailable ? t('webrtc.no_selection') : t('ui.browser_stream.picker.desktop_detail')
            "
            @click="selectApp()"
          >
            <span class="app-picker__artwork app-picker__artwork--desktop">
              <UiIcon name="devices" :size="18" />
            </span>
            <strong>{{ t('ui.browser_stream.desktop') }}</strong>
            <UiIcon v-if="appSelected()" class="app-picker__check" name="check" :size="14" />
          </button>

          <button
            v-for="app in filteredLaunchableApps"
            :key="app.id"
            class="app-picker__card"
            :class="{ 'app-picker__card--selected': appSelected(app.id) }"
            type="button"
            role="option"
            :aria-selected="appSelected(app.id)"
            :aria-label="app.name"
            :disabled="connectionPending || isConnected"
            @click="selectApp(app.id)"
          >
            <span class="app-picker__artwork">
              <img
                v-if="app.coverUrl && !appCoverFailed(app.id)"
                :src="app.coverUrl"
                :alt="t('ui.browser_stream.picker.cover_alt', { name: app.name })"
                loading="lazy"
                @error="markAppCoverFailed(app.id)"
              />
              <span v-else class="app-picker__artwork-fallback" aria-hidden="true">
                <UiIcon name="gamepad" :size="16" />
              </span>
            </span>
            <strong>{{ app.name }}</strong>
            <UiIcon v-if="appSelected(app.id)" class="app-picker__check" name="check" :size="14" />
          </button>
        </div>

        <p v-if="!launchableApps.length" class="app-picker__empty">
          {{ t('webrtc.no_applications_hint') }}
        </p>
        <p v-else-if="!filteredLaunchableApps.length" class="app-picker__empty">
          {{ t('webrtc.no_applications_match', { query: appSearch.trim() }) }}
        </p>
      </template>
    </section>

    <section class="stream-stage panel" aria-labelledby="browser-stream-stage-title">
      <div class="panel__heading stream-stage__heading">
        <div>
          <h2 id="browser-stream-stage-title">{{ selectedAppName }}</h2>
          <p>{{ t('ui.browser_stream.stage_description') }}</p>
        </div>
        <div class="stream-stage__heading-actions">
          <StatusBadge :label="connectionLabel" :tone="connectionTone" announce="polite" />
          <AppButton
            v-if="!fullscreenActive"
            icon="chevron-down"
            :label="
              streamCollapsed ? t('ui.browser_stream.expand') : t('ui.browser_stream.collapse')
            "
            variant="tertiary"
            :aria-expanded="!streamCollapsed"
            aria-controls="browser-stream-surface"
            @click="toggleStreamCollapsed"
          />
        </div>
      </div>

      <div
        ref="streamSurface"
        id="browser-stream-surface"
        class="stream-surface"
        v-show="!streamCollapsed"
        :class="{
          'stream-surface--interactive': inputReady,
          'stream-surface--pseudo-fullscreen': pseudoFullscreen,
          'stream-surface--touch-exit': showFullscreenSwipeExit,
        }"
        :tabindex="streamCollapsed ? -1 : 0"
        :aria-hidden="streamCollapsed ? 'true' : undefined"
        :aria-label="t('ui.browser_stream.stream_surface')"
        @keydown="sendKey($event, 'key_down')"
        @keyup="sendKey($event, 'key_up')"
        @contextmenu="suppressContextMenu"
        @pointerdown="sendPointerButton($event, 'mouse_down')"
        @pointermove="sendPointerMove"
        @pointerup="sendPointerButton($event, 'mouse_up')"
        @pointercancel="onPointerCaptureEnd"
        @lostpointercapture="onPointerCaptureEnd"
        @blur="releaseForwardedInput"
        @wheel="sendWheel"
      >
        <video ref="videoEl" autoplay muted playsinline disablepictureinpicture />
        <!-- FSR output. The video stays mounted and playing underneath (it is the
             decode surface and audio sink); v-show keeps it compositing so frame
             callbacks still fire, unlike display:none on the video itself. -->
        <canvas v-show="upscalerActive" ref="fxCanvas" class="stream-surface__fx"></canvas>
        <audio ref="audioEl" autoplay hidden />
        <TouchGamepad v-if="showTouchPad && inputReady" />

        <div v-if="showStats && isConnected" class="stream-stats">
          <div>
            path {{ streamStats.path ?? '—'
            }}<template v-if="streamStats.remoteAddress">
              · {{ streamStats.remoteAddress }}</template>
          </div>
          <!-- `?? 0` printed "0 ms" for a figure the browser had not reported yet,
               which reads as a measurement rather than a gap — and 0 ms RTT is a
               number no real connection produces. Missing stays missing. -->
          <div>rtt {{ statMs(streamStats.roundTripMs) }}</div>
          <div>jitter {{ statMs(streamStats.jitterMs) }}</div>
          <div>buffer {{ statMs(streamStats.jitterBufferMs) }}</div>
          <div>lost {{ streamStats.packetsLost ?? '—' }}</div>
          <div>dropped {{ streamStats.framesDropped ?? '—' }}</div>
          <div>
            {{ Math.round(streamStats.bitrateKbps ?? 0) }} kbps ·
            {{ Math.round(streamStats.fps ?? 0) }} fps
          </div>
          <div>{{ streamStats.codec ?? '' }}</div>
        </div>
        <div
          v-if="showPerformanceOverlay && isConnected"
          class="stream-performance-overlay"
          :aria-label="t('webrtc.show_performance_overlay')"
          aria-live="off"
        >
          <div v-for="line in overlayLines" :key="line" class="stream-performance-overlay__line">
            {{ line }}
          </div>
        </div>
        <div
          v-if="showFullscreenSwipeExit"
          class="stream-surface__exit-swipe"
          aria-hidden="true"
          @click.stop
          @keydown.stop
          @keyup.stop
          @lostpointercapture="finishFullscreenExitSwipe"
          @pointercancel="finishFullscreenExitSwipe"
          @pointerdown="startFullscreenExitSwipe"
          @pointermove="updateFullscreenExitSwipe"
          @pointerup="finishFullscreenExitSwipe"
        >
          <span aria-hidden="true" />
          {{ t('ui.browser_stream.exit_fullscreen_swipe_hint') }}
        </div>
        <div
          v-if="fullscreenActive"
          class="stream-surface__exit-fullscreen"
          :class="{ 'stream-surface__exit-fullscreen--clear-of-pad': showTouchPad && inputReady }"
          @click.stop
          @keydown.stop
          @keyup.stop
          @pointercancel.stop
          @pointerdown.stop
          @pointermove.stop
          @pointerup.stop
        >
          <label class="stream-surface__overlay-toggle">
            <input v-model="showPerformanceOverlay" type="checkbox" />
            <span>{{ t('webrtc.show_performance_overlay') }}</span>
          </label>
          <AppButton
            icon="x"
            :label="fullscreenExitControlLabel"
            variant="secondary"
            @click="exitFullscreen"
          />
        </div>
        <div v-if="!isConnected && !connectionPending" class="stream-surface__empty">
          <span class="stream-surface__empty-icon" aria-hidden="true"
            ><UiIcon name="play" :size="28"
          /></span>
          <strong>{{ t('ui.browser_stream.ready_to_start') }}</strong>
          <span>{{ t('ui.browser_stream.ready_to_start_detail') }}</span>
        </div>
        <div v-else-if="connectionPending" class="stream-surface__empty">
          <span class="stream-surface__spinner" aria-hidden="true" />
          <strong>{{ t('ui.browser_stream.status.connecting') }}</strong>
          <span>{{ t('ui.browser_stream.connecting_detail') }}</span>
        </div>
      </div>

      <div class="stream-stage__actions">
        <AppButton
          v-if="connectionPending"
          icon="stop"
          :label="t('ui.browser_stream.cancel')"
          variant="secondary"
          @click="disconnect()"
        />
        <AppButton
          v-else-if="!isConnected"
          icon="play"
          :label="primaryActionLabel"
          variant="primary"
          :disabled="startDisabled"
          @click="requestPrimaryAction"
        />
        <AppButton
          v-else
          icon="stop"
          :label="t('ui.browser_stream.disconnect')"
          variant="danger"
          @click="disconnect()"
        />
        <AppButton
          v-if="!isConnected && !connectionPending && hasRunningSession"
          icon="stop"
          :label="t('webrtc.terminate')"
          variant="danger"
          :disabled="sessionActionPending"
          @click="requestTerminate"
        />
        <AppButton
          icon="external-link"
          :label="t('ui.browser_stream.fullscreen')"
          variant="secondary"
          :disabled="!isConnected"
          @click="requestManualFullscreen"
        />
        <AppButton
          v-if="showInstallWebAppAction"
          icon="help"
          :label="t('ui.browser_stream.install.action')"
          variant="secondary"
          @click="installHelpOpen = true"
        />
        <label
          class="stream-stage__volume"
          :title="t('ui.browser_stream.settings.volume_help', 'Adjusts the stream audio in this browser only.')"
        >
          <UiIcon name="volume" :size="16" />
          <input
            v-model.number="form.volume"
            class="vs-range"
            type="range"
            min="0"
            max="100"
            step="1"
            :style="{ '--vs-range-fill': `${form.volume}%` }"
            :aria-label="t('ui.browser_stream.settings.volume', 'Volume')"
          />
          <span class="stream-stage__volume-value">{{ volumeLabel }}</span>
        </label>
        <!-- Bars rather than a number, because the number is the thing people cannot
             read. The title carries the specific measurement and what to do about
             it, which is the part worth having when it is not green. -->
        <span
          v-if="isConnected"
          class="stream-stage__link"
          :data-tier="linkQuality.tier"
          :title="`${linkQuality.headline} — ${linkQuality.detail}`"
        >
          <span class="stream-stage__link-bars" aria-hidden="true">
            <i v-for="bar in 4" :key="bar" :class="{ 'is-lit': bar <= linkQuality.bars }" />
          </span>
          <span class="vs-sr-only">
            {{ t('ui.browser_stream.link_quality', 'Connection quality') }}:
            {{ linkQuality.headline }}. {{ linkQuality.detail }}
          </span>
          <span aria-hidden="true">{{ linkQuality.headline }}</span>
        </span>
        <!-- These three are drawn entirely in this browser and never reach the host,
             so unlike the encoder settings they change instantly, mid-stream. They
             sit under the video rather than in the settings form because that is
             where you are looking when you want them. -->
        <div class="stream-stage__toggles" role="group" :aria-label="t('ui.browser_stream.settings.this_browser', 'This browser')">
          <button
            type="button"
            class="stream-stage__toggle"
            :class="{ 'stream-stage__toggle--on': showStats }"
            :aria-pressed="showStats"
            :title="t('ui.browser_stream.settings.show_stats', 'Show performance stats')"
            @click="showStats = !showStats"
          >
            <UiIcon name="activity" :size="18" />
            <span class="vs-sr-only">{{
              t('ui.browser_stream.settings.show_stats', 'Show performance stats')
            }}</span>
          </button>
          <button
            type="button"
            class="stream-stage__toggle"
            :class="{ 'stream-stage__toggle--on': enhance }"
            :aria-pressed="enhance"
            :disabled="enhanceUnavailable"
            :title="t('ui.browser_stream.settings.enhance', 'Sharpen upscaled video (FSR)')"
            @click="enhance = !enhance"
          >
            <UiIcon name="sparkle" :size="18" />
            <span class="vs-sr-only">{{
              t('ui.browser_stream.settings.enhance', 'Sharpen upscaled video (FSR)')
            }}</span>
          </button>
          <button
            type="button"
            class="stream-stage__toggle"
            :class="{ 'stream-stage__toggle--on': showTouchPad }"
            :aria-pressed="showTouchPad"
            :title="t('ui.browser_stream.settings.touch_pad', 'Show on-screen controller')"
            @click="showTouchPad = !showTouchPad"
          >
            <UiIcon name="gamepad" :size="18" />
            <span class="vs-sr-only">{{
              t('ui.browser_stream.settings.touch_pad', 'Show on-screen controller')
            }}</span>
          </button>
        </div>
        <span class="stream-stage__input-status" :data-ready="inputReady">
          <UiIcon :name="inputReady ? 'check-circle' : 'info'" :size="16" />
          {{ inputStatusLabel }}
        </span>
        <span
          class="stream-stage__gamepad-status"
          :data-connected="connectedGamepads > 0"
          :title="connectedGamepads > 0
            ? t('ui.browser_stream.gamepad_connected', { count: connectedGamepads })
            : t('ui.browser_stream.gamepad_none')"
        >
          <UiIcon name="gamepad" :size="16" />
          {{
            connectedGamepads > 0
              ? connectedGamepads === 1
                ? t('ui.browser_stream.gamepad_connected_one')
                : t('ui.browser_stream.gamepad_connected', { count: connectedGamepads })
              : t('ui.browser_stream.gamepad_none')
          }}
        </span>
      </div>
      <p>{{ t('webrtc.touch_input_hint') }}</p>
    </section>

    <div v-if="loading" class="browser-stream-loading" aria-hidden="true">
      <LoadingSkeleton variant="block" height="310px" />
      <LoadingSkeleton variant="block" height="310px" />
    </div>

    <div v-else class="browser-stream-grid">
      <section class="panel" aria-labelledby="browser-stream-settings-title">
        <div class="panel__heading">
          <div>
            <h2 id="browser-stream-settings-title">{{ t('ui.browser_stream.settings.title') }}</h2>
            <p>{{ t('ui.browser_stream.settings.description') }}</p>
          </div>
        </div>

        <form class="stream-form" @submit.prevent="requestPrimaryAction">
          <fieldset class="stream-form__group" :disabled="connectionPending || isConnected">
            <legend>{{ t('ui.browser_stream.settings.video') }}</legend>
            <div class="stream-form__select-grid">
              <label class="vs-field" for="browser-stream-codec">
                <span class="vs-field__label">{{ t('ui.browser_stream.settings.codec') }}</span>
                <select
                  id="browser-stream-codec"
                  v-model="codecChoice"
                  class="vs-select"
                  @change="onCodecChoiceChanged"
                >
                  <option value="auto">
                    {{ t('ui.browser_stream.codecs.auto', 'Automatic') }}
                  </option>
                  <option
                    v-for="codec in codecs"
                    :key="codec"
                    :value="codec"
                    :disabled="!codecAvailable(codec)"
                  >
                    {{ codecLabel(codec)
                    }}{{ codecAvailable(codec) ? '' : ` - ${codecUnavailableReason(codec)}` }}
                  </option>
                </select>
              </label>

              <label class="vs-field" for="browser-stream-resolution">
                <span class="vs-field__label">{{
                  t('ui.browser_stream.settings.resolution', 'Resolution')
                }}</span>
                <select
                  id="browser-stream-resolution"
                  v-model="resolutionChoice"
                  class="vs-select"
                  @change="onResolutionChanged"
                >
                  <option v-for="preset in RESOLUTIONS" :key="preset.label" :value="preset.label">
                    {{ preset.label }} ({{ preset.width }}x{{ preset.height }})
                  </option>
                  <option value="custom">
                    {{ t('ui.browser_stream.settings.custom', 'Custom') }}
                  </option>
                </select>
              </label>

              <div
                v-if="resolutionChoice === 'custom'"
                class="stream-form__numeric-grid stream-form__span"
              >
                <label class="vs-field" for="browser-stream-width">
                  <span class="vs-field__label">{{ t('ui.browser_stream.settings.width') }}</span>
                  <input
                    id="browser-stream-width"
                    v-model.number="form.width"
                    class="vs-input"
                    type="number"
                    :min="hostCapabilities.limits.min_dimension"
                    :max="hostCapabilities.limits.max_dimension"
                    step="2"
                  />
                </label>
                <label class="vs-field" for="browser-stream-height">
                  <span class="vs-field__label">{{ t('ui.browser_stream.settings.height') }}</span>
                  <input
                    id="browser-stream-height"
                    v-model.number="form.height"
                    class="vs-input"
                    type="number"
                    :min="hostCapabilities.limits.min_dimension"
                    :max="hostCapabilities.limits.max_dimension"
                    step="2"
                  />
                </label>
              </div>

              <label class="vs-field" for="browser-stream-fps-choice">
                <span class="vs-field__label">{{ t('ui.browser_stream.settings.fps') }}</span>
                <select
                  id="browser-stream-fps-choice"
                  v-model="fpsChoice"
                  class="vs-select"
                  @change="onFpsChanged"
                >
                  <option v-for="rate in FRAME_RATES" :key="rate" :value="String(rate)">
                    {{ rate }} fps
                  </option>
                  <option value="custom">
                    {{ t('ui.browser_stream.settings.custom', 'Custom') }}
                  </option>
                </select>
              </label>

              <label
                v-if="fpsChoice === 'custom'"
                class="vs-field stream-form__span"
                for="browser-stream-fps"
              >
                <span class="vs-field__label">{{
                  t('ui.browser_stream.settings.custom_fps', 'Custom frame rate')
                }}</span>
                <input
                  id="browser-stream-fps"
                  v-model.number="form.fps"
                  class="vs-input"
                  type="number"
                  :min="hostCapabilities.limits.min_fps"
                  :max="hostCapabilities.limits.max_fps"
                  step="1"
                />
              </label>
            </div>

            <label
              class="stream-form__check"
              :class="{ 'stream-form__check--disabled': hdrControlDisabled }"
            >
              <input
                :checked="effectiveHdr"
                type="checkbox"
                :disabled="hdrControlDisabled"
                @change="setHdr"
              />
              <span>
                <strong>{{ t('ui.browser_stream.settings.hdr') }}</strong>
                <small>{{ hdrControlDescription }}</small>
              </span>
            </label>

            <div class="stream-form__toggles">
              <label class="stream-form__check stream-form__check--plain">
                <input v-model="useHostBitrate" type="checkbox" />
                <span>{{
                  t('ui.browser_stream.settings.bitrate_host_default_option', 'Let the host choose bitrate')
                }}</span>
              </label>
            </div>

            <label class="vs-field" for="browser-stream-bitrate">
              <span class="vs-field__label">
                {{ t('ui.browser_stream.settings.bitrate_simple', 'Bitrate') }} —
                {{ bitrateLabel }}
              </span>
              <input
                id="browser-stream-bitrate"
                v-model.number="form.bitrateKbps"
                class="vs-range"
                type="range"
                :disabled="useHostBitrate"
                :min="Math.max(hostCapabilities.limits.min_bitrate_kbps, 1000)"
                :max="hostCapabilities.limits.max_bitrate_kbps || 150000"
                step="1000"
                :style="{ '--vs-range-fill': `${bitrateFillPercent}%` }"
              />
              <small class="vs-field__help stream-form__bitrate-help">
                <template v-if="!bitrateMatchesRecommendation && recommendedBitrateKbps() > 0">
                  {{
                    t('ui.browser_stream.settings.bitrate_recommended_hint', {
                      value: `${(recommendedBitrateKbps() / 1000).toFixed(0)} Mbps`,
                    })
                  }}
                </template>
                <template v-else>
                  {{ t('ui.browser_stream.settings.bitrate_tracks_recommendation') }}
                </template>
                <AppButton
                  v-if="!bitrateMatchesRecommendation && recommendedBitrateKbps() > 0 && !useHostBitrate"
                  size="compact"
                  variant="tertiary"
                  :label="t('ui.browser_stream.settings.bitrate_use_recommended', 'Use recommended')"
                  @click="applyRecommendedBitrate"
                />
              </small>
            </label>

            <div class="stream-pacing">
              <div class="stream-pacing__heading">
                <span class="vs-field__label">{{ t('webrtc.frame_pacing') }}</span>
                <small>{{ t('webrtc.frame_pacing_desc') }}</small>
              </div>
              <div
                class="stream-pacing__presets"
                role="group"
                :aria-label="t('webrtc.frame_pacing')"
              >
                <button
                  v-for="option in pacingOptions"
                  :key="option.value"
                  class="stream-pacing__preset"
                  :class="{
                    'stream-pacing__preset--selected': form.videoPacingMode === option.value,
                  }"
                  type="button"
                  :aria-pressed="form.videoPacingMode === option.value"
                  @click="applyPacingPreset(option.value)"
                >
                  {{ option.label }}
                </button>
              </div>
              <div class="stream-form__numeric-grid stream-pacing__values">
                <label class="vs-field" for="browser-stream-pacing-slack">
                  <span class="vs-field__label">{{ t('webrtc.frame_pacing_slack') }}</span>
                  <input
                    id="browser-stream-pacing-slack"
                    v-model.number="form.videoPacingSlackMs"
                    class="vs-input"
                    type="number"
                    min="0"
                    max="10"
                    step="1"
                  />
                  <span class="vs-field__help">0–10 ms</span>
                </label>
                <label class="vs-field" for="browser-stream-max-frame-age">
                  <span class="vs-field__label">{{ t('webrtc.frame_pacing_max_delay') }}</span>
                  <input
                    id="browser-stream-max-frame-age"
                    v-model.number="maxFrameAgeFrames"
                    class="vs-input"
                    type="number"
                    min="1"
                    :max="maxAllowedFramesForFps(form.fps)"
                    step="1"
                  />
                  <span class="vs-field__help">
                    {{
                      t('ui.browser_stream.settings.frame_age_help', {
                        milliseconds: maxFrameAgeMsLabel,
                        fps: form.fps,
                      })
                    }}
                  </span>
                </label>
              </div>
            </div>
          </fieldset>

          <label
            class="stream-form__check"
            :class="{ 'stream-form__check--disabled': connectionPending || isConnected }"
          >
            <input
              v-model="form.muteHostAudio"
              type="checkbox"
              :disabled="connectionPending || isConnected"
            />
            <span>
              <strong>{{ t('ui.browser_stream.settings.mute_host_audio') }}</strong>
              <small>{{ t('ui.browser_stream.settings.mute_host_audio_help') }}</small>
            </span>
          </label>

          <p v-if="validationError" class="stream-form__validation" role="status">
            <UiIcon name="alert-triangle" :size="16" />
            {{ validationError }}
          </p>
        </form>
      </section>

      <section class="panel" aria-labelledby="browser-stream-controls-title">
        <div class="panel__heading">
          <div>
            <h2 id="browser-stream-controls-title">{{ t('ui.browser_stream.controls.title') }}</h2>
            <p>{{ t('ui.browser_stream.controls.description') }}</p>
          </div>
        </div>

        <label class="stream-form__check" :class="{ 'stream-form__check--disabled': !isConnected }">
          <input v-model="inputForwarding" type="checkbox" :disabled="!isConnected" />
          <span>
            <strong>{{ t('ui.browser_stream.controls.input') }}</strong>
            <small>{{ t('ui.browser_stream.controls.input_help') }}</small>
          </span>
        </label>

        <label class="stream-form__check" :class="{ 'stream-form__check--disabled': !isConnected }">
          <input v-model="showPerformanceOverlay" type="checkbox" :disabled="!isConnected" />
          <span>
            <strong>{{ t('webrtc.show_performance_overlay') }}</strong>
            <small>{{ t('ui.browser_stream.controls.performance_overlay_help') }}</small>
          </span>
        </label>

        <label class="stream-form__check">
          <input v-model="autoFullscreen" type="checkbox" />
          <span>
            <strong>{{ t('webrtc.auto_fullscreen') }}</strong>
            <small>{{ t('webrtc.auto_fullscreen_desc') }}</small>
          </span>
        </label>

        <div class="browser-capabilities" aria-labelledby="browser-stream-capabilities-title">
          <h3 id="browser-stream-capabilities-title">
            {{ t('ui.browser_stream.capabilities.title') }}
          </h3>
          <ul>
            <li v-for="codec in codecs" :key="codec">
              <span>{{ codecLabel(codec) }}</span>
              <StatusBadge
                :label="
                  codecAvailable(codec)
                    ? t('ui.browser_stream.capabilities.available')
                    : t('ui.browser_stream.capabilities.unavailable')
                "
                :tone="codecAvailable(codec) ? 'success' : 'neutral'"
                compact
              />
              <small
                v-if="
                  codecAvailable(codec) &&
                  hostCapabilities.codecs[codec].hdr &&
                  browserCapabilities[codec].hdr
                "
              >
                {{ t('ui.browser_stream.capabilities.hdr_ready') }}
              </small>
              <small v-else>
                {{
                  codecAvailable(codec)
                    ? t('ui.browser_stream.capabilities.sdr_only')
                    : codecUnavailableReason(codec)
                }}
              </small>
            </li>
          </ul>
        </div>
        <div class="conn-test" aria-labelledby="browser-stream-conn-title">
          <h3 id="browser-stream-conn-title">
            {{ t('ui.browser_stream.conn.title', 'Connection test') }}
          </h3>
          <p class="conn-test__help">
            {{
              t(
                'ui.browser_stream.conn.help',
                'Checks which route a stream would take. Starts no stream and uses no invite.',
              )
            }}
          </p>
          <AppButton
            variant="secondary"
            :disabled="connTesting"
            :busy="connTesting"
            @click="runConnectionTest"
          >
            {{
              connTesting
                ? t('ui.browser_stream.conn.testing', 'Testing…')
                : t('ui.browser_stream.conn.run', 'Test connection')
            }}
          </AppButton>

          <InlineAlert v-if="connError" tone="danger">{{ connError }}</InlineAlert>

          <div v-if="connReport" class="conn-test__result">
            <StatusBadge :label="connReport.verdict" :tone="connTone" compact />
            <p class="conn-test__summary">{{ connSummary }}</p>
            <ul class="conn-test__facts">
              <li>
                <span>{{ t('ui.browser_stream.conn.local', 'Local network') }}</span>
                <small>{{ connReport.hasHost ? connReport.counts.host : 0 }}</small>
              </li>
              <li>
                <span>{{ t('ui.browser_stream.conn.stun', 'STUN (public address)') }}</span>
                <small>{{ connReport.publicAddress ?? '—' }}</small>
              </li>
              <li>
                <span>{{ t('ui.browser_stream.conn.turn', 'TURN relay') }}</span>
                <small>
                  {{
                    connReport.relayOnlyWorks
                      ? `${connReport.relayLatencyMs ?? '?'} ms`
                      : t('ui.browser_stream.conn.none', 'unavailable')
                  }}
                </small>
              </li>
            </ul>
            <!-- The error codes are the useful part when something is wrong: 401 is
                 TURN refusing the credentials, which candidate counts never show.
                 Informational ones are shown but never in red — a 701 against a
                 host that connected is its missing IPv6 record, not a fault, and
                 colouring it as one sends people chasing a problem they do not
                 have. -->
            <ul v-if="connReport.errors.length" class="conn-test__errors">
              <li
                v-for="(e, i) in connReport.errors"
                :key="i"
                :class="{ 'conn-test__errors--info': e.informational }"
              >
                {{ e.url }} — {{ e.errorCode }} {{ e.errorText }}
                <template v-if="e.informational">
                  {{
                    t(
                      'ui.browser_stream.conn.ipv6_note',
                      '(expected — this server has no IPv6 address; the IPv4 connection succeeded)',
                    )
                  }}
                </template>
              </li>
            </ul>
          </div>
        </div>
      </section>
    </div>

    <ConfirmDialog
      v-model:open="installHelpOpen"
      :title="t('ui.browser_stream.install.title')"
      :description="t('ui.browser_stream.install.description')"
      :confirm-label="t('ui.browser_stream.install.done')"
      :cancel-label="t('_common.cancel')"
      initial-focus="confirm"
    />

    <ConfirmDialog
      v-model:open="terminateOpen"
      :title="t('webrtc.terminate_confirm_title')"
      :description="terminateDescription"
      :confirm-label="terminateConfirmLabel"
      :cancel-label="t('_common.cancel')"
      tone="danger"
      :busy="sessionActionPending"
      :busy-label="terminateConfirmLabel"
      :close-on-confirm="false"
      @confirm="confirmTerminate"
    />
  </div>
</template>

<style scoped>
.stream-stats {
  position: absolute;
  top: 0.75rem;
  right: 0.75rem;
  z-index: 5;
  padding: 0.5rem 0.75rem;
  border-radius: var(--vs-radius-control);
  color: #fff;
  font-family: var(--vs-font-mono, monospace);
  font-size: 0.6875rem;
  line-height: 1.45;
  background: rgb(0 0 0 / 72%);
  pointer-events: none;
}

.browser-stream-page {
  display: grid;
  gap: var(--vs-space-24);
}

.app-picker {
  display: grid;
  gap: var(--vs-space-16);
}

.app-picker__heading {
  align-items: end;
  margin-bottom: 0;
}

.app-picker__search {
  position: relative;
  display: flex;
  min-width: min(16rem, 100%);
  align-items: center;
}

.app-picker__search > .vs-icon {
  position: absolute;
  left: var(--vs-space-12);
  z-index: 1;
  color: var(--vs-color-text-muted);
  pointer-events: none;
}

.app-picker__search .vs-input {
  width: 100%;
  padding-left: 2.35rem;
}

.app-picker__grid {
  display: flex;
  flex-wrap: wrap;
  gap: var(--vs-space-8);
}

.app-picker__card {
  display: inline-flex;
  min-width: 0;
  align-items: center;
  gap: var(--vs-space-8);
  padding: var(--vs-space-4) var(--vs-space-12) var(--vs-space-4) var(--vs-space-4);
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-pill);
  background: var(--vs-color-bg-surface);
  color: var(--vs-color-text-primary);
  text-align: left;
  cursor: pointer;
  transition: border-color var(--vs-motion-duration-control) var(--vs-motion-easing-standard);
}

.app-picker__card:hover:not(:disabled),
.app-picker__card:focus-visible,
.app-picker__card--selected {
  border-color: var(--vs-color-accent-default);
}

.app-picker__card--selected {
  background: color-mix(in srgb, var(--vs-color-accent-default) 8%, var(--vs-color-bg-surface));
}

.app-picker__card:disabled {
  cursor: not-allowed;
  opacity: 0.66;
}

.app-picker__card strong {
  overflow: hidden;
  max-width: 14rem;
  font-size: var(--vs-type-size-control);
  text-overflow: ellipsis;
  white-space: nowrap;
}

.app-picker__check {
  color: var(--vs-color-accent-default);
}

/* A square, not a portrait slab.
 *
 * These were 2rem x 2.65rem, the aspect of box art, back when the picker was a grid
 * of large cover cards. In a pill-shaped chip that reads as a tall grey rectangle
 * wedged into a rounded row — and for the many apps with no cover at all it was an
 * empty block with a small glyph floating in it. A square sits inside the pill's
 * radius without fighting it, and the covers that do exist still fill it. */
.conn-test {
  display: grid;
  gap: var(--vs-space-8);
  padding-block-start: var(--vs-space-16);
  border-block-start: var(--vs-border-width) solid var(--vs-color-border-subtle);
}

.conn-test__help,
.conn-test__summary {
  margin: 0;
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
}

.conn-test__result {
  display: grid;
  gap: var(--vs-space-8);
}

.conn-test__facts,
.conn-test__errors {
  display: grid;
  gap: var(--vs-space-4);
  padding: 0;
  margin: 0;
  list-style: none;
  font-size: var(--vs-type-size-helper);
}

.conn-test__facts li {
  display: flex;
  justify-content: space-between;
  gap: var(--vs-space-8);
}

.conn-test__facts small {
  color: var(--vs-color-text-secondary);
  font-variant-numeric: tabular-nums;
}

.conn-test__errors li {
  color: var(--vs-color-status-danger);
  overflow-wrap: anywhere;
}

.conn-test__errors--info {
  color: var(--vs-color-text-secondary);
}

.app-picker__artwork {
  display: grid;
  overflow: hidden;
  width: 1.75rem;
  height: 1.75rem;
  flex: none;
  place-items: stretch;
  border-radius: 50%;
  background: var(--vs-color-bg-subtle);
}

.app-picker__artwork img {
  width: 100%;
  height: 100%;
  object-fit: cover;
}

/* The circle is kept even with no cover to put in it: an app with artwork and one
 * without then read as the same kind of thing, the way avatars do, instead of some
 * chips carrying a disc and others a bare glyph. */
.app-picker__artwork--desktop,
.app-picker__artwork-fallback {
  place-items: center;
  color: var(--vs-color-text-muted);
}

.app-picker__empty {
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
}

.app-picker__empty {
  margin: 0;
}

.stream-stage {
  display: grid;
  gap: var(--vs-space-16);
}

.stream-stage__heading {
  margin-bottom: 0;
}

.stream-stage__heading-actions {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  justify-content: flex-end;
  gap: var(--vs-space-8);
}

.stream-stage__heading-actions [aria-controls='browser-stream-surface'] .vs-button__icon {
  transition: transform 160ms ease;
}

.stream-stage__heading-actions
  [aria-controls='browser-stream-surface'][aria-expanded='true']
  .vs-button__icon {
  transform: rotate(180deg);
}

.stream-surface {
  /* One colour for the stage, defined once.
     It used to be written as #090b10 here and #000 in the fullscreen rule, so the
     same surface was two different blacks depending on mode, and the letterboxing
     around a video that does not match the stage aspect showed whichever one the
     rule underneath happened to set. The video element gets it too: its own
     background is transparent, so before the first frame arrives — which is
     exactly the loading case — whatever sits behind it shows through, and that
     has to be the same colour or the seam is visible. */
  --stream-stage-bg: #090b10;

  position: relative;
  display: grid;
  min-height: min(60vw, 42rem);
  overflow: hidden;
  place-items: center;
  border: 1px solid var(--vs-color-border-strong);
  border-radius: var(--vs-radius-card);
  outline: none;
  background: var(--stream-stage-bg);
}

.stream-surface:focus-visible {
  box-shadow: 0 0 0 3px color-mix(in srgb, var(--vs-color-accent-default) 44%, transparent);
}

.stream-surface--interactive {
  cursor: none;
  overscroll-behavior: none;
  touch-action: none;
  -webkit-touch-callout: none;
  user-select: none;
}

.stream-surface video {
  display: block;
  background: var(--stream-stage-bg);
  width: 100%;
  height: 100%;
  max-height: 42rem;
  object-fit: contain;
}

/* Sits exactly over the video, which keeps playing underneath as the decode
   surface. Never takes pointer events — the surface below owns input forwarding. */
.stream-surface__fx {
  position: absolute;
  inset: 0;
  width: 100%;
  height: 100%;
  pointer-events: none;
}

.stream-performance-overlay {
  position: absolute;
  z-index: 1;
  top: var(--vs-space-12);
  left: var(--vs-space-12);
  max-width: calc(100% - var(--vs-space-24));
  padding: var(--vs-space-8) var(--vs-space-12);
  border: 1px solid rgb(255 255 255 / 0.16);
  border-radius: var(--vs-radius-control);
  background: rgb(0 0 0 / 0.72);
  box-shadow: 0 0.25rem 1rem rgb(0 0 0 / 0.22);
  color: rgb(255 255 255 / 0.9);
  pointer-events: none;
  backdrop-filter: blur(8px);
}

/* Keep Safari's top-edge fullscreen exit affordance visible on touch devices. */
.stream-surface--touch-exit .stream-performance-overlay {
  top: max(4.75rem, calc(env(safe-area-inset-top) + 3.25rem));
}

.stream-performance-overlay__line {
  overflow-wrap: anywhere;
  font-family: ui-monospace, SFMono-Regular, Consolas, monospace;
  font-size: 0.72rem;
  line-height: 1.55;
}

.stream-surface:fullscreen,
.stream-surface:-webkit-full-screen,
.stream-surface--pseudo-fullscreen {
  width: 100vw;
  height: 100vh;
  height: 100dvh;
  min-height: 0;
  border: 0;
  border-radius: 0;
  background: var(--stream-stage-bg);
}

.stream-surface--pseudo-fullscreen {
  position: fixed;
  z-index: 10000;
  width: 100lvw;
  height: 100lvh;
  inset: 0;
}

.stream-surface:fullscreen video,
.stream-surface:-webkit-full-screen video,
.stream-surface--pseudo-fullscreen video,
.stream-surface video:fullscreen,
.stream-surface video:-webkit-full-screen {
  width: 100%;
  height: 100%;
  max-height: none;
  object-fit: contain;
}

.stream-surface__empty {
  position: absolute;
  inset: 0;
  display: grid;
  align-content: center;
  justify-items: center;
  gap: var(--vs-space-8);
  padding: var(--vs-space-24);
  color: rgb(255 255 255 / 0.82);
  text-align: center;
}

.stream-surface__exit-fullscreen {
  position: absolute;
  display: flex;
  align-items: center;
  gap: var(--vs-space-8);
  z-index: 2;
  right: max(var(--vs-space-16), env(safe-area-inset-right));
  bottom: max(var(--vs-space-16), env(safe-area-inset-bottom));
  cursor: default;
}

/* Clear of the controller, which now claims every corner: sticks and clusters
   along the bottom, bumpers and triggers along the top edge at both ends. Moving
   this to the top right only traded a collision with A and B for one with RT and
   RB. What the pad leaves free is the middle of the top edge, under its own row
   of small controls — so this sits there, and shrinks to match them, because a
   full-size button in that gap would reach the triggers again. */
.stream-surface__exit-fullscreen--clear-of-pad {
  top: calc(max(var(--vs-space-16), env(safe-area-inset-top)) + 2.25rem);
  right: auto;
  bottom: auto;
  left: 50%;
  translate: -50% 0;
}

.stream-surface__exit-fullscreen--clear-of-pad :where(button, a) {
  min-height: 0;
  padding: 5px 10px;
  border-radius: 999px;
  font-size: 11px;
}

.stream-surface__overlay-toggle {
  display: inline-flex;
  align-items: center;
  gap: var(--vs-space-4);
  min-height: 2.25rem;
  padding: 0 var(--vs-space-8);
  border: 1px solid rgb(255 255 255 / 0.18);
  border-radius: var(--vs-radius-control);
  background: rgb(10 12 18 / 0.68);
  color: rgb(255 255 255 / 0.88);
  cursor: pointer;
  font-size: var(--vs-type-size-helper);
  touch-action: manipulation;
  backdrop-filter: blur(10px);
}

.stream-surface__overlay-toggle input {
  width: 1rem;
  height: 1rem;
  margin: 0;
  accent-color: var(--vs-color-accent-default);
}

.stream-surface__exit-swipe {
  position: absolute;
  z-index: 2;
  top: max(var(--vs-space-12), env(safe-area-inset-top));
  left: 50%;
  display: grid;
  width: min(13rem, 50vw);
  min-height: 2.75rem;
  padding: var(--vs-space-8) var(--vs-space-12);
  transform: translateX(-50%);
  place-items: center;
  gap: var(--vs-space-4);
  border: 1px solid rgb(255 255 255 / 0.18);
  border-radius: 999px;
  background: rgb(10 12 18 / 0.68);
  color: rgb(255 255 255 / 0.82);
  font-size: var(--vs-type-size-helper);
  cursor: default;
  touch-action: none;
  backdrop-filter: blur(10px);
}

.stream-surface__exit-swipe span {
  width: 2.25rem;
  height: 0.2rem;
  border-radius: 999px;
  background: rgb(255 255 255 / 0.72);
}

.stream-surface__empty span:not(.stream-surface__empty-icon):not(.stream-surface__spinner) {
  max-width: 28rem;
  color: rgb(255 255 255 / 0.66);
}

.stream-surface__empty-icon {
  display: grid;
  width: 3.25rem;
  height: 3.25rem;
  place-items: center;
  border: 1px solid rgb(255 255 255 / 0.26);
  border-radius: 50%;
  background: rgb(255 255 255 / 0.1);
}

.stream-surface__spinner {
  width: 2rem;
  height: 2rem;
  border: 2px solid rgb(255 255 255 / 0.28);
  border-right-color: rgb(255 255 255 / 0.9);
  border-radius: 50%;
  animation: stream-spin 0.8s linear infinite;
}

.stream-stage__actions {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: var(--vs-space-8);
}

.stream-stage__input-status {
  display: inline-flex;
  align-items: center;
  gap: var(--vs-space-4);
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-helper);
}

.stream-stage__input-status[data-ready='true'] {
  color: var(--vs-color-status-success);
}

.stream-stage__gamepad-status {
  display: inline-flex;
  align-items: center;
  gap: var(--vs-space-4);
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-helper);
}

.stream-stage__gamepad-status[data-connected='true'] {
  color: var(--vs-color-status-success);
}

/* aria-pressed carries the state for assistive tech; the fill carries it visually.
   A pressed toggle has to be unmistakable at icon size, so it inverts rather than
   shifting a border colour. */
/* Bars grow left to right and colour by tier. The unlit ones stay visible so the
   indicator reads as "2 of 4" rather than as two floating marks. */
.stream-stage__link {
  display: inline-flex;
  align-items: center;
  gap: var(--vs-space-8);
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
  white-space: nowrap;
}

.stream-stage__link-bars {
  display: inline-flex;
  align-items: flex-end;
  gap: 2px;
  height: 1rem;
}

.stream-stage__link-bars i {
  width: 3px;
  border-radius: 1px;
  background: var(--vs-color-border-strong);
}

.stream-stage__link-bars i:nth-child(1) { height: 25%; }
.stream-stage__link-bars i:nth-child(2) { height: 50%; }
.stream-stage__link-bars i:nth-child(3) { height: 75%; }
.stream-stage__link-bars i:nth-child(4) { height: 100%; }

.stream-stage__link[data-tier='excellent'] .is-lit,
.stream-stage__link[data-tier='good'] .is-lit {
  background: var(--vs-color-status-success);
}

.stream-stage__link[data-tier='fair'] .is-lit {
  background: var(--vs-color-status-warning);
}

.stream-stage__link[data-tier='poor'] .is-lit {
  background: var(--vs-color-status-danger);
}

.stream-stage__toggles {
  display: flex;
  gap: var(--vs-space-4);
}

.stream-stage__toggle {
  display: grid;
  width: 2rem;
  height: 2rem;
  place-items: center;
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-control);
  background: var(--vs-color-bg-surface);
  color: var(--vs-color-text-secondary);
  cursor: pointer;
  transition:
    background-color var(--vs-motion-duration-control) var(--vs-motion-easing-standard),
    border-color var(--vs-motion-duration-control) var(--vs-motion-easing-standard),
    color var(--vs-motion-duration-control) var(--vs-motion-easing-standard);
}

/* A wash, not the accent.
 *
 * The accent is #0A0A0A in the light theme, so putting it on the border and the
 * glyph turned a 2rem button into a black ring around a black icon — which both
 * read as "this went black" and, worse, looked almost exactly like the pressed
 * state below. Hover has to say "you are over this", not "this is on". Grey says
 * the first; black is reserved for the second. */
.stream-stage__toggle:hover:not(:disabled) {
  background: var(--vs-color-bg-subtle);
  color: var(--vs-color-text-primary);
}

/* Already on: darken rather than lighten, so hovering never looks like turning
   it off. */
.stream-stage__toggle--on:hover:not(:disabled) {
  background: var(--vs-color-accent-hover);
  color: var(--vs-color-text-on-accent);
}

.stream-stage__toggle--on {
  border-color: var(--vs-color-accent-default);
  background: var(--vs-color-accent-default);
  color: var(--vs-color-text-on-accent);
}

.stream-stage__toggle:disabled {
  cursor: not-allowed;
  opacity: 0.45;
}

.stream-stage__volume {
  display: inline-flex;
  align-items: center;
  gap: var(--vs-space-8);
  margin-inline-start: auto;
  color: var(--vs-color-text-muted);
  cursor: pointer;
}

.stream-stage__volume .vs-range {
  width: 7.5rem;
}

.stream-stage__volume-value {
  min-width: 2.6rem;
  font-size: var(--vs-type-size-helper);
  font-variant-numeric: tabular-nums;
  text-align: right;
}

.browser-stream-loading,
.browser-stream-grid {
  display: grid;
  grid-template-columns: minmax(0, 1.25fr) minmax(18rem, 0.75fr);
  gap: var(--vs-space-24);
}

.stream-form,
.stream-form__group {
  display: grid;
  gap: var(--vs-space-16);
}

.stream-form__group {
  padding: var(--vs-space-16);
  border: 1px solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-control);
}

.stream-form__group legend {
  padding-inline: var(--vs-space-4);
  color: var(--vs-color-text-primary);
  font-size: var(--vs-type-size-control);
  font-weight: var(--vs-type-weight-semibold);
}

.stream-form__select-grid {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: var(--vs-space-12) var(--vs-space-16);
}

.stream-form__span {
  grid-column: 1 / -1;
}

.stream-form__numeric-grid {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: var(--vs-space-12);
}

.stream-form__group-help {
  margin: 0 0 var(--vs-space-8);
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
}

.stream-form__toggles {
  display: flex;
  flex-wrap: wrap;
  gap: var(--vs-space-8) var(--vs-space-24);
}

.stream-form__check {
  display: flex;
  align-items: flex-start;
  gap: var(--vs-space-12);
  padding: var(--vs-space-12);
  border: 1px solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-control);
  cursor: pointer;
}

.stream-form__check input {
  width: 1rem;
  height: 1rem;
  margin-top: 0.15rem;
  accent-color: var(--vs-color-accent-default);
}

.stream-form__check span {
  display: grid;
  gap: var(--vs-space-2);
}

.stream-form__check strong {
  color: var(--vs-color-text-primary);
  font-size: var(--vs-type-size-control);
}

.stream-form__check small,
.vs-field__help,
.browser-capabilities small {
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
  line-height: 1.4;
}

.stream-form__check--plain {
  padding: 0;
  border: 0;
  border-radius: 0;
  align-items: center;
}

.stream-form__check--plain input {
  margin-top: 0;
}

.stream-form__check--disabled {
  cursor: not-allowed;
  opacity: 0.68;
}

.stream-form__validation {
  display: flex;
  align-items: flex-start;
  gap: var(--vs-space-8);
  margin: 0;
  color: var(--vs-color-status-warning);
  font-size: var(--vs-type-size-helper);
  line-height: 1.4;
}

.stream-form__bitrate-help {
  display: inline-flex;
  align-items: center;
  gap: var(--vs-space-8);
}

.stream-pacing {
  display: grid;
  gap: var(--vs-space-12);
  padding-top: var(--vs-space-4);
  border-top: 1px solid var(--vs-color-border-subtle);
}

.stream-pacing__heading {
  display: grid;
  gap: var(--vs-space-4);
}

.stream-pacing__heading small {
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
  line-height: 1.4;
}

.stream-pacing__presets {
  display: flex;
  flex-wrap: wrap;
  gap: var(--vs-space-8);
}

.stream-pacing__preset {
  min-height: 2.25rem;
  padding: 0 var(--vs-space-12);
  border: 1px solid var(--vs-color-border-strong);
  border-radius: var(--vs-radius-control);
  background: var(--vs-color-bg-surface);
  color: var(--vs-color-text-secondary);
  cursor: pointer;
  font: inherit;
}

.stream-pacing__preset:hover,
.stream-pacing__preset:focus-visible,
.stream-pacing__preset--selected {
  border-color: var(--vs-color-accent-default);
  color: var(--vs-color-text-primary);
}

.stream-pacing__preset--selected {
  background: color-mix(in srgb, var(--vs-color-accent-default) 14%, transparent);
}

.stream-pacing__values {
  gap: var(--vs-space-12);
}

.browser-capabilities {
  margin-top: var(--vs-space-24);
}

.browser-capabilities h3 {
  margin: 0 0 var(--vs-space-12);
  font-size: var(--vs-type-size-control);
}

.browser-capabilities ul {
  display: grid;
  gap: var(--vs-space-8);
  padding: 0;
  margin: 0;
  list-style: none;
}

.browser-capabilities li {
  display: grid;
  grid-template-columns: minmax(4.5rem, auto) auto minmax(0, 1fr);
  align-items: center;
  gap: var(--vs-space-8);
  padding: var(--vs-space-8) 0;
  border-top: 1px solid var(--vs-color-border-subtle);
}

@keyframes stream-spin {
  to {
    transform: rotate(1turn);
  }
}

@media (max-width: 1023px) {
  .browser-stream-loading,
  .browser-stream-grid {
    grid-template-columns: 1fr;
  }
}

@media (max-width: 767px) {
  .app-picker__heading {
    align-items: stretch;
  }

  .app-picker__search {
    min-width: 0;
  }

  .stream-surface {
    min-height: 15rem;
  }

  .stream-stage__volume {
    margin-inline-start: 0;
  }

  .stream-stage__input-status {
    width: 100%;
  }

  .stream-form__select-grid,
  .stream-form__numeric-grid {
    grid-template-columns: 1fr;
  }

  .browser-capabilities li {
    grid-template-columns: minmax(0, 1fr) auto;
  }

  .browser-capabilities small {
    grid-column: 1 / -1;
  }
}

@media (prefers-reduced-motion: reduce) {
  .stream-surface__spinner {
    animation: none;
  }
}
</style>
