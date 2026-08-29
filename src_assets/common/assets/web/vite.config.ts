import { resolve } from 'node:path';
import { fileURLToPath, URL } from 'node:url';

import vue from '@vitejs/plugin-vue';
import { defineConfig } from 'vite';

const configuredOutputDirectory = process.env.SUNSHINE_WEB_OUTPUT_DIR;

export default defineConfig({
  plugins: [vue()],
  base: '/v2/',
  resolve: {
    // Ordered: the webrtc utilities live in the legacy tree and are shared, which is
    // what the .d.ts stubs under utils/webrtc describe. Everything else is local.
    alias: [
      {
        find: '@/components/TouchGamepad.vue',
        replacement: fileURLToPath(
          new URL('../web-legacy/components/TouchGamepad.vue', import.meta.url),
        ),
      },
      {
        find: /^@\/utils\/webrtc\/(.*)$/,
        replacement: fileURLToPath(new URL('../web-legacy/utils/webrtc/$1', import.meta.url)),
      },
      { find: '@', replacement: fileURLToPath(new URL('.', import.meta.url)) },
    ],
  },
  build: {
    outDir: configuredOutputDirectory
      ? resolve(configuredOutputDirectory)
      : fileURLToPath(new URL('../../../../build/assets/web/v2', import.meta.url)),
    emptyOutDir: true,
    target: 'es2022',
  },
  server: {
    host: '127.0.0.1',
    port: 5173,
    proxy: {
      '/api': {
        target: 'https://127.0.0.1:47990',
        changeOrigin: true,
        secure: false,
      },
      '/covers': {
        target: 'https://127.0.0.1:47990',
        changeOrigin: true,
        secure: false,
      },
    },
  },
});
