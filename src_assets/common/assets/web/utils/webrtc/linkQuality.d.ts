/**
 * Type surface for the shared link-quality assessment, which lives in the legacy
 * tree and is aliased in by vite.config.ts. The alias resolves at build time;
 * TypeScript needs this stub because "@/*" maps into web/ only.
 */

export type LinkQualityTier = 'excellent' | 'good' | 'fair' | 'poor' | 'unknown';
export type LinkQualityCause = 'latency' | 'jitter' | 'loss' | null;

export interface LinkQualityInput {
  roundTripMs?: number | undefined;
  jitterMs?: number | undefined;
  lossPercent?: number | undefined;
  relayed?: boolean | undefined;
}

export interface LinkQualityVerdict {
  tier: LinkQualityTier;
  bars: number;
  cause: LinkQualityCause;
  headline: string;
  detail: string;
}

export declare function assessLinkQuality(input: LinkQualityInput): LinkQualityVerdict;
