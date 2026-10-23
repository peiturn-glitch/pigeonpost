<script setup>
import { computed, onBeforeUnmount, onMounted, reactive, ref } from 'vue'
import { api } from '../api'
import { useBulkAddresses } from '../bulkAddresses'
import { t } from '../i18n'
import { useMailStore } from '../store'
import { defaultPorts, mailPresets, presetForEmail } from '../utils'
import BulkAddresses from './BulkAddresses.vue'
import BulkStatus from './BulkStatus.vue'
import Icon from './Icon.vue'
import SignatureEditor from './SignatureEditor.vue'

const store = useMailStore()
const { account, identity } = store.identityDialog
const isNew = !identity

const form = reactive({
  id: identity?.id ?? null,
  name: identity?.name ?? account.name,
  email: identity?.email ?? '',
  replyTo: identity?.replyTo ?? '',
  signature: identity?.signature ?? '',
  signatureHtml: identity?.signatureHtml ?? '',
  useAccountSmtp: identity?.useAccountSmtp ?? true,
  smtpHost: identity?.smtpHost ?? '',
  smtpPort: identity?.smtpPort ?? 587,
  smtpSecurity: identity?.smtpSecurity ?? 'starttls',
  smtpUsername: identity?.smtpUsername ?? '',
  smtpPassword: '',
  tlsVerify: identity?.tlsVerify ?? true,
})

const hasStoredPassword = !!identity?.hasSmtpPassword
const usernameTouched = ref(!isNew)
const busy = ref(false)
const error = ref('')
const testResult = ref(null)

const needsPassword = computed(() => !!form.smtpUsername && !form.smtpPassword && !hasStoredPassword)

// Flera avsändaradresser med samma inställningar, en avsändare per adress.
const {
  state: bulk,
  parsed: bulkParsed,
  pending: bulkPending,
  failures: bulkFailures,
  reset: resetBulk,
  run: runBulk,
} = useBulkAddresses({
  // Kontots egen adress och de avsändare som redan finns.
  existing: () => [account.email, ...(store.account(account.id)?.identities || []).map((i) => i.email)],
  create: (entry, skipTest) => api.createIdentity(account.id, bulkBody(entry, skipTest)),
})

// Med egen SMTP-server behöver varje avsändare ett lösenord om den loggar in.
const bulkNeedsPassword = computed(
  () => !form.useAccountSmtp && (bulk.usernameMode === 'address' || !!form.smtpUsername) && !form.smtpPassword,
)

const canSubmit = computed(() => {
  if (bulk.enabled) {
    if (!bulkPending.value.length) return false
    return form.useAccountSmtp || (!!form.smtpHost && !bulkNeedsPassword.value)
  }
  return form.email.includes('@') && (form.useAccountSmtp || (form.smtpHost && !needsPassword.value))
})

function onBulkToggle() {
  resetBulk()
  testResult.value = null
}

function guessFromBulk() {
  const first = bulkParsed.value.addresses[0]
  if (!first || form.useAccountSmtp || form.smtpHost) return
  const preset = presetForEmail(first.email)
  if (preset) {
    Object.assign(form, { smtpHost: preset.smtpHost, smtpPort: preset.smtpPort ?? 587, smtpSecurity: preset.smtpSecurity ?? 'starttls' })
  }
}

function bulkBody(entry, skipTest) {
  const ownSmtpUsername = bulk.usernameMode === 'address' ? entry.email : form.smtpUsername
  return {
    name: entry.name || form.name,
    email: entry.email,
    replyTo: form.replyTo,
    signature: form.signature,
    signatureHtml: form.signatureHtml,
    useAccountSmtp: form.useAccountSmtp,
    smtpHost: form.smtpHost,
    smtpPort: Number(form.smtpPort),
    smtpSecurity: form.smtpSecurity,
    smtpUsername: form.useAccountSmtp ? '' : ownSmtpUsername,
    smtpPassword: form.smtpPassword,
    tlsVerify: form.tlsVerify,
    skipTest: skipTest || !bulk.test,
  }
}

async function addIdentities(options) {
  busy.value = true
  error.value = ''
  testResult.value = null
  let created = []
  try {
    created = await runBulk(options)
  } finally {
    busy.value = false
  }
  if (!created.length) return
  await store.loadAccounts()
  store.notify(created.length === 1 ? t('Sender added') : t('{n} senders were added', { n: created.length }))
  if (!bulkFailures.value.length) close()
}

function close() {
  store.identityDialog = null
}

function onEmailBlur() {
  if (form.useAccountSmtp) return
  if (!usernameTouched.value) form.smtpUsername = form.email
  if (!form.smtpHost) {
    const preset = presetForEmail(form.email)
    if (preset) Object.assign(form, { smtpHost: preset.smtpHost, smtpPort: preset.smtpPort ?? 587, smtpSecurity: preset.smtpSecurity ?? 'starttls' })
  }
}

function onCustomSmtp() {
  // Förifyll med kontots server som utgångspunkt när man byter till egen server.
  if (!form.useAccountSmtp && !form.smtpHost) {
    const preset = presetForEmail(form.email)
    if (form.email && preset) {
      Object.assign(form, { smtpHost: preset.smtpHost, smtpPort: preset.smtpPort ?? 587, smtpSecurity: preset.smtpSecurity ?? 'starttls' })
    } else {
      Object.assign(form, { smtpHost: account.smtpHost, smtpPort: account.smtpPort, smtpSecurity: account.smtpSecurity })
    }
    if (!usernameTouched.value) form.smtpUsername = form.email
  }
}

function applyPreset(event) {
  const preset = mailPresets[event.target.value]
  if (preset) Object.assign(form, { smtpHost: preset.smtpHost, smtpPort: preset.smtpPort, smtpSecurity: preset.smtpSecurity })
  if (event.target.value === 'greenmail') form.tlsVerify = false
  event.target.value = ''
}

function body(skipTest = false) {
  return { ...form, smtpPort: Number(form.smtpPort), skipTest }
}

async function test() {
  busy.value = true
  error.value = ''
  testResult.value = null
  try {
    // I flerläget testas den första adressen, eftersom alla avsändare har samma server.
    const first = bulkParsed.value.addresses[0]
    testResult.value = await api.testIdentity(account.id, bulk.enabled ? bulkBody(first, true) : body())
  } catch (e) {
    error.value = e.message
  } finally {
    busy.value = false
  }
}

async function save(skipTest = false) {
  busy.value = true
  error.value = ''
  try {
    if (isNew) await api.createIdentity(account.id, body(skipTest))
    else await api.updateIdentity(account.id, form.id, body(skipTest))
    await store.loadAccounts()
    store.notify(isNew ? t('Sender added') : t('Sender saved'))
    close()
  } catch (e) {
    error.value = e.message
    if (e.data?.test) testResult.value = e.data.test
  } finally {
    busy.value = false
  }
}

function onKey(event) {
  if (event.key === 'Escape') close()
}
onMounted(() => window.addEventListener('keydown', onKey))
onBeforeUnmount(() => window.removeEventListener('keydown', onKey))
</script>

<template>
  <div class="modal-backdrop">
    <form class="modal identity" @submit.prevent="bulk.enabled ? addIdentities() : save(false)">
      <div class="modal-header">
        <h2>{{ isNew ? t('New sender') : t('Edit sender') }} · {{ account.email }}</h2>
        <button type="button" class="btn ghost" :title="t('Close')" @click="close"><Icon name="close" /></button>
      </div>

      <div class="content">
        <label v-if="isNew" class="check bulk-toggle">
          <input v-model="bulk.enabled" type="checkbox" @change="onBulkToggle" />
          <span>
            {{ t('Several addresses with the same settings') }}
            <span class="muted">{{ t('– each address becomes its own sender') }}</span>
          </span>
        </label>

        <section class="grid">
          <label for="id-name">{{ t('Name') }}</label>
          <input id="id-name" v-model="form.name" class="input" :placeholder="t('First and last name')" />
          <template v-if="bulk.enabled">
            <label for="id-emails">{{ t('Email addresses') }}</label>
            <BulkAddresses :state="bulk" :parsed="bulkParsed" input-id="id-emails" @blur="guessFromBulk">
              <template #hint>{{ t('The name above is used for addresses without a name of their own.') }}</template>
            </BulkAddresses>
          </template>
          <template v-else>
            <label for="id-email">{{ t('Email address') }}</label>
            <input id="id-email" v-model="form.email" class="input" type="email" required @blur="onEmailBlur" />
          </template>
          <label for="id-replyto">{{ t('Reply-to address') }}</label>
          <input id="id-replyto" v-model="form.replyTo" class="input" type="email" :placeholder="t('Optional, if replies should go to another address')" />
          <label for="id-sig">{{ t('Signature') }}</label>
          <SignatureEditor v-model:text="form.signature" v-model:html="form.signatureHtml" input-id="id-sig" />
        </section>

        <section>
          <h3>{{ t('Outgoing server') }}</h3>
          <label class="radio">
            <input v-model="form.useAccountSmtp" type="radio" :value="true" />
            {{ t('Use the account server ({host}:{port})', { host: account.smtpHost, port: account.smtpPort }) }}
          </label>
          <label class="radio">
            <input v-model="form.useAccountSmtp" type="radio" :value="false" @change="onCustomSmtp" />
            {{ t('Own SMTP server') }}
          </label>
          <p v-if="form.useAccountSmtp" class="muted small">
            {{ t('Many servers only allow sender addresses that belong to the account. Choose your own SMTP server if the address belongs to another service.') }}
          </p>

          <div v-if="!form.useAccountSmtp" class="smtp">
            <div class="servers">
              <span class="muted">{{ t('Server') }}</span>
              <span class="muted">{{ t('Port') }}</span>
              <span class="muted">{{ t('Security') }}</span>
              <input v-model="form.smtpHost" class="input" :placeholder="t('smtp.example.com')" required />
              <input v-model="form.smtpPort" class="input" type="number" min="1" max="65535" required />
              <select v-model="form.smtpSecurity" class="input" @change="form.smtpPort = defaultPorts.smtp[form.smtpSecurity]">
                <option value="tls">SSL/TLS</option>
                <option value="starttls">STARTTLS</option>
                <option value="none">{{ t('None') }}</option>
              </select>
            </div>
            <div class="grid">
              <template v-if="bulk.enabled">
                <span class="label">{{ t('Username') }}</span>
                <div class="username-choice">
                  <label class="radio">
                    <input v-model="bulk.usernameMode" type="radio" value="address" />
                    {{ t('The address is the username for each sender') }}
                  </label>
                  <label class="radio">
                    <input v-model="bulk.usernameMode" type="radio" value="same" />
                    {{ t('Same username for all') }}
                  </label>
                  <input
                    v-if="bulk.usernameMode === 'same'"
                    v-model="form.smtpUsername"
                    class="input"
                    :placeholder="t('Empty = no sign-in')"
                    autocomplete="off"
                    @input="usernameTouched = true"
                  />
                </div>
              </template>
              <template v-else>
                <label for="id-user">{{ t('Username') }}</label>
                <input id="id-user" v-model="form.smtpUsername" class="input" :placeholder="t('Empty = no sign-in')" autocomplete="off" @input="usernameTouched = true" />
              </template>
              <label for="id-pass">{{ t('Password') }}</label>
              <input
                id="id-pass"
                v-model="form.smtpPassword"
                class="input"
                type="password"
                autocomplete="new-password"
                :placeholder="hasStoredPassword ? t('Unchanged') : ''"
                :disabled="!bulk.enabled && !form.smtpUsername"
              />
            </div>
            <div class="row">
              <label class="check">
                <input v-model="form.tlsVerify" type="checkbox" />
                {{ t('Verify server certificate') }}
              </label>
              <select class="input preset" @change="applyPreset">
                <option value="">{{ t('Preset…') }}</option>
                <option value="gmail">Gmail</option>
                <option value="outlook">Outlook / Microsoft 365</option>
                <option value="icloud">iCloud</option>
                <option value="yahoo">Yahoo</option>
                <option value="greenmail">{{ t('Test server') }}</option>
              </select>
            </div>
          </div>
        </section>

        <template v-if="bulk.enabled">
          <label v-if="!form.useAccountSmtp" class="check">
            <input v-model="bulk.test" type="checkbox" />
            {{ t('Test the SMTP sign-in for each address') }}
          </label>
          <BulkStatus :entries="bulk.entries" :exists-text="t('Already exists as a sender')" />
        </template>

        <p v-if="testResult" :class="testResult.ok ? 'ok' : 'error-text'">
          SMTP: {{ testResult.ok ? t('connection works') : testResult.error }}
        </p>
        <p v-if="error" class="error-text">{{ error }}</p>
      </div>

      <div class="modal-footer">
        <button type="button" class="btn" :disabled="busy || !canSubmit || form.useAccountSmtp" @click="test">
          {{ bulk.enabled ? t('Test first address') : t('Test SMTP') }}
        </button>
        <template v-if="bulk.enabled">
          <button
            v-if="bulkFailures.some((e) => e.canSkipTest)"
            type="button"
            class="btn"
            :disabled="busy"
            @click="addIdentities({ skipTest: true, onlyFailed: true })"
          >
            {{ t('Add the failed ones anyway') }}
          </button>
          <button type="submit" class="btn primary" :disabled="busy || !canSubmit">
            {{ busy ? t('Adding…') : bulkPending.length === 1 ? t('Add 1 sender') : t('Add {n} senders', { n: bulkPending.length }) }}
          </button>
        </template>
        <template v-else>
          <button v-if="testResult && !testResult.ok" type="button" class="btn" :disabled="busy" @click="save(true)">{{ t('Save anyway') }}</button>
          <button type="submit" class="btn primary" :disabled="busy || !canSubmit">
            {{ busy ? t('Checking…') : isNew ? t('Add') : t('Save') }}
          </button>
        </template>
      </div>
    </form>
  </div>
</template>

<style scoped>
.modal-backdrop {
  z-index: 55;
}

.identity {
  max-width: 600px;
}

.bulk-toggle {
  display: flex;
  gap: 8px;
  align-items: flex-start;
  font-size: 13px;
  margin: 10px 0 0;
}

.username-choice {
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.content {
  overflow-y: auto;
  padding: 8px 16px 16px;
}

section {
  padding: 10px 0;
  border-bottom: 1px solid var(--border);
}

section:last-of-type {
  border-bottom: 0;
}

h3 {
  margin: 0 0 8px;
  font-size: 13px;
}

.grid {
  display: grid;
  grid-template-columns: 120px minmax(0, 1fr);
  gap: 8px 12px;
  align-items: center;
}

.grid label,
.grid .label {
  text-align: right;
  color: var(--text-muted);
}

.grid .label {
  align-self: start;
  padding-top: 4px;
}

.radio,
.check {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 3px 0;
}

.small {
  font-size: 12px;
  margin: 4px 0 0;
}

.smtp {
  margin-top: 10px;
  display: flex;
  flex-direction: column;
  gap: 10px;
}

.servers {
  display: grid;
  grid-template-columns: minmax(0, 1fr) 90px 120px;
  gap: 4px 8px;
}

.row {
  display: flex;
  justify-content: space-between;
  align-items: center;
  gap: 12px;
}

.preset {
  width: auto;
}

.ok {
  color: var(--success);
}

@media (max-width: 600px) {
  .grid,
  .servers {
    grid-template-columns: 1fr;
  }
  .grid label {
    text-align: left;
  }
}
</style>
