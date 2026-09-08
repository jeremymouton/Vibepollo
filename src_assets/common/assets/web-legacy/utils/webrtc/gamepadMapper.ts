/**
 * Turning a browser Gamepad into the host's button mask and stick values.
 *
 * When `Gamepad.mapping` is `'standard'` the indices are fixed by the W3C and
 * the layout below is exact. When it is empty the indices mean whatever the
 * driver decided, and a guest arrives with whatever pad they own — so rather
 * than guess a second hardcoded table, the axis roles are calibrated from the
 * pad's resting position at connect: a trigger axis rests at -1, a stick or hat
 * axis rests at 0. A pad that calibrates to nothing unusual falls back to the
 * standard layout, so the fallback path can only add inputs, never move one.
 */

export type GamepadVector = [number, number, number];

export interface GamepadSnapshot {
  buttons: number;
  lt: number;
  rt: number;
  lsX: number;
  lsY: number;
  rsX: number;
  rsY: number;
}

/** How much the index layout below can be trusted, for diagnostics. */
export type GamepadMappingSource = 'standard' | 'known' | 'fallback';

export interface GamepadAxisLayout {
  rightStickX: number;
  rightStickY: number;
  leftTrigger?: number;
  rightTrigger?: number;
  hatX?: number;
  hatY?: number;
}

export interface GamepadProfile {
  buttonMap: Map<number, number>;
  supportedButtons: number;
  capabilities: number;
  type: number;
  source: GamepadMappingSource;
  axisLayout: GamepadAxisLayout;
  name: string;
}

export const GAMEPAD_TYPE = {
  unknown: 0,
  xbox: 1,
  playstation: 2,
  nintendo: 3,
} as const;

export const GAMEPAD_CAPS = {
  analogTriggers: 0x01,
  touchpad: 0x08,
  accel: 0x10,
  gyro: 0x20,
} as const;

export const GAMEPAD_BUTTONS = {
  dpadUp: 0x0001,
  dpadDown: 0x0002,
  dpadLeft: 0x0004,
  dpadRight: 0x0008,
  start: 0x0010,
  back: 0x0020,
  leftStick: 0x0040,
  rightStick: 0x0080,
  leftButton: 0x0100,
  rightButton: 0x0200,
  home: 0x0400,
  a: 0x1000,
  b: 0x2000,
  x: 0x4000,
  y: 0x8000,
  paddle1: 0x010000,
  paddle2: 0x020000,
  paddle3: 0x040000,
  paddle4: 0x080000,
  touchpadButton: 0x100000,
  miscButton: 0x200000,
} as const;

export const AXIS_DEADZONE = 0.08;

const STANDARD_AXIS_LAYOUT: GamepadAxisLayout = { rightStickX: 2, rightStickY: 3 };

const STANDARD_BUTTON_MAP = new Map<number, number>([
  [0, GAMEPAD_BUTTONS.a],
  [1, GAMEPAD_BUTTONS.b],
  [2, GAMEPAD_BUTTONS.x],
  [3, GAMEPAD_BUTTONS.y],
  [4, GAMEPAD_BUTTONS.leftButton],
  [5, GAMEPAD_BUTTONS.rightButton],
  [8, GAMEPAD_BUTTONS.back],
  [9, GAMEPAD_BUTTONS.start],
  [10, GAMEPAD_BUTTONS.leftStick],
  [11, GAMEPAD_BUTTONS.rightStick],
  [12, GAMEPAD_BUTTONS.dpadUp],
  [13, GAMEPAD_BUTTONS.dpadDown],
  [14, GAMEPAD_BUTTONS.dpadLeft],
  [15, GAMEPAD_BUTTONS.dpadRight],
  [16, GAMEPAD_BUTTONS.home],
  [17, GAMEPAD_BUTTONS.miscButton],
]);

/// Vendors whose pads follow the standard button order even when the browser
/// declines to say so, so a missing `mapping` is not worth reporting as unknown.
const KNOWN_GENERIC_ID_PATTERNS = [
  '8bitdo',
  'gamesir',
  'gulikit',
  'hori',
  'logitech',
  'mayflash',
  'powera',
  'stadia',
  'steelseries',
];

const TRIGGER_REST_MAX = -0.9;
const STICK_REST_MAX = 0.2;
/// A hat reports only -1, 0 and +1; anything between is a stick that happened
/// to be resting at zero when the pad was calibrated.
const HAT_STEP_TOLERANCE = 0.15;
const HAT_THRESHOLD = 0.5;

function typeLabel(type: number): string {
  if (type === GAMEPAD_TYPE.xbox) return 'Xbox';
  if (type === GAMEPAD_TYPE.playstation) return 'PlayStation';
  if (type === GAMEPAD_TYPE.nintendo) return 'Nintendo';
  return 'Generic';
}

export function resolveGamepadType(gamepad: Gamepad): number {
  const id = (gamepad.id || '').toLowerCase();
  if (id.includes('nintendo') || id.includes('switch') || id.includes('joy-con')) {
    return GAMEPAD_TYPE.nintendo;
  }
  if (
    id.includes('playstation') ||
    id.includes('dualshock') ||
    id.includes('dualsense') ||
    id.includes('ps4') ||
    id.includes('ps5')
  ) {
    return GAMEPAD_TYPE.playstation;
  }
  if (id.includes('xbox') || id.includes('xinput')) {
    return GAMEPAD_TYPE.xbox;
  }
  if (id.includes('wireless controller')) {
    return GAMEPAD_TYPE.playstation;
  }
  return GAMEPAD_TYPE.unknown;
}

function resolveMappingSource(gamepad: Gamepad, type: number): GamepadMappingSource {
  if (gamepad.mapping === 'standard') return 'standard';
  if (type !== GAMEPAD_TYPE.unknown) return 'known';
  const id = (gamepad.id || '').toLowerCase();
  return KNOWN_GENERIC_ID_PATTERNS.some((pattern) => id.includes(pattern)) ? 'known' : 'fallback';
}

function resolveButtonMap(gamepad: Gamepad, type: number): Map<number, number> {
  const map = new Map(STANDARD_BUTTON_MAP);
  if (type === GAMEPAD_TYPE.playstation) {
    map.set(17, GAMEPAD_BUTTONS.touchpadButton);
  }
  const paddles = [
    GAMEPAD_BUTTONS.paddle1,
    GAMEPAD_BUTTONS.paddle2,
    GAMEPAD_BUTTONS.paddle3,
    GAMEPAD_BUTTONS.paddle4,
  ];
  paddles.forEach((bit, offset) => {
    const index = 18 + offset;
    if (gamepad.buttons.length > index) {
      map.set(index, bit);
    }
  });
  return map;
}

/**
 * Read the axis roles off a pad sitting at rest.
 *
 * Axes 0 and 1 are the left stick in every layout worth supporting. Past those,
 * an axis resting hard negative is a trigger and one resting at zero is half of
 * a stick or a hat; the first such pair is the right stick and a second pair,
 * if the pad has one, is the D-pad hat. A guest holding a trigger while the pad
 * connects hides it from the calibration, which costs them both analog triggers
 * and nothing else.
 */
export function inferAxisLayout(axes: readonly number[]): GamepadAxisLayout {
  const triggers: number[] = [];
  const centred: number[] = [];
  for (let index = 2; index < axes.length; index += 1) {
    const value = axes[index];
    if (typeof value !== 'number' || !Number.isFinite(value)) continue;
    if (value <= TRIGGER_REST_MAX) triggers.push(index);
    else if (Math.abs(value) <= STICK_REST_MAX) centred.push(index);
  }

  const pairs: Array<[number, number]> = [];
  for (let i = 0; i + 1 < centred.length; i += 2) {
    const x = centred[i];
    const y = centred[i + 1];
    if (x === undefined || y === undefined) break;
    pairs.push([x, y]);
  }

  const rightStick = pairs[0];
  const layout: GamepadAxisLayout = {
    rightStickX: rightStick?.[0] ?? STANDARD_AXIS_LAYOUT.rightStickX,
    rightStickY: rightStick?.[1] ?? STANDARD_AXIS_LAYOUT.rightStickY,
  };
  // Which side a lone trigger axis belongs to is a coin flip, and calling it
  // wrong puts one trigger on both. Both or neither.
  const leftTrigger = triggers[0];
  const rightTrigger = triggers[1];
  if (leftTrigger !== undefined && rightTrigger !== undefined) {
    layout.leftTrigger = leftTrigger;
    layout.rightTrigger = rightTrigger;
  }
  const hat = pairs.length > 1 ? pairs[pairs.length - 1] : undefined;
  if (hat) {
    layout.hatX = hat[0];
    layout.hatY = hat[1];
  }
  return layout;
}

function clamp(value: number, min: number, max: number): number {
  if (!Number.isFinite(value)) return 0;
  return Math.min(max, Math.max(min, value));
}

export function applyDeadzone(value: number, deadzone: number): number {
  const safe = clamp(value, -1, 1);
  const abs = Math.abs(safe);
  if (abs <= deadzone) return 0;
  const scaled = (abs - deadzone) / (1 - deadzone);
  return Math.min(1, Math.max(0, scaled)) * Math.sign(safe);
}

function toInt16(value: number): number {
  return Math.round(clamp(value, -1, 1) * 32767);
}

function toUint8(value: number): number {
  return Math.round(clamp(value, 0, 1) * 255);
}

/// A trigger axis sweeps -1 (released) to +1 (pulled), not 0 to 1.
function triggerAxisValue(value?: number): number {
  if (typeof value !== 'number' || !Number.isFinite(value)) return 0;
  return (clamp(value, -1, 1) + 1) / 2;
}

function readButtons(gamepad: Gamepad, buttonMap: Map<number, number>): number {
  let mask = 0;
  buttonMap.forEach((bit, index) => {
    const button = gamepad.buttons[index];
    if (button?.pressed) {
      mask |= bit;
    }
  });
  return mask;
}

function readTrigger(gamepad: Gamepad, buttonIndex: number, axisIndex?: number): number {
  const fromButton = gamepad.buttons[buttonIndex]?.value ?? 0;
  const fromAxis = axisIndex === undefined ? 0 : triggerAxisValue(gamepad.axes?.[axisIndex]);
  return toUint8(Math.max(fromButton, fromAxis));
}

function isHatValue(value: number): boolean {
  return [-1, 0, 1].some((step) => Math.abs(value - step) <= HAT_STEP_TOLERANCE);
}

function readHat(axes: readonly number[], layout: GamepadAxisLayout): number {
  if (layout.hatX === undefined || layout.hatY === undefined) return 0;
  const rawX = axes[layout.hatX];
  const rawY = axes[layout.hatY];
  if (typeof rawX !== 'number' || typeof rawY !== 'number') return 0;
  if (!isHatValue(rawX) || !isHatValue(rawY)) return 0;
  let mask = 0;
  if (rawX <= -HAT_THRESHOLD) mask |= GAMEPAD_BUTTONS.dpadLeft;
  if (rawX >= HAT_THRESHOLD) mask |= GAMEPAD_BUTTONS.dpadRight;
  if (rawY <= -HAT_THRESHOLD) mask |= GAMEPAD_BUTTONS.dpadUp;
  if (rawY >= HAT_THRESHOLD) mask |= GAMEPAD_BUTTONS.dpadDown;
  return mask;
}

export function createGamepadProfile(gamepad: Gamepad): GamepadProfile {
  const type = resolveGamepadType(gamepad);
  const source = resolveMappingSource(gamepad, type);
  const buttonMap = resolveButtonMap(gamepad, type);
  const axisLayout =
    source === 'standard' ? { ...STANDARD_AXIS_LAYOUT } : inferAxisLayout(gamepad.axes || []);

  let supportedButtons = 0;
  buttonMap.forEach((bit) => {
    supportedButtons |= bit;
  });
  const motion = readGamepadMotion(gamepad);
  const hasAnalogTriggers =
    gamepad.buttons.length > 6 ||
    axisLayout.leftTrigger !== undefined ||
    axisLayout.rightTrigger !== undefined;
  let capabilities = 0;
  if (hasAnalogTriggers) capabilities |= GAMEPAD_CAPS.analogTriggers;
  if (motion.accel || type === GAMEPAD_TYPE.playstation) capabilities |= GAMEPAD_CAPS.accel;
  if (motion.gyro || type === GAMEPAD_TYPE.playstation) capabilities |= GAMEPAD_CAPS.gyro;

  return {
    buttonMap,
    supportedButtons,
    capabilities,
    type,
    source,
    axisLayout,
    name: gamepad.id || `Gamepad ${gamepad.index + 1}`,
  };
}

export function readGamepadState(gamepad: Gamepad, profile: GamepadProfile): GamepadSnapshot {
  const axes = gamepad.axes || [];
  const layout = profile.axisLayout;
  const lx = applyDeadzone(axes[0] ?? 0, AXIS_DEADZONE);
  const ly = applyDeadzone(-(axes[1] ?? 0), AXIS_DEADZONE);
  const rx = applyDeadzone(axes[layout.rightStickX] ?? 0, AXIS_DEADZONE);
  const ry = applyDeadzone(-(axes[layout.rightStickY] ?? 0), AXIS_DEADZONE);
  return {
    buttons: readButtons(gamepad, profile.buttonMap) | readHat(axes, layout),
    lt: readTrigger(gamepad, 6, layout.leftTrigger),
    rt: readTrigger(gamepad, 7, layout.rightTrigger),
    lsX: toInt16(lx),
    lsY: toInt16(ly),
    rsX: toInt16(rx),
    rsY: toInt16(ry),
  };
}

function readMotionVector(value: unknown): GamepadVector | undefined {
  if (!value || typeof value !== 'object') return undefined;
  const array = Array.isArray(value)
    ? value
    : (value as { length?: number; [index: number]: number });
  if (typeof array.length !== 'number' || array.length < 3) return undefined;
  const x = Number(array[0]);
  const y = Number(array[1]);
  const z = Number(array[2]);
  if (!Number.isFinite(x) || !Number.isFinite(y) || !Number.isFinite(z)) return undefined;
  return [x, y, z];
}

export function readGamepadMotion(gamepad: Gamepad): {
  gyro?: GamepadVector;
  accel?: GamepadVector;
} {
  const pose = (
    gamepad as { pose?: { angularVelocity?: unknown; linearAcceleration?: unknown } | null }
  ).pose;
  const motion = (
    gamepad as { motion?: { angularVelocity?: unknown; linearAcceleration?: unknown } }
  ).motion;
  const motionData = (
    gamepad as { motionData?: { angularVelocity?: unknown; linearAcceleration?: unknown } }
  ).motionData;
  const source = motion ?? motionData ?? pose ?? null;
  if (!source) return {};
  const gyro = readMotionVector(source.angularVelocity);
  const accel = readMotionVector(source.linearAcceleration);
  const result: { gyro?: GamepadVector; accel?: GamepadVector } = {};
  if (gyro) result.gyro = gyro;
  if (accel) result.accel = accel;
  return result;
}

/** One line naming the pad and how much of its layout was guessed. */
export function describeGamepad(profile: GamepadProfile): string {
  const layout = profile.axisLayout;
  const parts = [`${typeLabel(profile.type)} mapping=${profile.source}`];
  if (profile.source !== 'standard') {
    const hat = layout.hatX === undefined ? 'btn' : `${layout.hatX},${layout.hatY ?? '?'}`;
    parts.push(
      `rs=${layout.rightStickX},${layout.rightStickY}`,
      `triggers=${layout.leftTrigger ?? 'btn'},${layout.rightTrigger ?? 'btn'}`,
      `hat=${hat}`,
    );
  }
  return `${profile.name} [${parts.join(' ')}]`;
}
