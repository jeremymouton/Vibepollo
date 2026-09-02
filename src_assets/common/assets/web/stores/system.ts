import { computed, ref } from 'vue';
import { defineStore } from 'pinia';

import {
  ApiError,
  apiGet,
  apiPost,
  clearCsrfToken,
  refreshSessionToken,
  setSessionLostHandler,
} from '@/api/client';
import type { SessionStatus } from '@/types/sessions';

export interface AuthStatus {
  authenticated: boolean;
  credentials_configured: boolean;
  login_required: boolean;
}

export interface HostMetadata {
  platform?: string;
  status?: boolean;
  version?: string;
  commit?: string;
  branch?: string;
  release_date?: string;
  [key: string]: unknown;
}

export type ThemePreference = 'auto' | 'dark' | 'light';

const defaultAuth: AuthStatus = {
  authenticated: false,
  credentials_configured: true,
  login_required: true,
};

function storedBoolean(key: string, fallback: boolean): boolean {
  const value = localStorage.getItem(key);
  return value === null ? fallback : value === 'true';
}

function readThemePreference(): ThemePreference {
  const value = localStorage.getItem('vibepollo.theme');
  return value === 'dark' || value === 'light' || value === 'auto' ? value : 'auto';
}

export const useSystemStore = defineStore('system', () => {
  const booting = ref(true);
  const auth = ref<AuthStatus>({ ...defaultAuth });
  const metadata = ref<HostMetadata | null>(null);
  const session = ref<SessionStatus | null>(null);
  const loadingHost = ref(false);
  const error = ref('');
  const navCollapsed = ref(storedBoolean('vibepollo.nav-collapsed', false));
  const mobileNavOpen = ref(false);
  const theme = ref<ThemePreference>(readThemePreference());
  const lastUpdatedAt = ref<number | null>(null);

  const needsSetup = computed(() => !auth.value.credentials_configured);
  const needsLogin = computed(
    () => auth.value.credentials_configured && auth.value.login_required,
  );
  const canUseApp = computed(() => !needsSetup.value && !needsLogin.value);
  const isStreaming = computed(
    () => Boolean(session.value?.appRunning || (session.value?.activeSessions ?? 0) > 0),
  );
  const health = computed<'healthy' | 'streaming' | 'warning'>(() => {
    if (error.value) return 'warning';
    if (isStreaming.value) return 'streaming';
    return 'healthy';
  });

  function applyTheme(): void {
    const resolved =
      theme.value === 'auto'
        ? window.matchMedia('(prefers-color-scheme: light)').matches
          ? 'light'
          : 'dark'
        : theme.value;
    document.documentElement.dataset.theme = resolved;
    const themeMeta = document.querySelector<HTMLMetaElement>('meta[name="theme-color"]');
    themeMeta?.setAttribute('content', resolved === 'dark' ? '#060a18' : '#fff8ee');
  }

  function setTheme(value: ThemePreference): void {
    theme.value = value;
    localStorage.setItem('vibepollo.theme', value);
    applyTheme();
  }

  function toggleNav(): void {
    navCollapsed.value = !navCollapsed.value;
    localStorage.setItem('vibepollo.nav-collapsed', String(navCollapsed.value));
  }

  async function fetchAuthStatus(): Promise<AuthStatus> {
    const result = await apiGet<AuthStatus>('/api/auth/status');
    auth.value = result;
    return result;
  }

  /// The session is gone and the refresh token could not save it. Drop the host
  /// data with it: leaving the last snapshot on screen behind a login prompt shows
  /// stale numbers as if they were current.
  function onSessionLost(): void {
    auth.value = { ...auth.value, authenticated: false, login_required: true };
    metadata.value = null;
    session.value = null;
    error.value = '';
  }

  setSessionLostHandler(onSessionLost);

  function isAuthFailure(reason: unknown): boolean {
    return reason instanceof ApiError && (reason.status === 401 || reason.status === 403);
  }

  async function refreshHost(): Promise<void> {
    if (loadingHost.value || needsLogin.value || needsSetup.value) return;
    loadingHost.value = true;
    try {
      const [metadataResult, sessionResult] = await Promise.allSettled([
        apiGet<HostMetadata>('/api/metadata'),
        apiGet<SessionStatus>('/api/session/status'),
      ]);
      if (metadataResult.status === 'fulfilled') metadata.value = metadataResult.value;
      if (sessionResult.status === 'fulfilled') session.value = sessionResult.value;

      const failure = [metadataResult, sessionResult].find(
        (result): result is PromiseRejectedResult => result.status === 'rejected',
      );
      // A refused request is not an unavailable host. Both were reported as
      // "host status unavailable", so an expired session left that banner on the
      // dashboard indefinitely with nothing to say the sign-in had lapsed.
      if (failure && isAuthFailure(failure.reason)) {
        onSessionLost();
        return;
      }
      error.value = failure ? 'ui.system.host_status_unavailable' : '';
      lastUpdatedAt.value = Date.now();
    } finally {
      loadingHost.value = false;
    }
  }

  async function initialize(): Promise<void> {
    booting.value = true;
    error.value = '';
    applyTheme();
    try {
      await fetchAuthStatus();
      // The host decides "login required" from the session cookie alone — it does
      // not look at the refresh token. So a tab reopened after the session's two
      // hours is told to sign in while a "stay signed in" token good for a week
      // sits unused. Spend it before asking for a password.
      if (needsLogin.value && (await refreshSessionToken())) {
        await fetchAuthStatus();
      }
      await refreshHost();
    } catch {
      error.value = 'ui.system.unreachable';
    } finally {
      booting.value = false;
    }
  }

  async function login(username: string, password: string, rememberMe: boolean): Promise<void> {
    await apiPost('/api/auth/login', {
      password,
      remember_me: rememberMe,
      username,
    });
    clearCsrfToken();
    await fetchAuthStatus();
    await refreshHost();
  }

  async function createCredentials(
    username: string,
    password: string,
    confirmPassword: string,
  ): Promise<void> {
    await apiPost('/api/password', {
      confirmNewPassword: confirmPassword,
      currentPassword: '',
      currentUsername: '',
      newPassword: password,
      newUsername: username,
    });
    clearCsrfToken();
    await fetchAuthStatus();
  }

  async function logout(): Promise<void> {
    try {
      await apiPost('/api/auth/logout');
    } finally {
      clearCsrfToken();
      metadata.value = null;
      session.value = null;
      await fetchAuthStatus();
    }
  }

  const media = window.matchMedia('(prefers-color-scheme: light)');
  media.addEventListener('change', () => {
    if (theme.value === 'auto') applyTheme();
  });

  return {
    applyTheme,
    auth,
    booting,
    canUseApp,
    createCredentials,
    error,
    fetchAuthStatus,
    health,
    initialize,
    isStreaming,
    lastUpdatedAt,
    loadingHost,
    login,
    logout,
    metadata,
    mobileNavOpen,
    navCollapsed,
    needsLogin,
    needsSetup,
    refreshHost,
    session,
    setTheme,
    theme,
    toggleNav,
  };
});
