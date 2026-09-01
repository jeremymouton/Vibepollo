/**
 * Connectivity probe — what route would a stream actually take?
 *
 * This exists because "it's laggy" and "it won't connect" are indistinguishable
 * from the outside, and the fact that separates them is not a latency number: it
 * is which ICE candidate pair the connection would settle on. A guest relayed
 * through the droplet and a guest connected directly report the same symptom and
 * need completely different fixes.
 *
 * It deliberately does NOT create a stream session. Session creation starts an
 * encoder, and a session whose signalling stalls leaves that encoder running
 * against a queue nobody drains — the failure that took the host down on
 * 2026-09-01. A reachability check must not be able to cause an outage. So this
 * talks only to the ICE servers: it gathers candidates and reports what came
 * back, which is enough to answer every question below without the host ever
 * allocating anything.
 *
 * What it can prove:
 *   - host candidates missing        -> the browser is suppressing them, which is
 *                                       what a VPN extension's "WebRTC leak
 *                                       protection" does, and why the same stream
 *                                       is fine in Safari and bad in Chrome.
 *   - no srflx candidate             -> STUN is unreachable; nothing will connect
 *                                       except on the same LAN.
 *   - no relay candidate             -> TURN is unreachable or refusing the
 *                                       credentials, so any guest who cannot go
 *                                       direct has no fallback at all.
 *
 * What it cannot prove: that the gaming PC itself is reachable. Only a real
 * session pairs candidates end to end. This answers "can I get out of here and
 * back", not "is the far end up".
 */

export type CandidateKind = 'host' | 'srflx' | 'prflx' | 'relay';

export interface IceServerError {
  /** STUN/TURN URL that failed, as the browser reported it. */
  url: string;
  /** 401 means the TURN credentials were rejected. */
  errorCode: number;
  errorText: string;
  /**
   * Not a fault: expected noise that would otherwise read as a failure.
   *
   * Browsers resolve each ICE server once per address family and report a 701
   * for the family that has no record. A host with an A record and no AAAA —
   * which is the normal case — therefore emits a 701 for its IPv6 attempt on
   * every single probe, while IPv4 connects perfectly. Showing that in red
   * sends people hunting for a network fault that does not exist, so it is
   * flagged here and de-emphasised in the UI rather than dropped, because on a
   * probe where nothing connected the same line is the actual answer.
   */
  informational: boolean;
}

export interface ConnectivityReport {
  /** Candidate counts by type, over the unrestricted pass. */
  counts: Record<CandidateKind, number>;
  /** A local network path exists. Absent almost always means the browser hid it. */
  hasHost: boolean;
  /** STUN answered, so the public address is known. */
  hasStun: boolean;
  /** TURN allocated a relay, so a guest who cannot go direct still has a route. */
  hasRelay: boolean;
  /** The relay-only pass allocated — proves TURN in isolation, not just as a bonus. */
  relayOnlyWorks: boolean;
  /** Public address as STUN saw it, when it reported one. */
  publicAddress: string | null;
  /** Milliseconds to first relay candidate; a proxy for how far the TURN server is. */
  relayLatencyMs: number | null;
  /** Total gathering time for the unrestricted pass. */
  gatheringMs: number;
  /** Per-URL failures. The most useful field when something is wrong. */
  errors: IceServerError[];
  /** One-line summary suitable for showing a non-technical guest. */
  verdict: 'good' | 'relay-only' | 'degraded' | 'blocked';
}

/** Gathering rarely needs this long; past it, something is not answering. */
const GATHER_TIMEOUT_MS = 8000;

function emptyCounts(): Record<CandidateKind, number> {
  return { host: 0, srflx: 0, prflx: 0, relay: 0 };
}

interface GatherOutcome {
  counts: Record<CandidateKind, number>;
  publicAddress: string | null;
  relayLatencyMs: number | null;
  elapsedMs: number;
  errors: IceServerError[];
}

/**
 * One gathering pass.
 *
 * A data channel is created purely to give the connection something to gather
 * for — an offer with no media and no channels produces no candidates at all.
 */
async function gather(
  iceServers: RTCIceServer[],
  policy: RTCIceTransportPolicy,
): Promise<GatherOutcome> {
  const counts = emptyCounts();
  const errors: IceServerError[] = [];
  let publicAddress: string | null = null;
  let relayLatencyMs: number | null = null;

  const started = performance.now();
  const pc = new RTCPeerConnection({ iceServers, iceTransportPolicy: policy });

  try {
    pc.createDataChannel('probe');

    pc.onicecandidate = (event) => {
      const candidate = event.candidate;
      if (!candidate || !candidate.candidate) return;
      const kind = candidate.type as CandidateKind | null;
      if (kind && kind in counts) {
        counts[kind] += 1;
      }
      if (kind === 'srflx' && !publicAddress) {
        publicAddress = candidate.address ?? null;
      }
      if (kind === 'relay' && relayLatencyMs === null) {
        relayLatencyMs = Math.round(performance.now() - started);
      }
    };

    // The single most diagnostic signal available: a 401 here says the TURN
    // credentials were refused, which no amount of candidate counting reveals.
    pc.onicecandidateerror = (event) => {
      const e = event as RTCPeerConnectionIceErrorEvent;
      errors.push({
        url: e.url ?? '(unknown)',
        errorCode: e.errorCode ?? 0,
        errorText: e.errorText ?? '',
        // Decided below, once it is known whether anything actually connected.
        informational: false,
      });
    };

    const offer = await pc.createOffer();
    await pc.setLocalDescription(offer);

    await new Promise<void>((resolve) => {
      const done = (): void => {
        clearTimeout(timer);
        resolve();
      };
      const timer = setTimeout(done, GATHER_TIMEOUT_MS);
      if (pc.iceGatheringState === 'complete') {
        done();
        return;
      }
      pc.onicegatheringstatechange = () => {
        if (pc.iceGatheringState === 'complete') done();
      };
    });

    return {
      counts,
      publicAddress,
      relayLatencyMs,
      elapsedMs: Math.round(performance.now() - started),
      errors,
    };
  } finally {
    // Always closed. A probe that leaked peer connections would be a worse bug
    // than the one it is here to diagnose.
    pc.onicecandidate = null;
    pc.onicecandidateerror = null;
    pc.onicegatheringstatechange = null;
    pc.close();
  }
}

/**
 * Read the two passes as one answer.
 *
 * Ordered by what the person running this can act on. No relay at all is worst:
 * it means anyone off the LAN has no fallback. Missing host candidates is called
 * out separately because the cause is local to the browser and fixable in a way
 * the others are not.
 */
function verdictFor(
  counts: Record<CandidateKind, number>,
  relayOnlyWorks: boolean,
): ConnectivityReport['verdict'] {
  const hasStun = counts.srflx > 0;
  const hasRelay = counts.relay > 0 || relayOnlyWorks;
  if (!hasStun && !hasRelay) return 'blocked';
  if (!hasRelay) return 'degraded';
  if (counts.host === 0) return 'relay-only';
  return 'good';
}

/**
 * Run the probe.
 *
 * Two passes rather than one: the unrestricted pass shows what a real connection
 * would have to choose from, and the relay-only pass proves TURN on its own. Both
 * are needed — a relay candidate can appear in the first pass and still not be
 * usable, and a guest who will certainly be relayed cares only about the second.
 */
export async function probeConnectivity(iceServers: RTCIceServer[]): Promise<ConnectivityReport> {
  const full = await gather(iceServers, 'all');

  // Only worth the second pass if TURN is configured at all.
  const turnConfigured = iceServers.some((server) => {
    const urls = Array.isArray(server.urls) ? server.urls : [server.urls];
    return urls.some((url) => typeof url === 'string' && url.startsWith('turn'));
  });
  let relayOnlyWorks = false;
  let relayErrors: IceServerError[] = [];
  if (turnConfigured) {
    const relayPass = await gather(iceServers, 'relay');
    relayOnlyWorks = relayPass.counts.relay > 0;
    relayErrors = relayPass.errors;
  }

  // De-duplicate: the same server failing in both passes is one problem, not two.
  const seen = new Set<string>();
  const errors = [...full.errors, ...relayErrors].filter((e) => {
    const key = `${e.url}#${e.errorCode}`;
    if (seen.has(key)) return false;
    seen.add(key);
    return true;
  });

  // A 701 only means something when nothing got through. If candidates were
  // gathered, the server was reachable and the 701 is the other address family.
  const connected = full.counts.srflx > 0 || full.counts.relay > 0 || relayOnlyWorks;
  for (const error of errors) {
    error.informational = error.errorCode === 701 && connected;
  }

  return {
    counts: full.counts,
    hasHost: full.counts.host > 0,
    hasStun: full.counts.srflx > 0,
    hasRelay: full.counts.relay > 0,
    relayOnlyWorks,
    publicAddress: full.publicAddress,
    relayLatencyMs: full.relayLatencyMs,
    gatheringMs: full.elapsedMs,
    errors,
    verdict: verdictFor(full.counts, relayOnlyWorks),
  };
}

/** Fetch the ICE servers without creating a session. Guest-or-owner on the host. */
export async function fetchIceServers(): Promise<RTCIceServer[]> {
  const response = await fetch('/api/webrtc/ice-config', {
    credentials: 'include',
    headers: { Accept: 'application/json' },
  });
  if (!response.ok) {
    throw new Error(`ICE config unavailable (HTTP ${response.status})`);
  }
  const payload = (await response.json()) as { ice_servers?: RTCIceServer[] };
  return payload.ice_servers ?? [];
}
