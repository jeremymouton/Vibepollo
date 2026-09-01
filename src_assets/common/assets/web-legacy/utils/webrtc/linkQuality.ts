/**
 * Is this link good enough to play on?
 *
 * The stats overlay answers that only for someone who already knows what "rtt
 * 84ms · jitter 19ms · lost 412" means. Everyone else — including the guest, who
 * is the person least able to do anything about it and most likely to be the one
 * suffering — sees a wall of numbers and no verdict. This turns the same figures
 * into one of four answers and, more usefully, into WHICH measurement is the
 * problem, because the fix differs completely: latency is distance and routing,
 * jitter is usually Wi-Fi, and loss is a saturated or lossy link.
 *
 * Shared between the admin page and the guest player deliberately. Rumble and FSR
 * were both written for one surface and silently missing from the other for
 * weeks; a verdict that disagreed between the two would be worse than either.
 *
 * The thresholds are for interactive game streaming, which is far less forgiving
 * than video calling — the numbers a conferencing tool would call "good" are
 * where a game starts feeling loose.
 */

export type LinkQualityTier = 'excellent' | 'good' | 'fair' | 'poor' | 'unknown';

/** Which measurement dragged the verdict down, so advice can be specific. */
export type LinkQualityCause = 'latency' | 'jitter' | 'loss' | null;

export interface LinkQualityInput {
  roundTripMs?: number | undefined;
  jitterMs?: number | undefined;
  /** Packet loss over the last sampling window, as a percentage. */
  lossPercent?: number | undefined;
  /** True when media is going through a TURN relay rather than peer to peer. */
  relayed?: boolean | undefined;
}

export interface LinkQualityVerdict {
  tier: LinkQualityTier;
  /** 0-4, for a signal-bars style indicator. */
  bars: number;
  cause: LinkQualityCause;
  /** Two or three words, for the indicator itself. */
  headline: string;
  /** One sentence naming the actual problem and what it means. */
  detail: string;
}

const TIER_BARS: Record<LinkQualityTier, number> = {
  excellent: 4,
  good: 3,
  fair: 2,
  poor: 1,
  unknown: 0,
};

const TIER_ORDER: LinkQualityTier[] = ['excellent', 'good', 'fair', 'poor'];

/**
 * Thresholds per measurement, worst-first.
 *
 * Round trip is the one people feel as "input lag": under 30ms is
 * indistinguishable from local, and by 100ms a fast game is visibly behind the
 * controller. Jitter matters more than its size suggests, because a jitter buffer
 * absorbs it by ADDING latency — 30ms of jitter costs more than 30ms of steady
 * delay. Loss is worst of all for a stream: a dropped packet is a damaged frame,
 * and the decoder either shows the damage or freezes until the next keyframe.
 */
function tierFor(value: number, bounds: [number, number, number]): LinkQualityTier {
  const [excellent, good, fair] = bounds;
  if (value <= excellent) return 'excellent';
  if (value <= good) return 'good';
  if (value <= fair) return 'fair';
  return 'poor';
}

function worseOf(a: LinkQualityTier, b: LinkQualityTier): LinkQualityTier {
  if (a === 'unknown') return b;
  if (b === 'unknown') return a;
  return TIER_ORDER.indexOf(a) > TIER_ORDER.indexOf(b) ? a : b;
}

/**
 * The verdict is the WORST of the three, never an average.
 *
 * Averaging hides exactly the case that matters: a link with perfect latency and
 * 4% packet loss is unplayable, and would score "good" on any mean. The one bad
 * measurement is the experience.
 */
export function assessLinkQuality(input: LinkQualityInput): LinkQualityVerdict {
  const measurements: Array<{
    cause: Exclude<LinkQualityCause, null>;
    tier: LinkQualityTier;
    value: number;
  }> = [];

  if (typeof input.roundTripMs === 'number' && Number.isFinite(input.roundTripMs)) {
    measurements.push({
      cause: 'latency',
      tier: tierFor(input.roundTripMs, [30, 60, 100]),
      value: input.roundTripMs,
    });
  }
  if (typeof input.jitterMs === 'number' && Number.isFinite(input.jitterMs)) {
    measurements.push({
      cause: 'jitter',
      tier: tierFor(input.jitterMs, [5, 15, 30]),
      value: input.jitterMs,
    });
  }
  if (typeof input.lossPercent === 'number' && Number.isFinite(input.lossPercent)) {
    measurements.push({
      cause: 'loss',
      tier: tierFor(input.lossPercent, [0.1, 0.5, 2]),
      value: input.lossPercent,
    });
  }

  if (!measurements.length) {
    return {
      tier: 'unknown',
      bars: 0,
      cause: null,
      headline: 'Measuring…',
      detail: 'Not enough data yet to judge the connection.',
    };
  }

  const tier = measurements.map((m) => m.tier).reduce(worseOf, 'unknown');
  // Name the measurement that set the verdict, not merely the largest number —
  // 90ms of latency and 3% loss are not comparable as raw values.
  const culprit = measurements.filter((m) => m.tier === tier).sort((a, b) => b.value - a.value)[0];
  const cause = tier === 'excellent' ? null : (culprit?.cause ?? null);

  const detail = ((): string => {
    if (tier === 'excellent') {
      return input.relayed
        ? 'Relayed through the server, but comfortably fast enough.'
        : 'Direct connection, comfortably fast enough.';
    }
    const value = culprit ? Math.round(culprit.value * 10) / 10 : 0;
    switch (cause) {
      case 'latency':
        return `Round trip to the host is ${value} ms, so input will feel behind. Distance or routing — a VPN or a far-away relay is the usual cause.`;
      case 'jitter':
        return `Timing varies by ${value} ms, which is absorbed by delaying playback. Wi-Fi is the usual cause; a cable fixes it more often than any setting.`;
      case 'loss':
        return `${value}% of packets are not arriving, which damages frames. Usually a saturated link — lowering the bitrate often helps more than raising it.`;
      default:
        return 'Connection quality is reduced.';
    }
  })();

  const headline = {
    excellent: 'Excellent',
    good: 'Good',
    fair: 'Fair',
    poor: 'Poor',
    unknown: 'Measuring…',
  }[tier];

  return { tier, bars: TIER_BARS[tier], cause, headline, detail };
}
