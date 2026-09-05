import type { GamepadFeedbackMessage, InputMessage } from '../../types/webrtc';

export interface InputCaptureMetrics {
  lastMoveDelayMs?: number;
  avgMoveDelayMs?: number;
  maxMoveDelayMs?: number;
  lastMoveEventLagMs?: number;
  avgMoveEventLagMs?: number;
  maxMoveEventLagMs?: number;
  moveRateHz?: number;
  moveSendRateHz?: number;
  moveCoalesceRatio?: number;
}

interface InputCaptureOptions {
  video?: HTMLVideoElement | null;
  onMetrics?: (metrics: InputCaptureMetrics) => void;
  gamepad?: boolean;
  /** Capture pointer, wheel and keyboard as well as gamepads. Defaults to true. */
  pointerAndKeyboard?: boolean;
  shouldDrop?: (payload: InputMessage) => boolean;
}

/** Encode absolute motion as the host's seven-byte, sequence-stamped move packet. */
export declare function encodeMouseMove(x: number, y: number): ArrayBuffer;
export declare function requestKeyboardLock(keys?: string[]): Promise<boolean>;
export declare function releaseKeyboardLock(): void;
export declare function applyGamepadFeedback(message: GamepadFeedbackMessage | unknown): void;
export declare function attachInputCapture(
  element: HTMLElement,
  send: (payload: string | ArrayBuffer) => boolean | void,
  options?: InputCaptureOptions,
): () => void;

/** Publish a Gamepad-shaped object so the capture loop reads it as hardware. */
export declare function setVirtualGamepad(pad?: Gamepad): void;
