<script setup lang="ts">
/**
 * The running stream, kept in the corner while the owner is elsewhere in the app.
 *
 * Shows the same MediaStream the stream page is showing rather than a second
 * connection — a MediaStream can feed any number of video elements, so this costs
 * nothing beyond another decode surface, and the session is untouched.
 *
 * Plays the audio too. The stream page is kept alive behind this, but kept alive
 * is not playing: when KeepAlive lifts the page out of the document its media
 * elements pause, so for as long as this player is on screen it is the only sound
 * source. It is shown only off the stream route, so the two never overlap.
 */
import { onBeforeUnmount, ref, watch } from 'vue';
import { useRouter } from 'vue-router';

import { useActiveStream } from '@/stores/activeStream';

const stream = useActiveStream();
const router = useRouter();
const videoEl = ref<HTMLVideoElement>();

function bind(): void {
  const el = videoEl.value;
  if (!el) return;
  const media = stream.media.value;
  el.volume = stream.volume.value;
  if (el.srcObject !== (media ?? null)) {
    el.srcObject = media ?? null;
    if (media) void el.play().catch(() => undefined);
  }
}

watch([() => stream.media.value, () => stream.volume.value, videoEl], bind, { immediate: true });
onBeforeUnmount(() => {
  if (videoEl.value) videoEl.value.srcObject = null;
});

function backToStream(): void {
  void router.push('/stream');
}
</script>

<template>
  <div v-if="stream.connected.value" class="mini-stream">
    <video ref="videoEl" autoplay playsinline disablepictureinpicture />
    <button class="mini-stream__open" type="button" @click="backToStream">
      Back to stream<template v-if="stream.appName.value"> · {{ stream.appName.value }}</template>
    </button>
  </div>
</template>

<style scoped>
.mini-stream {
  position: fixed;
  right: 1rem;
  bottom: 1rem;
  z-index: 40;
  overflow: hidden;
  width: min(20rem, 40vw);
  border: 1px solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-control);
  background: #000;
  box-shadow: 0 0.5rem 1.5rem rgb(0 0 0 / 45%);
}

.mini-stream video {
  display: block;
  width: 100%;
  aspect-ratio: 16 / 9;
  object-fit: contain;
  background: #000;
}

.mini-stream__open {
  display: block;
  width: 100%;
  padding: 0.375rem 0.5rem;
  border: 0;
  color: #fff;
  font-size: 0.75rem;
  text-align: left;
  background: rgb(0 0 0 / 70%);
  cursor: pointer;
}

.mini-stream__open:hover {
  background: var(--vs-color-accent-default);
}

@media (max-width: 47.999rem) {
  .mini-stream {
    width: min(12rem, 45vw);
  }
}
</style>
