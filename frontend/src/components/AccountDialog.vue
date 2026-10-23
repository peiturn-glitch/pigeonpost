<script setup>
import { computed, onBeforeUnmount, onMounted, reactive, ref } from 'vue'
import { api } from '../api'
import { useBulkAddresses } from '../bulkAddresses'
import { t } from '../i18n'
import { isGoogleMailAddress, isGoogleMailHost } from '../oauth'
import { useMailStore } from '../store'
import { defaultPorts, mailPresets as presets, presetForEmail } from '../utils'
import BulkAddresses from './BulkAddresses.vue'
import BulkStatus from './BulkStatus.vue'
import GoogleSignIn from './GoogleSignIn.vue'
import Icon from './Icon.vue'
import SignatureEditor from './SignatureEditor.vue'

const store = useMailStore()
const existing = store.accountDialog
const isNew = !existing.id
const wasGoogle = existing.authType === 'oauth-google'

// Extra avsändare läses från storen så att listan uppdateras när IdentityDialog sparar.
const identities = computed(() => (isNew ? [] : store.account(existing.id)?.identities || []))

function editIdentity(identity = null) {
  store.identityDialog = { account: store.account(existing.id), identity }
}

async function removeIdentity(identity) {
  if (!window.confirm(t('Remove the sender {email}?', { email: identity.email }))) return
  try {
    await api.deleteIdentity(existing.id, identity.id)
    await store.loadAccounts()
  } catch (e) {
    error.value = e.message
  }
}

const form = reactive({
  id: existing.id ?? null,
  name: existing.name ?? '',
  email: existing.email ?? '',
  username: existing.username ?? '',
  password: '',
  imapHost: existing.imapHost ?? '',
  imapPort: existing.imapPort ?? 993,
  imapSecurity: existing.imapSecurity ?? 'tls',
  smtpHost: existing.smtpHost ?? '',
  smtpPort: existing.smtpPort ?? 465,
  smtpSecurity: existing.smtpSecurity ?? 'tls',
  smtpUsername: existing.smtpUsername ?? '',
  tlsVerify: existing.tlsVerify ?? true,
  signature: existing.signature ?? '',
  signatureHtml: existing.signatureHtml ?? '',
  authType: existing.authType ?? 'password',
})

const busy = ref(false)
const error = ref('')
const testResult = ref(null)

// Google-inloggning: state från en avslutad inloggning skickas med när kontot sparas.
const oauthState = ref('')
const googleEmail = ref(wasGoogle ? existing.username : '')
const googleConfigured = computed(() => store.oauthProviders.google?.configured)
const isGmail = computed(() => isGoogleMailHost(form.imapHost) || isGoogleMailAddress(form.email))
const useGoogle = computed(() => form.authType === 'oauth-google')

function onGoogleSignedIn(result) {
  oauthState.value = result.state
  googleEmail.value = result.email
  form.username = result.email
  if (!form.email) form.email = result.email
  if (!form.name) form.name = result.name
  if (!form.imapHost) Object.assign(form, presets.gmail)
  testResult.value = null
  error.value = ''
}

function onKey(event) {
  // Escape stänger först en öppen avsändardialog ovanpå denna.
  if (event.key === 'Escape' && !store.identityDialog) store.accountDialog = null
}
onMounted(() => window.addEventListener('keydown', onKey))
onBeforeUnmount(() => window.removeEventListener('keydown', onKey))
const usernameTouched = ref(!isNew)

function guessFromEmail() {
  if (!usernameTouched.value && !useGoogle.value) form.username = form.email
  if (!isNew || form.imapHost) return
  const preset = presetForEmail(form.email)
  if (preset) Object.assign(form, preset)
  // Föreslå Google-inloggning för Gmail när servern har den aktiverad.
  if (isGmail.value && googleConfigured.value) form.authType = 'oauth-google'
}

function onSecurityChange(kind) {
  const key = `${kind}Security`
  form[`${kind}Port`] = defaultPorts[kind][form[key]]
}

function applyPreset(event) {
  const preset = presets[event.target.value]
  if (preset) Object.assign(form, preset)
  if (event.target.value === 'greenmail') form.tlsVerify = false
  event.target.value = ''
}

// Flera adresser med samma serverinställningar och lösenord, ett konto per adress.
const {
  state: bulk,
  parsed: bulkParsed,
  pending: bulkPending,
  failures: bulkFailures,
  reset: resetBulk,
  run: runBulk,
} = useBulkAddresses({
  existing: () => store.accounts.map((a) => a.email),
  create: (entry, skipTest) => api.createAccount(bulkBody(entry, skipTest)),
})

function onBulkToggle() {
  // Google-inloggning görs per konto, så flera adresser läggs alltid till med lösenord.
  if (bulk.enabled) form.authType = 'password'
  resetBulk()
  testResult.value = null
}

function guessFromBulk() {
  const first = bulkParsed.value.addresses[0]
  if (!first) return
  if (!usernameTouched.value && bulk.usernameMode === 'same') form.username = first.email
  if (form.imapHost) return
  const preset = presetForEmail(first.email)
  if (preset) Object.assign(form, preset)
}

function bulkBody(entry, skipTest) {
  return {
    name: entry.name || form.name,
    email: entry.email,
    username: bulk.usernameMode === 'address' ? entry.email : form.username,
    password: form.password,
    imapHost: form.imapHost,
    imapPort: Number(form.imapPort),
    imapSecurity: form.imapSecurity,
    smtpHost: form.smtpHost,
    smtpPort: Number(form.smtpPort),
    smtpSecurity: form.smtpSecurity,
    smtpUsername: form.smtpUsername,
    tlsVerify: form.tlsVerify,
    signature: form.signature,
    signatureHtml: form.signatureHtml,
    authType: 'password',
    skipTest: skipTest || !bulk.test,
  }
}

async function addAccounts(options) {
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
  await Promise.all(created.map((a) => store.loadFolders(a.id)))
  store.notify(created.length === 1 ? t('Account added') : t('{n} accounts were added', { n: created.length }))
  if (!store.expanded.has(`acc:${created[0].id}`)) store.toggleExpanded(`acc:${created[0].id}`)
  if (!bulkFailures.value.length) {
    await store.selectFolder(created[0].id, 'INBOX')
    store.accountDialog = null
  }
}

const canSubmit = computed(() => {
  if (bulk.enabled) {
    if (!bulkPending.value.length || !form.imapHost || !form.smtpHost || !form.password) return false
    return bulk.usernameMode === 'address' || !!form.username
  }
  if (!form.email || !form.imapHost || !form.smtpHost) return false
  // Google: kräver en ny inloggning, eller att kontot redan är anslutet med Google.
  if (useGoogle.value) return !!oauthState.value || wasGoogle
  // Lösenord: krävs för nya konton och vid byte från Google-inloggning.
  return !!form.username && (!!form.password || (!isNew && !wasGoogle))
})

function body(skipTest = false) {
  const data = { ...form, imapPort: Number(form.imapPort), smtpPort: Number(form.smtpPort), skipTest }
  if (useGoogle.value) {
    delete data.password
    if (oauthState.value) data.oauthState = oauthState.value
  }
  return data
}

async function test() {
  busy.value = true
  error.value = ''
  testResult.value = null
  try {
    // I flerläget testas den första adressen, eftersom alla konton har samma inställningar.
    const first = bulkParsed.value.addresses[0]
    testResult.value = await api.testAccount(bulk.enabled ? bulkBody(first, true) : body())
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
    await store.saveAccount(body(skipTest))
    store.notify(isNew ? t('Account added') : t('Account saved'))
    store.accountDialog = null
  } catch (e) {
    error.value = e.message
    if (e.data?.test) testResult.value = e.data.test
  } finally {
    busy.value = false
  }
}

async function remove() {
  if (!window.confirm(t('Remove the account {email} from PigeonPost? Mail on the server is not affected.', { email: form.email }))) return
  busy.value = true
  try {
    await store.removeAccount(form.id)
    store.accountDialog = null
  } catch (e) {
    error.value = e.message
  } finally {
    busy.value = false
  }
}
</script>

<template>
  <div class="modal-backdrop">
    <form class="modal account" @submit.prevent="bulk.enabled ? addAccounts() : save(false)">
      <div class="modal-header">
        <h2>{{ isNew ? t('Add email account') : t('Account settings') }}</h2>
        <button type="button" class="btn ghost" :title="t('Close')" @click="store.accountDialog = null"><Icon name="close" /></button>
      </div>

      <div class="content">
        <section>
          <label v-if="isNew" class="check bulk-toggle">
            <input v-model="bulk.enabled" type="checkbox" @change="onBulkToggle" />
            <span>
              {{ t('Several addresses with the same settings') }}
              <span class="muted">{{ t('– each address becomes its own account') }}</span>
            </span>
          </label>

          <div class="grid">
            <label for="acc-name">{{ t('Your name') }}</label>
            <input id="acc-name" v-model="form.name" class="input" :placeholder="t('First and last name')" />

            <template v-if="bulk.enabled">
              <label for="acc-emails">{{ t('Email addresses') }}</label>
              <BulkAddresses :state="bulk" :parsed="bulkParsed" input-id="acc-emails" @blur="guessFromBulk">
                <template #hint>{{ t('The name above is used for addresses without a name of their own.') }}</template>
              </BulkAddresses>

              <span class="label">{{ t('Username') }}</span>
              <div class="auth-choice">
                <label class="radio">
                  <input v-model="bulk.usernameMode" type="radio" value="address" />
                  {{ t('The address is the username for each account') }}
                </label>
                <label class="radio">
                  <input v-model="bulk.usernameMode" type="radio" value="same" />
                  {{ t('Same username for all') }}
                </label>
                <input
                  v-if="bulk.usernameMode === 'same'"
                  v-model="form.username"
                  class="input"
                  :placeholder="t('Username')"
                  required
                  @input="usernameTouched = true"
                />
              </div>

              <label for="acc-bulkpass">{{ t('Password') }}</label>
              <input
                id="acc-bulkpass"
                v-model="form.password"
                class="input"
                type="password"
                autocomplete="new-password"
                required
              />
            </template>

            <template v-else>
            <label for="acc-email">{{ t('Email address') }}</label>
            <input id="acc-email" v-model="form.email" class="input" type="email" required @blur="guessFromEmail" />

            <template v-if="isGmail || useGoogle">
              <span class="label">{{ t('Sign-in') }}</span>
              <div class="auth-choice">
                <label class="radio">
                  <input v-model="form.authType" type="radio" value="oauth-google" :disabled="!googleConfigured && !wasGoogle" />
                  {{ t('Sign in with Google') }} <span class="muted">{{ t('(recommended, no app password needed)') }}</span>
                </label>
                <label class="radio">
                  <input v-model="form.authType" type="radio" value="password" />
                  {{ t('Password or app password') }}
                </label>
                <p v-if="!googleConfigured" class="muted small">
                  {{ t('Google sign-in is not enabled on the server. The administrator enables it with GOOGLE_CLIENT_ID and GOOGLE_CLIENT_SECRET (see README).') }}
                </p>
              </div>
            </template>

            <template v-if="useGoogle">
              <span class="label">{{ t('Google account') }}</span>
              <div class="google-account">
                <p v-if="googleEmail" class="signed-in">
                  <span :class="oauthState ? 'ok' : ''">✓</span>
                  {{ oauthState ? t('Signed in as') : t('Connected as') }} <strong>{{ googleEmail }}</strong>
                </p>
                <GoogleSignIn
                  v-if="googleConfigured"
                  :login-hint="form.email"
                  :label="googleEmail ? t('Sign in again') : t('Sign in with Google')"
                  :disabled="busy"
                  @signed-in="onGoogleSignedIn"
                />
              </div>
            </template>
            <template v-else>
              <label for="acc-user">{{ t('Username') }}</label>
              <input id="acc-user" v-model="form.username" class="input" required @input="usernameTouched = true" />
              <label for="acc-pass">{{ t('Password') }}</label>
              <input
                id="acc-pass"
                v-model="form.password"
                class="input"
                type="password"
                autocomplete="new-password"
                :placeholder="isNew || wasGoogle ? '' : t('Unchanged')"
                :required="isNew || wasGoogle"
              />
            </template>
            </template>
          </div>

          <template v-if="bulk.enabled">
            <label class="check">
              <input v-model="bulk.test" type="checkbox" />
              {{ t('Test the sign-in for each address (uncheck if the server blocks many sign-ins in a row)') }}
            </label>
            <BulkStatus :entries="bulk.entries" :exists-text="t('Already exists as an account')" />
            <p class="muted small">
              {{ t('If the addresses are aliases of the same mailbox, it is usually better to add them as extra senders in one account, under') }}
              <strong>{{ t('More senders') }}</strong>.
            </p>
          </template>
          <p v-if="!useGoogle" class="hint muted">
            {{ t('Gmail, iCloud and Yahoo require an app password when two-step verification is enabled.') }}
          </p>
        </section>

        <section>
          <div class="section-title">
            <h3>{{ t('Servers') }}</h3>
            <select class="input preset" @change="applyPreset">
              <option value="">{{ t('Preset…') }}</option>
              <option value="gmail">Gmail</option>
              <option value="outlook">Outlook / Microsoft 365</option>
              <option value="icloud">iCloud</option>
              <option value="yahoo">Yahoo</option>
              <option value="greenmail">{{ t('Test server (docker compose --profile dev)') }}</option>
            </select>
          </div>
          <div class="servers">
            <span />
            <span class="muted">{{ t('Server') }}</span>
            <span class="muted">{{ t('Port') }}</span>
            <span class="muted">{{ t('Security') }}</span>

            <label>{{ t('Incoming (IMAP)') }}</label>
            <input v-model="form.imapHost" class="input" required />
            <input v-model="form.imapPort" class="input" type="number" min="1" max="65535" required />
            <select v-model="form.imapSecurity" class="input" @change="onSecurityChange('imap')">
              <option value="tls">SSL/TLS</option>
              <option value="starttls">STARTTLS</option>
              <option value="none">{{ t('None') }}</option>
            </select>

            <label>{{ t('Outgoing (SMTP)') }}</label>
            <input v-model="form.smtpHost" class="input" required />
            <input v-model="form.smtpPort" class="input" type="number" min="1" max="65535" required />
            <select v-model="form.smtpSecurity" class="input" @change="onSecurityChange('smtp')">
              <option value="tls">SSL/TLS</option>
              <option value="starttls">STARTTLS</option>
              <option value="none">{{ t('None') }}</option>
            </select>
          </div>
          <div class="grid">
            <label for="acc-smtpuser">{{ t('SMTP username') }}</label>
            <input id="acc-smtpuser" v-model="form.smtpUsername" class="input" :placeholder="t('Same as above')" />
          </div>
          <label class="check">
            <input v-model="form.tlsVerify" type="checkbox" />
            {{ t('Verify server certificate (only turn off for your own servers with self-signed certificates)') }}
          </label>
        </section>

        <section>
          <h3>{{ t('Signature') }}</h3>
          <SignatureEditor v-model:text="form.signature" v-model:html="form.signatureHtml" />
        </section>

        <section>
          <div class="section-title">
            <h3>{{ t('More senders') }}</h3>
            <button v-if="!isNew" type="button" class="btn" @click="editIdentity()">{{ t('Add sender') }}</button>
          </div>
          <p v-if="isNew" class="muted small">{{ t('Save the account first. Then you can add more addresses and SMTP servers to send from.') }}</p>
          <p v-else-if="!identities.length" class="muted small">
            {{ t('Add other addresses to send as, for example an alias or an address with its own SMTP server. You choose the sender in the From field when writing.') }}
          </p>
          <ul v-else class="identities">
            <li v-for="i in identities" :key="i.id">
              <div class="identity-text">
                <strong>{{ i.name }}</strong> <span class="muted">&lt;{{ i.email }}&gt;</span>
                <div class="muted small">
                  {{ i.useAccountSmtp ? t('Sent via the account server ({host})', { host: form.smtpHost }) : t('Own SMTP server: {host}:{port}', { host: i.smtpHost, port: i.smtpPort }) }}
                </div>
              </div>
              <button type="button" class="btn" @click="editIdentity(i)">{{ t('Edit') }}</button>
              <button type="button" class="btn danger" @click="removeIdentity(i)">{{ t('Remove') }}</button>
            </li>
          </ul>
        </section>

        <div v-if="testResult" class="test">
          <p :class="testResult.imap.ok ? 'ok' : 'error-text'">
            IMAP: {{ testResult.imap.ok ? t('connection works') : testResult.imap.error }}
          </p>
          <p :class="testResult.smtp.ok ? 'ok' : 'error-text'">
            SMTP: {{ testResult.smtp.ok ? t('connection works') : testResult.smtp.error }}
          </p>
        </div>
        <p v-if="error" class="error-text">{{ error }}</p>
      </div>

      <div class="modal-footer">
        <button v-if="!isNew" type="button" class="btn danger" :disabled="busy" @click="remove">{{ t('Remove account') }}</button>
        <div class="spacer" />
        <button type="button" class="btn" :disabled="busy || !canSubmit" @click="test">
          {{ bulk.enabled ? t('Test first address') : t('Test connection') }}
        </button>
        <template v-if="bulk.enabled">
          <button
            v-if="bulkFailures.some((e) => e.canSkipTest)"
            type="button"
            class="btn"
            :disabled="busy"
            @click="addAccounts({ skipTest: true, onlyFailed: true })"
          >
            {{ t('Add the failed ones anyway') }}
          </button>
          <button type="submit" class="btn primary" :disabled="busy || !canSubmit">
            {{ busy ? t('Adding…') : bulkPending.length === 1 ? t('Add 1 account') : t('Add {n} accounts', { n: bulkPending.length }) }}
          </button>
        </template>
        <template v-else>
          <button v-if="testResult && (!testResult.imap.ok || !testResult.smtp.ok)" type="button" class="btn" :disabled="busy" @click="save(true)">
            {{ t('Save anyway') }}
          </button>
          <button type="submit" class="btn primary" :disabled="busy || !canSubmit">
            {{ busy ? t('Checking…') : isNew ? t('Add') : t('Save') }}
          </button>
        </template>
      </div>
    </form>
  </div>
</template>

<style scoped>
.account {
  max-width: 680px;
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

.section-title {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  margin-bottom: 8px;
}

.section-title h3 {
  margin: 0;
}

.preset {
  width: auto;
}

.grid {
  display: grid;
  grid-template-columns: 150px minmax(0, 1fr);
  gap: 8px 12px;
  align-items: center;
  margin-bottom: 8px;
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

.auth-choice .radio {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 3px 0;
  color: var(--text);
  text-align: left;
}

.google-account .signed-in {
  margin: 0 0 6px;
}

.servers {
  display: grid;
  grid-template-columns: 150px minmax(0, 1fr) 90px 120px;
  gap: 6px 8px;
  align-items: center;
  margin-bottom: 8px;
}

.servers label {
  text-align: right;
  color: var(--text-muted);
}

.hint {
  margin: 4px 0 0 162px;
  font-size: 12px;
}

.check {
  display: flex;
  gap: 8px;
  align-items: flex-start;
  font-size: 12px;
  margin-top: 6px;
}

.signature {
  resize: vertical;
}

.bulk-toggle {
  margin: 0 0 10px;
  font-size: 13px;
}

.test p {
  margin: 4px 0;
}

.ok {
  color: var(--success);
}

.spacer {
  flex: 1;
}

@media (max-width: 600px) {
  .grid,
  .servers {
    grid-template-columns: 1fr;
  }
  .grid label,
  .servers label {
    text-align: left;
  }
  .hint {
    margin-left: 0;
  }
}
</style>
