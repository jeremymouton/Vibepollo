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

export function useActiveStream() {
  return {
    media: computed(() => media.value),
    connected: computed(() => connected.value),
    appName: computed(() => appName.value),

    /// Called by the stream page when media arrives or the session ends.
    publish(stream?: MediaStream, name = ''): void {
      media.value = stream;
      connected.value = Boolean(stream);
      appName.value = stream ? name : '';
    },
  };
}
