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
import { computed, onMounted, ref } from 'vue';
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
  gamepad_base_slot: number;
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
}

const { t } = useI18n();

const invites = ref<Invite[]>([]);
const loading = ref(false);
const error = ref('');
const notice = ref('');
const busyId = ref('');

const showCreate = ref(false);
const creating = ref(false);
const confirmDelete = ref<Invite | null>(null);

const draft = ref({
  label: '',
  preset: 'gamepad' as 'view' | 'gamepad' | 'full',
  allow_browser: true,
  allow_pairing: false,
  gamepad_base_slot: 1,
  expires_in_hours: 24,
  max_uses: 0,
});

/// The host cannot know the origin it is reached on — behind a reverse proxy it
/// may be a name it has never heard of — so the API returns a relative path and
/// the full link is assembled here, in the one place that does know.
function fullLink(invite: Invite): string {
  return window.location.origin + invite.path;
}

function statusOf(invite: Invite): { label: string; tone: StatusTone } {
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
    const response = await apiGet<{ invites?: Invite[] }>('/api/invites');
    invites.value = Array.isArray(response.invites) ? response.invites : [];
  } catch (err) {
    error.value = describe(err);
  } finally {
    loading.value = false;
  }
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
      gamepad_base_slot: draft.value.gamepad_base_slot,
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

onMounted(load);
</script>

<template>
  <section class="invites">
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
          <span>{{ t('ui.invites.field_slot') }}</span>
          <input v-model.number="draft.gamepad_base_slot" type="number" min="0" max="15" />
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

    <LoadingSkeleton v-if="loading" />

    <EmptyState
      v-else-if="!hasInvites"
      icon="gamepad"
      :title="t('ui.invites.empty_title')"
      :description="t('ui.invites.empty_description')"
    />

    <ul v-else class="list">
      <li v-for="invite in invites" :key="invite.id" class="card">
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
          {{ expiryText(invite) }} · {{ usesText(invite) }} ·
          {{ t('ui.invites.slot', { slot: invite.gamepad_base_slot }) }}
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
  border: 1px solid var(--color-border, #2c313a);
  border-radius: 12px;
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
  border: 1px solid var(--color-border, #2c313a);
  border-radius: 8px;
  background: var(--color-surface, #0f1114);
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
  border: 1px solid var(--color-border, #2c313a);
  border-radius: 12px;
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
  background: var(--color-surface, #0f1114);
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
