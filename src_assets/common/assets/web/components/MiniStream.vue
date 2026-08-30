<script setup lang="ts">
/**
 * The running stream, kept in the corner while the owner is elsewhere in the app.
 *
 * Shows the same MediaStream the stream page is showing rather than a second
 * connection — a MediaStream can feed any number of video elements, so this costs
 * nothing beyond another decode surface, and the session is untouched.
 *
 * Muted on purpose: the audio is already playing on the stream page, which is
 * still mounted behind this. Two elements playing the same audio would echo.
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
  if (el.srcObject !== (media ?? null)) {
    el.srcObject = media ?? null;
    if (media) void el.play().catch(() => undefined);
  }
}

watch([() => stream.media.value, videoEl], bind, { immediate: true });
onBeforeUnmount(() => {
  if (videoEl.value) videoEl.value.srcObject = null;
});

function backToStream(): void {
  void router.push('/stream');
}
</script>

<template>
  <div v-if="stream.connected.value" class="mini-stream">
    <video ref="videoEl" autoplay muted playsinline disablepictureinpicture />
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
