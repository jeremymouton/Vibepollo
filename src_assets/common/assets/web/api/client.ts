export type ApiPayload = Record<string, unknown> | unknown[] | string | number | boolean | null;

export type ApiErrorCode =
  | 'invalid-json'
  | 'csrf-request-failed'
  | 'csrf-token-missing'
  | 'request-failed';

export class ApiError extends Error {
  readonly code: ApiErrorCode;
  readonly status: number;
  readonly payload: unknown;

  constructor(code: ApiErrorCode, status: number, payload?: unknown) {
    super(code);
    this.name = 'ApiError';
    this.code = code;
    this.status = status;
    this.payload = payload;
  }
}

interface ApiRequestOptions extends Omit<RequestInit, 'body'> {
  body?: BodyInit | null;
  json?: ApiPayload;
}

let csrfToken: string | null = null;
let csrfRequest: Promise<string> | null = null;
let refreshRequest: Promise<boolean> | null = null;
let sessionLostHandler: (() => void) | null = null;

/**
 * Endpoints that must never trigger a refresh attempt.
 *
 * The session token lives about two hours; "stay signed in" issues a refresh
 * token good for a week alongside it, which is redeemed for a new session token
 * when the short one lapses. This app never redeemed it, so the refresh token was
 * issued at login and left unused and the admin signed you out every two hours
 * whatever you ticked. Refreshing from inside these endpoints would recurse.
 */
const AUTH_PATHS = [
  '/api/auth/refresh',
  '/api/auth/login',
  '/api/auth/logout',
  '/api/auth/status',
  '/api/password',
];

function isAuthPath(path: string): boolean {
  const [clean] = path.split('?');
  return AUTH_PATHS.some((candidate) => clean === candidate);
}

/**
 * Called when a request was refused and the refresh token could not save it.
 *
 * The store registers this so the app can show its login view. Without it an
 * expired session surfaced only as failed data requests, which the dashboard
 * reported as "host status unavailable" — the host was fine, the session was not.
 */
export function setSessionLostHandler(handler: () => void): void {
  sessionLostHandler = handler;
}

/**
 * Redeem the refresh token for a fresh session token.
 *
 * Concurrent callers share one attempt, so a page whose panels all fail at once
 * refreshes a single time. Exported because a page LOAD needs it too: the host's
 * auth-status check reads only the session cookie and knows nothing about the
 * refresh token, so a returning tab is told a login is required while a week-long
 * token sits unused in the jar.
 */
export async function refreshSessionToken(): Promise<boolean> {
  if (refreshRequest) return refreshRequest;

  refreshRequest = (async () => {
    try {
      // Through apiRequest so the CSRF token is attached: the route validates it
      // (every POST does) even though it authenticates itself.
      const payload = await apiRequest<{ status?: unknown }>(
        '/api/auth/refresh',
        { method: 'POST', json: {} },
        true,
        false,
      );
      return payload?.status === true;
    } catch {
      // An expired or revoked refresh token answers 401. Nothing to recover.
      return false;
    }
  })();

  try {
    return await refreshRequest;
  } finally {
    refreshRequest = null;
  }
}

function isMutation(method: string): boolean {
  return ['POST', 'PUT', 'PATCH', 'DELETE'].includes(method);
}

function isCsrfFailure(payload: unknown): boolean {
  if (!payload || typeof payload !== 'object') return false;
  const error = (payload as { error?: unknown }).error;
  return typeof error === 'string' && error.toLocaleLowerCase().includes('csrf');
}

async function parseResponse(response: Response): Promise<unknown> {
  if (response.status === 204) return null;

  const text = await response.text();
  if (!text) return null;

  const contentType = response.headers.get('content-type') ?? '';
  if (contentType.includes('json')) {
    try {
      return JSON.parse(text) as unknown;
    } catch {
      throw new ApiError('invalid-json', response.status, text);
    }
  }
  return text;
}

async function requestCsrfToken(): Promise<string> {
  if (csrfToken) return csrfToken;
  if (csrfRequest) return csrfRequest;

  csrfRequest = (async () => {
    const response = await fetch('/api/csrf-token', {
      credentials: 'same-origin',
      headers: { Accept: 'application/json' },
    });
    const payload = await parseResponse(response);
    if (!response.ok) {
      throw new ApiError('csrf-request-failed', response.status, payload);
    }
    const token = (payload as { csrf_token?: unknown } | null)?.csrf_token;
    if (typeof token !== 'string' || !token) {
      throw new ApiError('csrf-token-missing', response.status, payload);
    }
    csrfToken = token;
    return token;
  })();

  try {
    return await csrfRequest;
  } finally {
    csrfRequest = null;
  }
}

export function clearCsrfToken(): void {
  csrfToken = null;
  csrfRequest = null;
}

export async function apiRequest<T>(
  path: string,
  options: ApiRequestOptions = {},
  retryCsrf = true,
  retryAuth = true,
): Promise<T> {
  const method = (options.method ?? 'GET').toUpperCase();
  const headers = new Headers(options.headers);
  headers.set('Accept', 'application/json');

  let body = options.body;
  if (options.json !== undefined) {
    headers.set('Content-Type', 'application/json');
    body = JSON.stringify(options.json);
  }

  if (isMutation(method)) {
    headers.set('X-CSRF-Token', await requestCsrfToken());
  }

  const response = await fetch(path, {
    ...options,
    body,
    credentials: options.credentials ?? 'same-origin',
    headers,
    method,
  });
  const payload = await parseResponse(response);

  if (!response.ok) {
    if (
      retryCsrf &&
      isMutation(method) &&
      response.status === 400 &&
      isCsrfFailure(payload)
    ) {
      clearCsrfToken();
      return apiRequest<T>(path, options, false);
    }
    // A 401 is usually just the two-hour session token lapsing. Redeem the
    // refresh token and replay the request once; only when that fails is the
    // session really gone, and then the app is told so it can ask for a login
    // rather than leaving the caller to report it as a host problem.
    if (response.status === 401 && retryAuth && !isAuthPath(path)) {
      clearCsrfToken();
      if (await refreshSessionToken()) {
        return apiRequest<T>(path, options, true, false);
      }
      sessionLostHandler?.();
    }
    if (response.status === 401 || response.status === 403) clearCsrfToken();
    throw new ApiError('request-failed', response.status, payload);
  }

  return payload as T;
}

export function apiGet<T>(path: string, options: ApiRequestOptions = {}): Promise<T> {
  return apiRequest<T>(path, { ...options, method: 'GET' });
}

export function apiPost<T>(path: string, json?: ApiPayload): Promise<T> {
  return apiRequest<T>(path, { method: 'POST', json: json ?? {} });
}

export function apiPatch<T>(path: string, json: ApiPayload): Promise<T> {
  return apiRequest<T>(path, { method: 'PATCH', json });
}

export function apiDelete<T>(path: string, json?: ApiPayload): Promise<T> {
  return apiRequest<T>(path, { method: 'DELETE', json });
}
