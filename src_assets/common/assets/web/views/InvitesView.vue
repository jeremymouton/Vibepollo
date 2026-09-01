<script setup lang="ts">
/**
 * Guest invites — create a link that lets a friend join without an account.
 *
 * The page deliberately does no permission arithmetic. The API returns a
 * `permission_summary` phrase, a ready-to-paste `path`, and a derived `live`
 * flag, so "what does this link hand out" and "can it still be used" have one
 * answer computed in one place rather than three clients each decoding a
 * bitmask slightly differently.
 */
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue';
import { useI18n } from 'vue-i18n';

import { ApiError, apiDelete, apiGet, apiPatch, apiPost } from '@/api/client';
import {
  AppButton,
  ConfirmDialog,
  EmptyState,
  InlineAlert,
  LoadingSkeleton,
  PageHeader,
  StatusBadge,
} from '@/components/ui';
import type { StatusTone } from '@/components/ui/types';

interface Invite {
  id: string;
  label: string;
  token: string;
  pin: string;
  path: string;
  perm: number;
  permission_summary: string;
  app_id: number;
  allow_browser: boolean;
  allow_pairing: boolean;
  revoked: boolean;
  created_at: number;
  expires_at: number;
  max_uses: number;
  uses: number;
  live: boolean;
  locked_for_seconds: number;
  active_sessions: number;
}

const { t } = useI18n();

const invites = ref<Invite[]>([]);
// Set from /api/invites — the `public_base_url` setting, or '' when unset.
const linkBase = ref('');
const loading = ref(false);
// True only until the first load() finishes. Background refreshes keep the
// existing list on screen instead of swapping it for a skeleton — that swap
// is what makes the page look like it is reloading every few seconds.
const initialLoad = ref(true);
const error = ref('');
const notice = ref('');
const busyId = ref('');

const showCreate = ref(false);
const creating = ref(false);
const confirmDelete = ref<Invite | null>(null);

/// The create form remembers its last shape, per browser.
///
/// Everything here except the label is a policy decision the owner makes the same
/// way most times — a 24h gamepad-only browser link, say — and resetting it on
/// every reload meant re-picking all five, with no way to see what the last invite
/// was cut from while adjusting. The label is deliberately not persisted: it names
/// one guest, so carrying it forward would mislabel the next invite.
const DRAFT_KEY = 'vibepollo.invites.draft';

interface InviteDraft {
  label: string;
  preset: 'view' | 'gamepad' | 'full';
  allow_browser: boolean;
  allow_pairing: boolean;
  expires_in_hours: number;
  max_uses: number;
}

function defaultDraft(): InviteDraft {
  return {
    label: '',
    preset: 'gamepad',
    allow_browser: true,
    allow_pairing: false,
    expires_in_hours: 24,
    max_uses: 0,
  };
}

function loadDraft(): InviteDraft {
  const fallback = defaultDraft();
  try {
    const raw = window.localStorage.getItem(DRAFT_KEY);
    if (!raw) return fallback;
    const saved = JSON.parse(raw) as Partial<InviteDraft>;
    return {
      label: fallback.label,
      preset:
        saved.preset === 'view' || saved.preset === 'gamepad' || saved.preset === 'full'
          ? saved.preset
          : fallback.preset,
      allow_browser:
        typeof saved.allow_browser === 'boolean' ? saved.allow_browser : fallback.allow_browser,
      allow_pairing:
        typeof saved.allow_pairing === 'boolean' ? saved.allow_pairing : fallback.allow_pairing,
      // 0 would mean an invite that expires instantly; a negative one is nonsense.
      expires_in_hours:
        Number.isFinite(Number(saved.expires_in_hours)) && Number(saved.expires_in_hours) > 0
          ? Number(saved.expires_in_hours)
          : fallback.expires_in_hours,
      // 0 is meaningful here — it is "unlimited uses" — so only reject negatives.
      max_uses:
        Number.isFinite(Number(saved.max_uses)) && Number(saved.max_uses) >= 0
          ? Number(saved.max_uses)
          : fallback.max_uses,
    };
  } catch {
    return fallback;
  }
}

const draft = ref<InviteDraft>(loadDraft());

watch(
  () => [
    draft.value.preset,
    draft.value.allow_browser,
    draft.value.allow_pairing,
    draft.value.expires_in_hours,
    draft.value.max_uses,
  ],
  () => {
    try {
      window.localStorage.setItem(
        DRAFT_KEY,
        JSON.stringify({ ...draft.value, label: '' } satisfies InviteDraft),
      );
    } catch {
      // Never let a storage failure block creating an invite.
    }
  },
);

/// The API returns a relative path, so the origin is chosen here. Prefer the
/// configured public base URL: the owner is looking at this page on the LAN, so
/// `window.location.origin` is a private address the guest cannot open. Falling
/// back to it keeps a LAN-only setup working with nothing to configure.
function fullLink(invite: Invite): string {
  return (linkBase.value || window.location.origin) + invite.path;
}

function statusOf(invite: Invite): { label: string; tone: StatusTone } {
  // Ranked above everything else on purpose: "someone is connected right now" is
  // the fact the owner most needs off this page, and it stays true even for an
  // invite that has since been revoked or run out of uses.
  if (invite.active_sessions > 0) {
    return {
      label: t('ui.invites.status_connected', { count: invite.active_sessions }),
      tone: 'info',
    };
  }
  if (invite.revoked) return { label: t('ui.invites.status_revoked'), tone: 'danger' };
  if (!invite.live) return { label: t('ui.invites.status_spent'), tone: 'neutral' };
  if (invite.locked_for_seconds > 0)
    return { label: t('ui.invites.status_locked'), tone: 'warning' };
  return { label: t('ui.invites.status_live'), tone: 'success' };
}

function expiryText(invite: Invite): string {
  if (!invite.expires_at) return t('ui.invites.never_expires');
  const when = new Date(invite.expires_at * 1000);
  return when < new Date()
    ? t('ui.invites.expired_on', { when: when.toLocaleString() })
    : t('ui.invites.expires_on', { when: when.toLocaleString() });
}

function usesText(invite: Invite): string {
  return invite.max_uses > 0
    ? t('ui.invites.uses_limited', { used: invite.uses, max: invite.max_uses })
    : t('ui.invites.uses_unlimited', { used: invite.uses });
}

const hasInvites = computed(() => invites.value.length > 0);

function describe(err: unknown): string {
  if (err instanceof ApiError && err.message) return err.message;
  return err instanceof Error ? err.message : String(err);
}

async function load(): Promise<void> {
  loading.value = true;
  error.value = '';
  try {
    const response = await apiGet<{ invites?: Invite[]; link_base?: string }>('/api/invites');
    const next = Array.isArray(response.invites) ? response.invites : [];
    const nextBase = (response.link_base ?? '').replace(/\/+$/, '');
    // Skip the array replacement when nothing changed. A new ref on every poll
    // forces every card to re-render even when the response is byte-identical,
    // which the user sees as the list "flashing".
    if (!sameInvites(invites.value, next)) {
      invites.value = next;
    }
    if (linkBase.value !== nextBase) {
      linkBase.value = nextBase;
    }
  } catch (err) {
    error.value = describe(err);
  } finally {
    loading.value = false;
    initialLoad.value = false;
  }
}

function sameInvites(a: Invite[], b: Invite[]): boolean {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) {
    const x = a[i];
    const y = b[i];
    // Order is stable from the API; the only fields that change while the page
    // is open are active_sessions, locked_for_seconds, uses, and revoked. A
    // mismatch on any of those means the card needs to re-render.
    if (
      x.id !== y.id ||
      x.active_sessions !== y.active_sessions ||
      x.locked_for_seconds !== y.locked_for_seconds ||
      x.uses !== y.uses ||
      x.revoked !== y.revoked
    ) {
      return false;
    }
  }
  return true;
}

async function create(): Promise<void> {
  creating.value = true;
  error.value = '';
  try {
    const created = await apiPost<Invite>('/api/invites', {
      label: draft.value.label.trim() || t('ui.invites.default_label'),
      preset: draft.value.preset,
      allow_browser: draft.value.allow_browser,
      allow_pairing: draft.value.allow_pairing,
      max_uses: draft.value.max_uses,
      expires_in_seconds: Math.round(draft.value.expires_in_hours * 3600),
    });
    invites.value = [created, ...invites.value];
    showCreate.value = false;
    draft.value.label = '';
    // Copying immediately is the whole point of the page — the owner made this
    // link to send it to someone, so put it on the clipboard without a second click.
    await copy(fullLink(created), t('ui.invites.copied_link'));
  } catch (err) {
    error.value = describe(err);
  } finally {
    creating.value = false;
  }
}

async function patch(invite: Invite, body: Record<string, unknown>): Promise<void> {
  busyId.value = invite.id;
  error.value = '';
  try {
    const updated = await apiPatch<Invite>(`/api/invites/${encodeURIComponent(invite.id)}`, body);
    invites.value = invites.value.map((i) => (i.id === updated.id ? updated : i));
  } catch (err) {
    error.value = describe(err);
  } finally {
    busyId.value = '';
  }
}

async function rotate(invite: Invite): Promise<void> {
  busyId.value = invite.id;
  error.value = '';
  try {
    const updated = await apiPost<Invite>(
      `/api/invites/${encodeURIComponent(invite.id)}/rotate`,
      {},
    );
    invites.value = invites.value.map((i) => (i.id === updated.id ? updated : i));
    notice.value = t('ui.invites.rotated');
  } catch (err) {
    error.value = describe(err);
  } finally {
    busyId.value = '';
  }
}

async function remove(invite: Invite): Promise<void> {
  busyId.value = invite.id;
  error.value = '';
  try {
    await apiDelete(`/api/invites/${encodeURIComponent(invite.id)}`);
    invites.value = invites.value.filter((i) => i.id !== invite.id);
  } catch (err) {
    error.value = describe(err);
  } finally {
    busyId.value = '';
    confirmDelete.value = null;
  }
}

async function copy(text: string, message: string): Promise<void> {
  try {
    await navigator.clipboard.writeText(text);
    notice.value = message;
  } catch {
    // Clipboard access is refused in plenty of ordinary situations (an insecure
    // origin, a denied permission). Say so rather than silently doing nothing,
    // since the value is selectable on the page anyway.
    notice.value = t('ui.invites.copy_failed');
  }
}

let refresh: ReturnType<typeof setInterval> | undefined;

onMounted(() => {
  load();
  // Someone joining or leaving is not something the owner triggers, so the page
  // has to ask. Five seconds is fast enough to feel live and slow enough to be
  // free.
  refresh = setInterval(load, 5000);
});

onBeforeUnmount(() => {
  if (refresh) clearInterval(refresh);
});
</script>

<template>
  <section class="vs-page invites">
    <PageHeader :title="t('ui.invites.title')" :description="t('ui.invites.description')">
      <template #actions>
        <AppButton
          icon="plus"
          :label="t('ui.invites.new')"
          variant="primary"
          @click="showCreate = !showCreate"
        />
      </template>
    </PageHeader>

    <InlineAlert v-if="error" tone="danger" :title="t('ui.invites.error')">{{ error }}</InlineAlert>
    <InlineAlert v-if="notice" tone="success" announce="polite" @dismiss="notice = ''">{{
      notice
    }}</InlineAlert>

    <form v-if="showCreate" class="create" @submit.prevent="create">
      <div class="create__grid">
        <label class="field">
          <span>{{ t('ui.invites.field_label') }}</span>
          <input v-model="draft.label" type="text" :placeholder="t('ui.invites.label_hint')" />
        </label>

        <label class="field">
          <span>{{ t('ui.invites.field_access') }}</span>
          <select v-model="draft.preset">
            <option value="view">{{ t('ui.invites.preset_view') }}</option>
            <option value="gamepad">{{ t('ui.invites.preset_gamepad') }}</option>
            <option value="full">{{ t('ui.invites.preset_full') }}</option>
          </select>
        </label>

        <label class="field">
          <span>{{ t('ui.invites.field_expiry') }}</span>
          <input v-model.number="draft.expires_in_hours" type="number" min="0" step="1" />
        </label>

        <label class="field">
          <span>{{ t('ui.invites.field_uses') }}</span>
          <input v-model.number="draft.max_uses" type="number" min="0" step="1" />
        </label>
      </div>

      <div class="create__toggles">
        <label
          ><input v-model="draft.allow_browser" type="checkbox" />
          {{ t('ui.invites.allow_browser') }}</label
        >
        <label
          ><input v-model="draft.allow_pairing" type="checkbox" />
          {{ t('ui.invites.allow_pairing') }}</label
        >
      </div>

      <p class="create__note">{{ t('ui.invites.pin_note') }}</p>

      <AppButton
        type="submit"
        variant="primary"
        :label="t('ui.invites.create')"
        :busy="creating"
        :busy-label="t('ui.invites.creating')"
      />
    </form>

    <LoadingSkeleton v-if="loading && initialLoad" />

    <EmptyState
      v-else-if="!hasInvites"
      icon="gamepad"
      :title="t('ui.invites.empty_title')"
      :description="t('ui.invites.empty_description')"
    />

    <ul v-else class="list">
      <li
        v-for="invite in invites"
        :key="invite.id"
        class="card"
        :class="{ 'card--connected': invite.active_sessions > 0 }"
      >
        <header class="card__head">
          <div>
            <h3>{{ invite.label }}</h3>
            <p class="card__grant">{{ invite.permission_summary }}</p>
          </div>
          <StatusBadge :label="statusOf(invite).label" :tone="statusOf(invite).tone" />
        </header>

        <div class="secret">
          <span class="secret__name">{{ t('ui.invites.link') }}</span>
          <code class="secret__value">{{ fullLink(invite) }}</code>
          <AppButton
            icon="copy"
            icon-only
            size="compact"
            :aria-label="t('ui.invites.copy_link')"
            @click="copy(fullLink(invite), t('ui.invites.copied_link'))"
          />
        </div>

        <div class="secret">
          <span class="secret__name">{{ t('ui.invites.pin') }}</span>
          <code class="secret__value secret__value--pin">{{ invite.pin }}</code>
          <AppButton
            icon="copy"
            icon-only
            size="compact"
            :aria-label="t('ui.invites.copy_pin')"
            @click="copy(invite.pin, t('ui.invites.copied_pin'))"
          />
        </div>

        <p class="card__meta">
          {{ expiryText(invite) }} · {{ usesText(invite) }}
        </p>

        <p class="card__ways">
          <span v-if="invite.allow_browser">{{ t('ui.invites.way_browser') }}</span>
          <span v-if="invite.allow_pairing">{{ t('ui.invites.way_pairing') }}</span>
          <span v-if="!invite.allow_browser && !invite.allow_pairing" class="card__ways--none">{{
            t('ui.invites.way_none')
          }}</span>
        </p>

        <footer class="card__actions">
          <AppButton
            icon="refresh"
            size="compact"
            :label="t('ui.invites.rotate')"
            :disabled="busyId === invite.id"
            @click="rotate(invite)"
          />
          <AppButton
            v-if="!invite.revoked"
            size="compact"
            :label="t('ui.invites.revoke')"
            :disabled="busyId === invite.id"
            @click="patch(invite, { revoked: true })"
          />
          <AppButton
            v-else
            size="compact"
            :label="t('ui.invites.restore')"
            :disabled="busyId === invite.id"
            @click="patch(invite, { revoked: false })"
          />
          <AppButton
            icon="trash"
            size="compact"
            variant="tertiary"
            :label="t('ui.invites.delete')"
            :disabled="busyId === invite.id"
            @click="confirmDelete = invite"
          />
        </footer>
      </li>
    </ul>

    <ConfirmDialog
      :open="confirmDelete !== null"
      tone="danger"
      :title="t('ui.invites.delete_title')"
      :description="t('ui.invites.delete_description')"
      :confirm-label="t('ui.invites.delete')"
      @confirm="confirmDelete && remove(confirmDelete)"
      @cancel="confirmDelete = null"
    />
  </section>
</template>

<style scoped>
.invites {
  display: flex;
  flex-direction: column;
  gap: 16px;
}

.create {
  display: flex;
  flex-direction: column;
  gap: 14px;
  padding: 18px;
  border: 1px solid var(--vs-color-border-subtle);
  border-radius: 12px;
  background: var(--vs-color-bg-surface);
}

.create__grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(180px, 1fr));
  gap: 12px;
}

.field {
  display: flex;
  flex-direction: column;
  gap: 6px;
  font-size: 13px;
}

.field input,
.field select {
  padding: 9px 10px;
  border: 1px solid var(--vs-color-border-subtle);
  border-radius: 8px;
  background: var(--vs-color-bg-subtle);
  color: inherit;
}

.create__toggles {
  display: flex;
  flex-wrap: wrap;
  gap: 18px;
  font-size: 14px;
}

.create__note {
  margin: 0;
  font-size: 13px;
  opacity: 0.7;
}

.list {
  display: grid;
  gap: 12px;
  margin: 0;
  padding: 0;
  list-style: none;
}

.card {
  display: flex;
  flex-direction: column;
  gap: 10px;
  padding: 16px;
  border: 1px solid var(--vs-color-border-subtle);
  border-radius: 12px;
  background: var(--vs-color-bg-surface);
}

.card--connected {
  border-color: var(--vs-color-accent-default);
  box-shadow: inset 3px 0 0 var(--vs-color-accent-default);
}

.card__head {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: 12px;
}

.card__head h3 {
  margin: 0;
  font-size: 16px;
}

.card__grant {
  margin: 2px 0 0;
  font-size: 13px;
  opacity: 0.7;
}

.secret {
  display: flex;
  align-items: center;
  gap: 8px;
}

.secret__name {
  min-width: 44px;
  font-size: 12px;
  text-transform: uppercase;
  letter-spacing: 0.06em;
  opacity: 0.6;
}

.secret__value {
  flex: 1;
  min-width: 0;
  padding: 7px 10px;
  border-radius: 7px;
  background: var(--vs-color-bg-subtle);
  font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
  font-size: 13px;
  overflow-x: auto;
  white-space: nowrap;
}

.secret__value--pin {
  flex: 0 0 auto;
  letter-spacing: 0.3em;
  font-size: 15px;
}

.card__meta,
.card__ways {
  margin: 0;
  font-size: 13px;
  opacity: 0.7;
}

.card__ways span + span::before {
  content: ' · ';
}

.card__ways--none {
  opacity: 0.8;
}

.card__actions {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  margin-top: 2px;
}
</style>
