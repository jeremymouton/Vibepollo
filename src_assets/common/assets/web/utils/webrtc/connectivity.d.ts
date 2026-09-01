/**
 * Type surface for the shared connectivity probe, which lives in the legacy tree
 * and is aliased in by vite.config.ts. The alias resolves at build time;
 * TypeScript needs this stub to resolve the same import, because "@/*" maps into
 * web/ only.
 */

export type CandidateKind = 'host' | 'srflx' | 'prflx' | 'relay';

export interface IceServerError {
  url: string;
  errorCode: number;
  errorText: string;
  informational: boolean;
}

export interface ConnectivityReport {
  counts: Record<CandidateKind, number>;
  hasHost: boolean;
  hasStun: boolean;
  hasRelay: boolean;
  relayOnlyWorks: boolean;
  publicAddress: string | null;
  relayLatencyMs: number | null;
  gatheringMs: number;
  errors: IceServerError[];
  verdict: 'good' | 'relay-only' | 'degraded' | 'blocked';
}

export declare function probeConnectivity(iceServers: RTCIceServer[]): Promise<ConnectivityReport>;
export declare function fetchIceServers(): Promise<RTCIceServer[]>;
