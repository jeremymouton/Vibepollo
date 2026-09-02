import { computed, ref, shallowRef } from 'vue';

/**
 * The stream that is currently running, shared across the whole app.
 *
 * The stream page owns the connection, but the owner should be able to open
 * Invites or Devices without dropping it — walking away from the page is not the
 * same as wanting to stop playing. So the page is kept alive across navigation
 * and publishes its media here; a small player elsewhere binds to the very same
 * MediaStream, which several video elements may share.
 */
const media = shallowRef<MediaStream>();
const connected = ref(false);
const appName = ref('');
/// 0..1, the stream page's slider. The mini player plays the audio while the page
/// is off screen, and it should be as loud as the owner left it.
const volume = ref(1);

export function useActiveStream() {
  return {
    media: computed(() => media.value),
    connected: computed(() => connected.value),
    appName: computed(() => appName.value),
    volume: computed(() => volume.value),

    /// Called by the stream page when media arrives or the session ends. The stream
    /// carries video AND audio: the page's own media elements pause whenever the
    /// page is kept alive off screen, so the mini player is the only thing that can
    /// play the game's sound in the meantime.
    publish(stream?: MediaStream, name = ''): void {
      media.value = stream;
      connected.value = Boolean(stream);
      appName.value = stream ? name : '';
    },

    setVolume(value: number): void {
      volume.value = Math.min(1, Math.max(0, value));
    },
  };
}
