<script setup lang="ts">
/**
 * On-screen controller for a guest playing on a phone or tablet.
 *
 * It does not send anything itself. It maintains a Gamepad-shaped object and hands
 * it to setVirtualGamepad, so the existing capture loop picks it up exactly as it
 * picks up hardware: same button bitmask, same deadzones, same trigger scaling,
 * same rate limiting. The host cannot tell the two apart, and this inherits the
 * invite's gamepad grant with no separate permission path to get wrong.
 *
 * Indexes follow the W3C standard mapping because that is what the capture loop
 * translates from: 0-3 face, 4/5 bumpers, 6/7 triggers, 8/9 back/start, 12-15
 * d-pad, axes 0/1 left stick and 2/3 right.
 */
import { onBeforeUnmount, onMounted, reactive, ref } from 'vue';

import { setVirtualGamepad } from '@/utils/webrtc/input';

const BUTTON_COUNT = 17;
const STICK_RADIUS = 56;

/**
 * Rumble, on a device that has no rumble motor.
 *
 * applyGamepadFeedback looks for a vibrationActuator on the pad and gives up if
 * there is none — which is why the on-screen controller felt nothing while a real
 * one shook. So it supplies one, and the shared code needs no knowledge of it.
 *
 * navigator.vibrate is a switch, not a dial: there is no intensity, only on and
 * off. Strength is approximated with a duty cycle — a solid buzz when the game
 * asks for a lot, broken pulses when it asks for little — which reads as weaker
 * without pretending to a precision the hardware does not have.
 *
 * iOS refuses the API outright. There it is simply a no-op, which is the correct
 * outcome and not worth apologising for on screen.
 */
let vibrationBusyUntil = 0;

const vibrationActuator = {
  type: 'dual-rumble',
  playEffect: (
    _type: string,
    params: { duration?: number; strongMagnitude?: number; weakMagnitude?: number },
  ) => {
    const vibrate = navigator.vibrate?.bind(navigator);
    if (!vibrate) return Promise.resolve('complete');

    const strength = Math.max(params.strongMagnitude ?? 0, params.weakMagnitude ?? 0);
    const now = performance.now();

    if (strength <= 0.05) {
      vibrate(0);
      vibrationBusyUntil = 0;
      return Promise.resolve('complete');
    }

    // A game may ask many times a second. Restarting the motor on each one makes it
    // stutter and drains the battery, so an effect already running is left alone.
    if (now < vibrationBusyUntil) return Promise.resolve('complete');

    const duration = Math.min(400, Math.max(20, Math.round(params.duration ?? 100)));
    if (strength > 0.6) {
      vibrate(duration);
    } else {
      // Roughly `strength` of each 40ms slice spent buzzing.
      const slice = 40;
      const on = Math.max(8, Math.round(slice * strength));
      const pattern: number[] = [];
      for (let elapsed = 0; elapsed < duration; elapsed += slice) {
        pattern.push(on, Math.max(0, slice - on));
      }
      vibrate(pattern);
    }
    vibrationBusyUntil = now + duration;
    return Promise.resolve('complete');
  },
};

const pressed = reactive<Record<number, number>>({});
const axes = reactive<number[]>([0, 0, 0, 0]);
const root = ref<HTMLDivElement>();

/// Rebuilt each frame rather than mutated in place: the capture loop keeps the
/// previous snapshot to diff against, and handing it the same mutating object
/// would make every frame look unchanged.
function snapshot(): Gamepad {
  const buttons = [];
  for (let i = 0; i < BUTTON_COUNT; i++) {
    const value = pressed[i] ?? 0;
    buttons.push({ pressed: value > 0.5, touched: value > 0, value });
  }
  return {
    id: 'Vibepollo on-screen controller (STANDARD GAMEPAD)',
    index: 0,
    connected: true,
    mapping: 'standard',
    timestamp: performance.now(),
    axes: axes.slice(),
    buttons,
    vibrationActuator,
  } as unknown as Gamepad;
}

let raf = 0;
function pump(): void {
  setVirtualGamepad(snapshot());
  raf = requestAnimationFrame(pump);
}

function press(index: number, value = 1): void {
  pressed[index] = value;
}
function release(index: number): void {
  pressed[index] = 0;
}

/// Sticks track the finger from wherever it first landed, so a thumb that starts
/// off-centre does not jump the stick to full deflection.
const stickOrigin = reactive<Record<number, { x: number; y: number }>>({});

function stickDown(event: PointerEvent, stick: number): void {
  (event.target as HTMLElement).setPointerCapture(event.pointerId);
  stickOrigin[stick] = { x: event.clientX, y: event.clientY };
  event.preventDefault();
}

function stickMove(event: PointerEvent, stick: number): void {
  const origin = stickOrigin[stick];
  if (!origin) return;
  const dx = (event.clientX - origin.x) / STICK_RADIUS;
  const dy = (event.clientY - origin.y) / STICK_RADIUS;
  const length = Math.hypot(dx, dy);
  const scale = length > 1 ? 1 / length : 1;
  axes[stick * 2] = dx * scale;
  axes[stick * 2 + 1] = dy * scale;
  event.preventDefault();
}

function stickUp(stick: number): void {
  delete stickOrigin[stick];
  axes[stick * 2] = 0;
  axes[stick * 2 + 1] = 0;
}

function stickStyle(stick: number): Record<string, string> {
  return {
    transform: `translate(${(axes[stick * 2] ?? 0) * 26}px, ${(axes[stick * 2 + 1] ?? 0) * 26}px)`,
  };
}

onMounted(() => {
  raf = requestAnimationFrame(pump);
});
onBeforeUnmount(() => {
  cancelAnimationFrame(raf);
  // Do not leave the phone buzzing after the controls are gone.
  navigator.vibrate?.(0);
  // Withdraw it, or the capture loop keeps reporting a pad nobody is holding.
  setVirtualGamepad();
});
</script>

<template>
  <div ref="root" class="touchpad" aria-label="On-screen controller">
    <div class="touchpad__side touchpad__side--left">
      <div class="shoulders">
        <button
          class="tbtn tbtn--wide"
          @pointerdown.prevent="press(6)"
          @pointerup="release(6)"
          @pointercancel="release(6)"
        >
          LT
        </button>
        <button
          class="tbtn tbtn--wide"
          @pointerdown.prevent="press(4)"
          @pointerup="release(4)"
          @pointercancel="release(4)"
        >
          LB
        </button>
      </div>
      <div
        class="stick"
        @pointerdown="stickDown($event, 0)"
        @pointermove="stickMove($event, 0)"
        @pointerup="stickUp(0)"
        @pointercancel="stickUp(0)"
      >
        <div class="stick__cap" :style="stickStyle(0)"></div>
      </div>
      <div class="dpad">
        <button
          class="tbtn dpad__up"
          @pointerdown.prevent="press(12)"
          @pointerup="release(12)"
          @pointercancel="release(12)"
        >
          ▲
        </button>
        <button
          class="tbtn dpad__left"
          @pointerdown.prevent="press(14)"
          @pointerup="release(14)"
          @pointercancel="release(14)"
        >
          ◀
        </button>
        <button
          class="tbtn dpad__right"
          @pointerdown.prevent="press(15)"
          @pointerup="release(15)"
          @pointercancel="release(15)"
        >
          ▶
        </button>
        <button
          class="tbtn dpad__down"
          @pointerdown.prevent="press(13)"
          @pointerup="release(13)"
          @pointercancel="release(13)"
        >
          ▼
        </button>
      </div>
    </div>

    <div class="touchpad__centre">
      <button
        class="tbtn tbtn--small"
        @pointerdown.prevent="press(8)"
        @pointerup="release(8)"
        @pointercancel="release(8)"
      >
        Back
      </button>
      <button
        class="tbtn tbtn--small"
        @pointerdown.prevent="press(9)"
        @pointerup="release(9)"
        @pointercancel="release(9)"
      >
        Start
      </button>
    </div>

    <div class="touchpad__side touchpad__side--right">
      <div class="shoulders shoulders--right">
        <button
          class="tbtn tbtn--wide"
          @pointerdown.prevent="press(5)"
          @pointerup="release(5)"
          @pointercancel="release(5)"
        >
          RB
        </button>
        <button
          class="tbtn tbtn--wide"
          @pointerdown.prevent="press(7)"
          @pointerup="release(7)"
          @pointercancel="release(7)"
        >
          RT
        </button>
      </div>
      <div class="face">
        <button
          class="tbtn face__y"
          @pointerdown.prevent="press(3)"
          @pointerup="release(3)"
          @pointercancel="release(3)"
        >
          Y
        </button>
        <button
          class="tbtn face__x"
          @pointerdown.prevent="press(2)"
          @pointerup="release(2)"
          @pointercancel="release(2)"
        >
          X
        </button>
        <button
          class="tbtn face__b"
          @pointerdown.prevent="press(1)"
          @pointerup="release(1)"
          @pointercancel="release(1)"
        >
          B
        </button>
        <button
          class="tbtn face__a"
          @pointerdown.prevent="press(0)"
          @pointerup="release(0)"
          @pointercancel="release(0)"
        >
          A
        </button>
      </div>
      <div
        class="stick"
        @pointerdown="stickDown($event, 1)"
        @pointermove="stickMove($event, 1)"
        @pointerup="stickUp(1)"
        @pointercancel="stickUp(1)"
      >
        <div class="stick__cap" :style="stickStyle(1)"></div>
      </div>
    </div>
  </div>
</template>

<style scoped>
.touchpad {
  position: absolute;
  inset: 0;
  display: flex;
  align-items: flex-end;
  justify-content: space-between;
  padding: 12px;
  /* The video underneath must stay visible; only the controls take touches. */
  pointer-events: none;
  touch-action: none;
  user-select: none;
}
.touchpad__side,
.touchpad__centre {
  display: flex;
  gap: 10px;
  align-items: flex-end;
  pointer-events: auto;
}
.touchpad__centre {
  align-self: flex-start;
  gap: 6px;
}
.shoulders {
  display: flex;
  flex-direction: column;
  gap: 6px;
}
.tbtn {
  display: grid;
  place-items: center;
  width: 52px;
  height: 52px;
  border: 1px solid rgb(255 255 255 / 35%);
  border-radius: 50%;
  color: #fff;
  font-size: 13px;
  font-weight: 600;
  background: rgb(0 0 0 / 35%);
  backdrop-filter: blur(2px);
  touch-action: none;
}
.tbtn:active {
  background: rgb(79 140 255 / 70%);
}
.tbtn--wide {
  width: 62px;
  height: 34px;
  border-radius: 17px;
}
.tbtn--small {
  width: 54px;
  height: 26px;
  border-radius: 13px;
  font-size: 11px;
}
.stick {
  display: grid;
  place-items: center;
  width: 112px;
  height: 112px;
  border: 1px solid rgb(255 255 255 / 25%);
  border-radius: 50%;
  background: rgb(0 0 0 / 30%);
  touch-action: none;
}
.stick__cap {
  width: 52px;
  height: 52px;
  border-radius: 50%;
  background: rgb(255 255 255 / 45%);
}
.dpad,
.face {
  position: relative;
  width: 112px;
  height: 112px;
}
.dpad .tbtn,
.face .tbtn {
  position: absolute;
  width: 44px;
  height: 44px;
}
.dpad__up,
.face__y {
  top: 0;
  left: 34px;
}
.dpad__down,
.face__a {
  bottom: 0;
  left: 34px;
}
.dpad__left,
.face__x {
  top: 34px;
  left: 0;
}
.dpad__right,
.face__b {
  top: 34px;
  right: 0;
}
</style>
