<script setup lang="ts">
import { useRoute } from 'vue-router';

import MiniStream from '@/components/MiniStream.vue';
import { onMounted } from 'vue';
import { useI18n } from 'vue-i18n';

import AppShell from '@/components/layout/AppShell.vue';
import AuthView from '@/views/AuthView.vue';
import { useSystemStore } from '@/stores/system';

const system = useSystemStore();
const route = useRoute();
const { t } = useI18n();

onMounted(() => {
  void system.initialize();
});
</script>

<template>
  <div v-if="system.booting" class="boot-screen" role="status" aria-live="polite">
    <img src="/images/logo-apollo-45.png" alt="" width="45" height="45" />
    <div>
      <strong>Vibepollo</strong>
      <span>{{ t('ui.app.connecting') }}</span>
    </div>
  </div>

  <AuthView v-else-if="system.needsSetup || system.needsLogin" />

  <AppShell v-else>
    <!-- The stream page is kept alive so navigating to Invites or Devices does not
         tear the session down. Leaving the page is not the same as wanting to stop
         playing, and reconnecting costs a renegotiation and a fresh encoder. -->
    <RouterView v-slot="{ Component }">
      <KeepAlive :include="['BrowserStreamView']">
        <component :is="Component" />
      </KeepAlive>
    </RouterView>
  </AppShell>

  <!-- Outside AppShell so it is not clipped by the page container, and shown only
       while a stream is running somewhere other than the page it belongs to. -->
  <MiniStream v-if="!system.needsSetup && !system.needsLogin && route.path !== '/stream'" />

  <div class="visually-hidden" aria-live="polite" aria-atomic="true">
    {{ system.error ? t(system.error) : '' }}
  </div>
</template>
