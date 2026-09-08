import { describe, expect, it } from 'vitest';

import {
  createGamepadProfile,
  GAMEPAD_BUTTONS,
  inferAxisLayout,
  readGamepadState,
} from '@web/utils/webrtc/gamepadMapper';

interface FakePadOptions {
  id?: string;
  mapping?: string;
  axes?: number[];
  buttonCount?: number;
  pressed?: number[];
  values?: Record<number, number>;
}

function fakePad({
  id = 'Test Pad',
  mapping = 'standard',
  axes = [0, 0, 0, 0],
  buttonCount = 18,
  pressed = [],
  values = {},
}: FakePadOptions = {}): Gamepad {
  const buttons = Array.from({ length: buttonCount }, (_unused, index) => ({
    pressed: pressed.includes(index),
    touched: pressed.includes(index),
    value: values[index] ?? (pressed.includes(index) ? 1 : 0),
  }));
  return { id, index: 0, mapping, axes, buttons, connected: true } as unknown as Gamepad;
}

/** Axis layout Chrome reports for a DualShock 4 on Linux without a standard mapping. */
const DS4_LINUX_AT_REST = [0, 0, -1, 0, 0, -1, 0, 0];

describe('mapping source', () => {
  it('trusts a browser that declares the standard mapping', () => {
    expect(createGamepadProfile(fakePad({ mapping: 'standard' })).source).toBe('standard');
  });

  it('recognises a known vendor that the browser did not map', () => {
    expect(createGamepadProfile(fakePad({ mapping: '', id: '8BitDo Pro 2' })).source).toBe('known');
    expect(
      createGamepadProfile(fakePad({ mapping: '', id: 'Xbox 360 Controller (STANDARD GAMEPAD)' }))
        .source,
    ).toBe('known');
  });

  it('falls back for a pad nobody recognises', () => {
    expect(createGamepadProfile(fakePad({ mapping: '', id: 'Generic USB Joystick' })).source).toBe(
      'fallback',
    );
  });
});

describe('axis calibration', () => {
  it('reads triggers, right stick and hat off a pad at rest', () => {
    expect(inferAxisLayout(DS4_LINUX_AT_REST)).toEqual({
      rightStickX: 3,
      rightStickY: 4,
      leftTrigger: 2,
      rightTrigger: 5,
      hatX: 6,
      hatY: 7,
    });
  });

  it('claims no hat when the pad only has two sticks and two triggers', () => {
    expect(inferAxisLayout([0, 0, 0, 0, -1, -1])).toEqual({
      rightStickX: 2,
      rightStickY: 3,
      leftTrigger: 4,
      rightTrigger: 5,
    });
  });

  it('keeps the standard layout when calibration learns nothing', () => {
    expect(inferAxisLayout([0, 0, 0, 0])).toEqual({ rightStickX: 2, rightStickY: 3 });
  });

  it('gives up on both triggers when one was held while the pad connected', () => {
    const layout = inferAxisLayout([0, 0, 1, 0, 0, -1, 0, 0]);
    expect(layout.leftTrigger).toBeUndefined();
    expect(layout.rightTrigger).toBeUndefined();
    expect(layout).toMatchObject({ rightStickX: 3, rightStickY: 4, hatX: 6, hatY: 7 });
  });

  it('never calibrates a standard-mapping pad, however many axes it reports', () => {
    const profile = createGamepadProfile(fakePad({ mapping: 'standard', axes: DS4_LINUX_AT_REST }));
    expect(profile.axisLayout).toEqual({ rightStickX: 2, rightStickY: 3 });
  });
});

describe('standard mapping', () => {
  const profile = createGamepadProfile(fakePad());

  it('reads sticks from axes 0 to 3', () => {
    const snapshot = readGamepadState(fakePad({ axes: [1, -1, -1, 1] }), profile);
    expect(snapshot).toMatchObject({ lsX: 32767, lsY: 32767, rsX: -32767, rsY: -32767 });
  });

  it('reads triggers from buttons 6 and 7', () => {
    const pad = fakePad({ values: { 6: 1, 7: 0.5 } });
    const snapshot = readGamepadState(pad, profile);
    expect(snapshot.lt).toBe(255);
    expect(snapshot.rt).toBe(128);
  });

  it('maps face and dpad buttons to the host mask', () => {
    const snapshot = readGamepadState(fakePad({ pressed: [0, 9, 14] }), profile);
    expect(snapshot.buttons).toBe(
      GAMEPAD_BUTTONS.a | GAMEPAD_BUTTONS.start | GAMEPAD_BUTTONS.dpadLeft,
    );
  });

  it('only offers paddles the pad actually has', () => {
    expect(createGamepadProfile(fakePad({ buttonCount: 18 })).buttonMap.has(18)).toBe(false);
    expect(createGamepadProfile(fakePad({ buttonCount: 19 })).buttonMap.has(18)).toBe(true);
  });
});

describe('calibrated fallback mapping', () => {
  const profile = createGamepadProfile(
    fakePad({ mapping: '', id: 'Generic USB Joystick', axes: DS4_LINUX_AT_REST, buttonCount: 12 }),
  );

  it('reads the right stick from the calibrated pair, not axes 2 and 3', () => {
    const axes = [...DS4_LINUX_AT_REST];
    axes[3] = 1;
    axes[4] = -1;
    const snapshot = readGamepadState(fakePad({ mapping: '', axes, buttonCount: 12 }), profile);
    expect(snapshot).toMatchObject({ rsX: 32767, rsY: 32767 });
  });

  it('reads a trigger that arrives as an axis sweeping -1 to 1', () => {
    const axes = [...DS4_LINUX_AT_REST];
    axes[2] = 1;
    axes[5] = 0;
    const snapshot = readGamepadState(fakePad({ mapping: '', axes, buttonCount: 12 }), profile);
    expect(snapshot.lt).toBe(255);
    expect(snapshot.rt).toBe(128);
  });

  it('does not read a pulled trigger as a dpad press', () => {
    const axes = [...DS4_LINUX_AT_REST];
    axes[2] = 1;
    const snapshot = readGamepadState(fakePad({ mapping: '', axes, buttonCount: 12 }), profile);
    expect(snapshot.buttons).toBe(0);
  });

  it('reads the dpad from the calibrated hat', () => {
    const axes = [...DS4_LINUX_AT_REST];
    axes[6] = -1;
    axes[7] = 1;
    const snapshot = readGamepadState(fakePad({ mapping: '', axes, buttonCount: 12 }), profile);
    expect(snapshot.buttons).toBe(GAMEPAD_BUTTONS.dpadLeft | GAMEPAD_BUTTONS.dpadDown);
  });

  it('ignores a hat axis holding a value no hat would report', () => {
    const axes = [...DS4_LINUX_AT_REST];
    axes[6] = 0.35;
    const snapshot = readGamepadState(fakePad({ mapping: '', axes, buttonCount: 12 }), profile);
    expect(snapshot.buttons).toBe(0);
  });

  it('reports analog triggers even though the pad has no trigger buttons', () => {
    expect(profile.capabilities & 0x01).toBe(0x01);
  });
});
