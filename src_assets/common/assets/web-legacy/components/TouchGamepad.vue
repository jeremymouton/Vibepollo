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
 *
 * Every pointer is routed through this component rather than through per-button
 * handlers. That indirection is the whole point: a finger is tracked from wherever
 * it lands until it lifts, so it can slide from one button onto another, hold two
 * at once, and never strand a button pressed because the lift happened somewhere
 * else. Per-button pointerdown/pointerup could do none of those things.
 */
import { computed, onBeforeUnmount, onMounted, reactive, ref } from 'vue';

import { setVirtualGamepad } from '@/utils/webrtc/input';

const BUTTON_COUNT = 17;
const STICK_RADIUS = 56;
/** How far a finger may stray from a button and still count as on it. */
const SNAP_SLOP_PX = 22;
/** Drag distance that takes a trigger from fully pulled to released. */
const TRIGGER_TRAVEL_PX = 64;
const LAYOUT_KEY = 'vibepollo.guest.padLayout';

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

/* ------------------------------------------------------------------ layout -- */

type GroupKey =
  | 'left-shoulders'
  | 'left-stick'
  | 'dpad'
  | 'centre'
  | 'right-shoulders'
  | 'face'
  | 'right-stick';

interface GroupLayout {
  dx: number;
  dy: number;
  scale: number;
}

/**
 * Where each cluster sits, relative to where the stylesheet puts it.
 *
 * No fixed arrangement suits every hand, every phone and every game — which is
 * why every emulator front end that has lived long enough ends up shipping this.
 * Offsets rather than absolute positions, so the default layout can be changed
 * later without stranding everyone who moved one thing.
 */
const layout = reactive<Record<GroupKey, GroupLayout>>(loadLayout());

function defaultLayout(): Record<GroupKey, GroupLayout> {
  const zero = (): GroupLayout => ({ dx: 0, dy: 0, scale: 1 });
  return {
    'left-shoulders': zero(),
    'left-stick': zero(),
    dpad: zero(),
    centre: zero(),
    'right-shoulders': zero(),
    face: zero(),
    'right-stick': zero(),
  };
}

function loadLayout(): Record<GroupKey, GroupLayout> {
  const fallback = defaultLayout();
  try {
    const raw = window.localStorage.getItem(LAYOUT_KEY);
    if (!raw) return fallback;
    const saved = JSON.parse(raw) as Partial<Record<GroupKey, Partial<GroupLayout>>>;
    for (const key of Object.keys(fallback) as GroupKey[]) {
      const entry = saved[key];
      if (!entry) continue;
      const dx = Number(entry.dx);
      const dy = Number(entry.dy);
      const scale = Number(entry.scale);
      // A layout saved on a much larger screen must not push a control off a
      // small one, so offsets are clamped to something recoverable.
      if (Number.isFinite(dx)) fallback[key].dx = Math.max(-600, Math.min(600, dx));
      if (Number.isFinite(dy)) fallback[key].dy = Math.max(-400, Math.min(400, dy));
      if (Number.isFinite(scale)) fallback[key].scale = Math.max(0.6, Math.min(1.8, scale));
    }
  } catch {
    // Private browsing, or a hand-edited value. Defaults are always usable.
  }
  return fallback;
}

function saveLayout(): void {
  try {
    window.localStorage.setItem(LAYOUT_KEY, JSON.stringify(layout));
  } catch {
    /* the layout just will not persist */
  }
}

function groupStyle(key: GroupKey): Record<string, string> {
  const l = layout[key];
  return { transform: `translate(${l.dx}px, ${l.dy}px) scale(${l.scale})` };
}

const editing = ref(false);
const selectedGroup = ref<GroupKey | null>(null);

function toggleEditing(): void {
  editing.value = !editing.value;
  selectedGroup.value = null;
  if (!editing.value) saveLayout();
  // Editing and playing at once would fight over every pointer.
  releaseEverything();
}

function resetLayout(): void {
  Object.assign(layout, defaultLayout());
  saveLayout();
}

function nudgeScale(delta: number): void {
  const key = selectedGroup.value;
  if (!key) return;
  layout[key].scale = Math.max(0.6, Math.min(1.8, layout[key].scale + delta));
  // Saved here as well as on leaving edit mode: someone who resizes and then
  // closes the tab, or never notices Done, should not lose the change.
  saveLayout();
}

/* ------------------------------------------------------------- hit testing -- */

interface ControlHit {
  kind: 'btn' | 'trigger' | 'stick';
  index: number;
  rect: DOMRect;
}

/// Rects are cached while a gesture runs. Layout cannot shift underneath a finger
/// during play, and querying every control on every pointermove is the one thing
/// here that would actually cost frames.
let controlCache: ControlHit[] = [];

function refreshControlCache(): void {
  const host = root.value;
  if (!host) return;
  controlCache = [...host.querySelectorAll<HTMLElement>('[data-ctl]')].map((el) => {
    const [kind, index] = (el.dataset.ctl ?? '').split(':');
    return {
      kind: kind as ControlHit['kind'],
      index: Number(index),
      rect: el.getBoundingClientRect(),
    };
  });
}

/**
 * What is under this point, allowing for fingers that land slightly off.
 *
 * Exact hit testing is why on-screen buttons feel unreliable: the visual is
 * already smaller than a fingertip, and the contact patch is nowhere near where
 * the user thinks they are pointing. A control is claimed if the point is inside
 * it, and otherwise the nearest one within a slop radius wins — so the gaps
 * between buttons stop being dead zones.
 */
function controlAt(x: number, y: number): ControlHit | null {
  let nearest: ControlHit | null = null;
  let nearestDistance = Infinity;
  for (const control of controlCache) {
    const r = control.rect;
    if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) return control;
    const dx = x < r.left ? r.left - x : x > r.right ? x - r.right : 0;
    const dy = y < r.top ? r.top - y : y > r.bottom ? y - r.bottom : 0;
    const distance = Math.hypot(dx, dy);
    // Sticks never claim by proximity; drifting into one mid-gesture would yank
    // the camera.
    if (control.kind !== 'stick' && distance < nearestDistance && distance <= SNAP_SLOP_PX) {
      nearest = control;
      nearestDistance = distance;
    }
  }
  return nearest;
}

/* ---------------------------------------------------------- pointer routing -- */

type PointerRole =
  | { kind: 'stick'; stick: number; origin: { x: number; y: number } }
  | { kind: 'buttons'; indices: Set<number> }
  | { kind: 'trigger'; index: number; originY: number; value: number }
  | { kind: 'edit'; group: GroupKey; startX: number; startY: number; dx: number; dy: number };

const pointers = new Map<number, PointerRole>();

/// The pressed map is derived from every live pointer, never mutated ad hoc. Two
/// fingers can hold the same button and releasing one must not release it.
function recomputePressed(): void {
  const next: Record<number, number> = {};
  for (const role of pointers.values()) {
    if (role.kind === 'buttons') {
      for (const index of role.indices) next[index] = 1;
    } else if (role.kind === 'trigger') {
      next[role.index] = role.value;
    }
  }
  for (let i = 0; i < BUTTON_COUNT; i++) {
    const value = next[i] ?? 0;
    if ((pressed[i] ?? 0) !== value) pressed[i] = value;
  }
}

/// A short tick on first contact. It is the only feedback available on glass, and
/// its absence is most of why on-screen controls feel vague.
function tick(): void {
  if (vibrationBusyUntil > performance.now()) return;
  navigator.vibrate?.(8);
}

function onPointerDown(event: PointerEvent): void {
  const target = event.target as HTMLElement | null;
  // The editor's own chrome is ordinary UI and must keep working normally.
  if (target?.closest('[data-chrome]')) return;

  event.preventDefault();
  // Capture keeps the move and up events coming once a finger leaves the element
  // it started on, which is what makes sliding work. It is an optimisation, not a
  // prerequisite: it throws if the pointer is already gone, and letting that
  // escape would abort the handler and drop the press entirely.
  try {
    root.value?.setPointerCapture(event.pointerId);
  } catch {
    /* the gesture still works, it just stops tracking if the finger leaves */
  }
  refreshControlCache();

  if (editing.value) {
    const group = document
      .elementFromPoint(event.clientX, event.clientY)
      ?.closest<HTMLElement>('[data-group]')?.dataset.group as GroupKey | undefined;
    if (!group) return;
    selectedGroup.value = group;
    pointers.set(event.pointerId, {
      kind: 'edit',
      group,
      startX: event.clientX,
      startY: event.clientY,
      dx: layout[group].dx,
      dy: layout[group].dy,
    });
    return;
  }

  const hit = controlAt(event.clientX, event.clientY);
  if (!hit) return;

  if (hit.kind === 'stick') {
    pointers.set(event.pointerId, {
      kind: 'stick',
      stick: hit.index,
      origin: { x: event.clientX, y: event.clientY },
    });
    return;
  }
  if (hit.kind === 'trigger') {
    // A tap is a full pull, which is what almost every game wants and matches the
    // behaviour this replaced. Sliding away from the button eases off, and the
    // fill on the button shows how far in it is.
    pointers.set(event.pointerId, {
      kind: 'trigger',
      index: hit.index,
      originY: event.clientY,
      value: 1,
    });
  } else {
    pointers.set(event.pointerId, { kind: 'buttons', indices: new Set([hit.index]) });
  }
  tick();
  recomputePressed();
}

function onPointerMove(event: PointerEvent): void {
  const role = pointers.get(event.pointerId);
  if (!role) return;
  event.preventDefault();

  if (role.kind === 'edit') {
    const l = layout[role.group];
    l.dx = role.dx + (event.clientX - role.startX);
    l.dy = role.dy + (event.clientY - role.startY);
    return;
  }

  if (role.kind === 'stick') {
    const dx = (event.clientX - role.origin.x) / STICK_RADIUS;
    const dy = (event.clientY - role.origin.y) / STICK_RADIUS;
    const length = Math.hypot(dx, dy);
    const scale = length > 1 ? 1 / length : 1;
    axes[role.stick * 2] = dx * scale;
    axes[role.stick * 2 + 1] = dy * scale;
    return;
  }

  if (role.kind === 'trigger') {
    const pulled = 1 - Math.abs(event.clientY - role.originY) / TRIGGER_TRAVEL_PX;
    role.value = Math.max(0, Math.min(1, pulled));
    recomputePressed();
    return;
  }

  // Buttons: whatever is under the finger now is what is held. Sliding onto a
  // second button adds it, which is how a combo gets pressed with one thumb.
  const hit = controlAt(event.clientX, event.clientY);
  const next = new Set<number>();
  if (hit && hit.kind === 'btn') next.add(hit.index);
  const changed = next.size !== role.indices.size || [...next].some((i) => !role.indices.has(i));
  if (changed) {
    if (next.size && ![...next].every((i) => role.indices.has(i))) tick();
    role.indices = next;
    recomputePressed();
  }
}

function onPointerUp(event: PointerEvent): void {
  const role = pointers.get(event.pointerId);
  if (!role) return;
  if (role.kind === 'stick') {
    axes[role.stick * 2] = 0;
    axes[role.stick * 2 + 1] = 0;
  }
  if (role.kind === 'edit') saveLayout();
  pointers.delete(event.pointerId);
  recomputePressed();
}

function releaseEverything(): void {
  pointers.clear();
  axes[0] = axes[1] = axes[2] = axes[3] = 0;
  recomputePressed();
}

/* ---------------------------------------------------------------- gyro aim -- */

/**
 * Tilt as a second right stick.
 *
 * The right thumb has to aim and press face buttons at the same time, and it
 * cannot do both. Tilt takes over the fine aim, which is the part a thumb is
 * worst at, and leaves the buttons free. It only contributes while the right
 * stick is untouched, so a deliberate stick input always wins.
 */
const gyroOn = ref(false);
const gyroUnavailable = ref(false);
let gyroBase: { beta: number; gamma: number } | null = null;
const GYRO_RANGE_DEG = 22;
const GYRO_DEADZONE = 0.12;

function onOrientation(event: DeviceOrientationEvent): void {
  if (!gyroOn.value) return;
  const beta = event.beta ?? 0;
  const gamma = event.gamma ?? 0;
  if (!gyroBase) {
    // Calibrate to however the phone is being held right now, not to flat.
    gyroBase = { beta, gamma };
    return;
  }
  // The right stick is being driven by a thumb; leave it alone.
  for (const role of pointers.values()) {
    if (role.kind === 'stick' && role.stick === 1) return;
  }
  const apply = (delta: number): number => {
    const v = Math.max(-1, Math.min(1, delta / GYRO_RANGE_DEG));
    return Math.abs(v) < GYRO_DEADZONE ? 0 : v;
  };
  axes[2] = apply(gamma - gyroBase.gamma);
  axes[3] = apply(beta - gyroBase.beta);
}

async function toggleGyro(): Promise<void> {
  if (gyroOn.value) {
    gyroOn.value = false;
    gyroBase = null;
    axes[2] = 0;
    axes[3] = 0;
    return;
  }
  // iOS demands the request come from a user gesture, which this click is.
  const anyEvent = DeviceOrientationEvent as unknown as {
    requestPermission?: () => Promise<'granted' | 'denied'>;
  };
  try {
    if (typeof anyEvent?.requestPermission === 'function') {
      const result = await anyEvent.requestPermission();
      if (result !== 'granted') {
        gyroUnavailable.value = true;
        return;
      }
    } else if (typeof DeviceOrientationEvent === 'undefined') {
      gyroUnavailable.value = true;
      return;
    }
  } catch {
    gyroUnavailable.value = true;
    return;
  }
  gyroBase = null;
  gyroOn.value = true;
}

/* --------------------------------------------------------------- lifecycle -- */

/// Read from `pressed` rather than from the pointer map: recomputePressed already
/// stores the analog value there, and `pressed` is reactive where a plain Map is
/// not — a computed over the Map simply never re-runs.
const triggerFill = computed<Record<number, number>>(() => ({
  6: pressed[6] ?? 0,
  7: pressed[7] ?? 0,
}));

function isHeld(index: number): boolean {
  return (pressed[index] ?? 0) > 0;
}

function stickStyle(stick: number): Record<string, string> {
  return {
    transform: `translate(${(axes[stick * 2] ?? 0) * 26}px, ${(axes[stick * 2 + 1] ?? 0) * 26}px)`,
  };
}

onMounted(() => {
  raf = requestAnimationFrame(pump);
  window.addEventListener('deviceorientation', onOrientation);
  window.addEventListener('resize', refreshControlCache);
});
onBeforeUnmount(() => {
  cancelAnimationFrame(raf);
  window.removeEventListener('deviceorientation', onOrientation);
  window.removeEventListener('resize', refreshControlCache);
  // Do not leave the phone buzzing after the controls are gone.
  navigator.vibrate?.(0);
  // Withdraw it, or the capture loop keeps reporting a pad nobody is holding.
  setVirtualGamepad();
});
</script>

<template>
  <div
    ref="root"
    class="touchpad"
    :class="{ 'touchpad--editing': editing }"
    aria-label="On-screen controller"
    @pointerdown.stop="onPointerDown"
    @pointermove.stop="onPointerMove"
    @pointerup.stop="onPointerUp"
    @pointercancel.stop="onPointerUp"
    @lostpointercapture.stop="onPointerUp"
    @contextmenu.stop.prevent
  >
    <!-- Bumpers and triggers ride the top edge, where the index fingers already
         rest when a phone is held in landscape. They used to sit in a column
         beside the thumbs, which is the one place on the screen no finger is
         near. -->
    <div class="edge edge--left" data-group="left-shoulders" :style="groupStyle('left-shoulders')">
      <button class="tbtn tbtn--edge" data-ctl="btn:4" :class="{ 'is-held': isHeld(4) }">LB</button>
      <button
        class="tbtn tbtn--edge tbtn--trigger"
        data-ctl="trigger:6"
        :class="{ 'is-held': isHeld(6) }"
        :style="{ '--pull': triggerFill[6] ?? 0 }"
      >
        LT
      </button>
    </div>
    <div class="edge edge--right" data-group="right-shoulders" :style="groupStyle('right-shoulders')">
      <button
        class="tbtn tbtn--edge tbtn--trigger"
        data-ctl="trigger:7"
        :class="{ 'is-held': isHeld(7) }"
        :style="{ '--pull': triggerFill[7] ?? 0 }"
      >
        RT
      </button>
      <button class="tbtn tbtn--edge" data-ctl="btn:5" :class="{ 'is-held': isHeld(5) }">RB</button>
    </div>

    <div class="touchpad__side touchpad__side--left">
      <div class="stick" data-ctl="stick:0" data-group="left-stick" :style="groupStyle('left-stick')">
        <div class="stick__cap" :style="stickStyle(0)"></div>
      </div>
      <div class="dpad" data-group="dpad" :style="groupStyle('dpad')">
        <button class="tbtn dpad__up" data-ctl="btn:12" :class="{ 'is-held': isHeld(12) }">▲</button>
        <button class="tbtn dpad__left" data-ctl="btn:14" :class="{ 'is-held': isHeld(14) }">◀</button>
        <button class="tbtn dpad__right" data-ctl="btn:15" :class="{ 'is-held': isHeld(15) }">▶</button>
        <button class="tbtn dpad__down" data-ctl="btn:13" :class="{ 'is-held': isHeld(13) }">▼</button>
      </div>
    </div>

    <div class="touchpad__centre" data-group="centre" :style="groupStyle('centre')">
      <button class="tbtn tbtn--small" data-ctl="btn:8" :class="{ 'is-held': isHeld(8) }">
        Back
      </button>
      <button class="tbtn tbtn--small" data-ctl="btn:9" :class="{ 'is-held': isHeld(9) }">
        Start
      </button>
    </div>

    <div class="touchpad__side touchpad__side--right">
      <div class="face" data-group="face" :style="groupStyle('face')">
        <button class="tbtn face__y" data-ctl="btn:3" :class="{ 'is-held': isHeld(3) }">Y</button>
        <button class="tbtn face__x" data-ctl="btn:2" :class="{ 'is-held': isHeld(2) }">X</button>
        <button class="tbtn face__b" data-ctl="btn:1" :class="{ 'is-held': isHeld(1) }">B</button>
        <button class="tbtn face__a" data-ctl="btn:0" :class="{ 'is-held': isHeld(0) }">A</button>
      </div>
      <div class="stick" data-ctl="stick:1" data-group="right-stick" :style="groupStyle('right-stick')">
        <div class="stick__cap" :style="stickStyle(1)"></div>
      </div>
    </div>

    <!-- data-chrome marks the editor's own controls, which the pointer router
         leaves alone so they behave like ordinary buttons. -->
    <div class="padchrome" data-chrome>
      <button class="padchrome__btn" :class="{ 'is-on': gyroOn }" @click="toggleGyro">
        {{ gyroUnavailable ? 'No gyro' : gyroOn ? 'Gyro on' : 'Gyro' }}
      </button>
      <button class="padchrome__btn" :class="{ 'is-on': editing }" @click="toggleEditing">
        {{ editing ? 'Done' : 'Move' }}
      </button>
      <template v-if="editing">
        <button class="padchrome__btn" :disabled="!selectedGroup" @click="nudgeScale(-0.1)">
          −
        </button>
        <button class="padchrome__btn" :disabled="!selectedGroup" @click="nudgeScale(0.1)">+</button>
        <button class="padchrome__btn" @click="resetLayout">Reset</button>
      </template>
    </div>

    <p v-if="editing" class="padhint" data-chrome>
      Drag a control to move it. Select one, then − and + to resize.
    </p>
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

/* Holding a button used to raise the text-selection callout and the magnifier,
   because a long press on glass is a selection gesture unless a page says
   otherwise. user-select alone does not stop it on iOS; -webkit-touch-callout
   is the one that does, and both have to reach every descendant, not just the
   container. */
.touchpad,
.touchpad * {
  -webkit-touch-callout: none;
  -webkit-user-select: none;
  user-select: none;
  -webkit-tap-highlight-color: transparent;
}

/* The whole surface takes pointers now, because a finger that slides off a
   button still has to be tracked. Nothing is drawn here, so the video is
   unobscured either way.

   Every pointer handler stops propagation, which together with this means the
   mouse is inert while the on-screen controller is up. That is deliberate: the
   page forwards pointermove on the stream surface as host mouse movement, so
   without it a thumb on the left stick both steered the stick AND dragged the
   mouse across the game. The old per-button handlers each carried .stop and it
   was lost when they were centralised here. A pad and a mouse cannot share one
   finger. */
.touchpad {
  pointer-events: auto;
}

.touchpad__side,
.touchpad__centre {
  display: flex;
  gap: 10px;
  align-items: flex-end;
}

/* Bottom centre, not top. At the top these collided with the page's own Stats,
   Quality, Fullscreen and Leave buttons — two sets of controls in the same
   corner, one of them the game's. */
.touchpad__centre {
  align-self: flex-end;
  gap: 6px;
  padding-bottom: 4px;
}

.edge {
  position: absolute;
  top: 8px;
  display: flex;
  gap: 8px;
}
.edge--left {
  left: 12px;
}
.edge--right {
  right: 12px;
}

.tbtn {
  display: grid;
  place-items: center;
  width: 52px;
  height: 52px;
  /* Padding is inside the hit area but outside the paint, so the touch target is
     larger than the circle without the circle looking bloated. Fingertips are
     wider than any button worth drawing. */
  padding: 7px;
  border: 1px solid rgb(255 255 255 / 35%);
  border-radius: 50%;
  color: #fff;
  font-size: 13px;
  font-weight: 600;
  background: rgb(0 0 0 / 35%);
  background-clip: content-box;
  backdrop-filter: blur(2px);
  touch-action: none;
}

/* :active never fires now that pointers are handled on the container, so held
   state is driven by the gamepad snapshot instead — which also means a button
   held by a slid finger lights up, and one held by two fingers stays lit when
   one lifts. */
.tbtn.is-held {
  background: rgb(79 140 255 / 70%);
}

.tbtn--edge {
  width: 74px;
  height: 40px;
  border-radius: 12px;
}

/* Fill shows how far the trigger is pulled, which is the only way an analog
   value on a flat surface is discoverable at all. */
.tbtn--trigger {
  position: relative;
  overflow: hidden;
}
.tbtn--trigger::after {
  position: absolute;
  bottom: 0;
  left: 0;
  width: 100%;
  height: calc(var(--pull, 0) * 100%);
  background: rgb(79 140 255 / 55%);
  content: '';
  pointer-events: none;
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

.touchpad--editing [data-group] {
  outline: 1px dashed rgb(79 140 255 / 80%);
  outline-offset: 4px;
}

.padchrome {
  position: absolute;
  top: 8px;
  left: 50%;
  display: flex;
  gap: 6px;
  translate: -50% 0;
}
.padchrome__btn {
  padding: 5px 10px;
  border: 1px solid rgb(255 255 255 / 30%);
  border-radius: 999px;
  background: rgb(0 0 0 / 45%);
  color: rgb(255 255 255 / 85%);
  font-size: 11px;
}
.padchrome__btn.is-on {
  border-color: rgb(79 140 255 / 90%);
  background: rgb(79 140 255 / 70%);
  color: #fff;
}
.padchrome__btn:disabled {
  opacity: 0.4;
}
.padhint {
  position: absolute;
  top: 40px;
  left: 50%;
  margin: 0;
  color: rgb(255 255 255 / 75%);
  font-size: 11px;
  text-align: center;
  translate: -50% 0;
}
</style>
