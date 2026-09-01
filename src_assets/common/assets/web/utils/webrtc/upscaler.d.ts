/**
 * Type surface for the shared FSR upscaler, which lives in the legacy tree and is
 * aliased in by vite.config.ts. The alias resolves at build time; TypeScript needs
 * this stub to resolve the same import, because "@/*" maps into web/ only.
 */

export interface UpscalerOptions {
  /** RCAS sharpness in stops: 0 = maximum sharpening, higher = softer. */
  sharpnessStops?: number;
  /** Cap on devicePixelRatio, to bound the EASU output size on 3x phone screens. */
  maxDevicePixelRatio?: number;
  /** Called once if the pipeline dies at runtime (context lost). */
  onFailure?: () => void;
}

export interface VideoUpscaler {
  detach(): void;
}

export declare function attachVideoUpscaler(
  video: HTMLVideoElement,
  canvas: HTMLCanvasElement,
  options?: UpscalerOptions,
): VideoUpscaler | null;
