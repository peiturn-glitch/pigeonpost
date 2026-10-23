<script setup>
import { computed, onBeforeUnmount, onMounted, reactive, ref } from 'vue'
import { api } from '../api'
import { useBulkAddresses } from '../bulkAddresses'
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
  if (!window.confirm(`Ta bort avsändaren ${identity.email}?`)) return
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
  store.notify(created.length === 1 ? 'Kontot lades till' : `${created.length} konton lades till`)
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
    store.notify(isNew ? 'Kontot lades till' : 'Kontot sparades')
    store.accountDialog = null
  } catch (e) {
    error.value = e.message
    if (e.data?.test) testResult.value = e.data.test
  } finally {
    busy.value = false
  }
}

async function remove() {
  if (!window.confirm(`Ta bort kontot ${form.email} från PigeonPost? E-posten på servern påverkas inte.`)) return
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
        <h2>{{ isNew ? 'Lägg till e-postkonto' : 'Kontoinställningar' }}</h2>
        <button type="button" class="btn ghost" title="Stäng" @click="store.accountDialog = null"><Icon name="close" /></button>
      </div>

      <div class="content">
        <section>
          <label v-if="isNew" class="check bulk-toggle">
            <input v-model="bulk.enabled" type="checkbox" @change="onBulkToggle" />
            <span>
              Flera adresser med samma inställningar
              <span class="muted">– varje adress blir ett eget konto</span>
            </span>
          </label>

          <div class="grid">
            <label for="acc-name">Ditt namn</label>
            <input id="acc-name" v-model="form.name" class="input" placeholder="Förnamn Efternamn" />

            <template v-if="bulk.enabled">
              <label for="acc-emails">E-postadresser</label>
              <BulkAddresses :state="bulk" :parsed="bulkParsed" input-id="acc-emails" @blur="guessFromBulk">
                <template #hint>Namnet ovan används för de adresser som saknar eget namn.</template>
              </BulkAddresses>

              <span class="label">Användarnamn</span>
              <div class="auth-choice">
                <label class="radio">
                  <input v-model="bulk.usernameMode" type="radio" value="address" />
                  Adressen är användarnamn för varje konto
                </label>
                <label class="radio">
                  <input v-model="bulk.usernameMode" type="radio" value="same" />
                  Samma användarnamn för alla
                </label>
                <input
                  v-if="bulk.usernameMode === 'same'"
                  v-model="form.username"
                  class="input"
                  placeholder="Användarnamn"
                  required
                  @input="usernameTouched = true"
                />
              </div>

              <label for="acc-bulkpass">Lösenord</label>
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
            <label for="acc-email">E-postadress</label>
            <input id="acc-email" v-model="form.email" class="input" type="email" required @blur="guessFromEmail" />

            <template v-if="isGmail || useGoogle">
              <span class="label">Inloggning</span>
              <div class="auth-choice">
                <label class="radio">
                  <input v-model="form.authType" type="radio" value="oauth-google" :disabled="!googleConfigured && !wasGoogle" />
                  Logga in med Google <span class="muted">(rekommenderas, inget app-lösenord)</span>
                </label>
                <label class="radio">
                  <input v-model="form.authType" type="radio" value="password" />
                  Lösenord eller app-lösenord
                </label>
                <p v-if="!googleConfigured" class="muted small">
                  Google-inloggning är inte aktiverad på servern. Administratören aktiverar den med GOOGLE_CLIENT_ID och
                  GOOGLE_CLIENT_SECRET (se README).
                </p>
              </div>
            </template>

            <template v-if="useGoogle">
              <span class="label">Google-konto</span>
              <div class="google-account">
                <p v-if="googleEmail" class="signed-in">
                  <span :class="oauthState ? 'ok' : ''">✓</span>
                  {{ oauthState ? 'Inloggad som' : 'Ansluten som' }} <strong>{{ googleEmail }}</strong>
                </p>
                <GoogleSignIn
                  v-if="googleConfigured"
                  :login-hint="form.email"
                  :label="googleEmail ? 'Logga in igen' : 'Logga in med Google'"
                  :disabled="busy"
                  @signed-in="onGoogleSignedIn"
                />
              </div>
            </template>
            <template v-else>
              <label for="acc-user">Användarnamn</label>
              <input id="acc-user" v-model="form.username" class="input" required @input="usernameTouched = true" />
              <label for="acc-pass">Lösenord</label>
              <input
                id="acc-pass"
                v-model="form.password"
                class="input"
                type="password"
                autocomplete="new-password"
                :placeholder="isNew || wasGoogle ? '' : 'Oförändrat'"
                :required="isNew || wasGoogle"
              />
            </template>
            </template>
          </div>

          <template v-if="bulk.enabled">
            <label class="check">
              <input v-model="bulk.test" type="checkbox" />
              Testa inloggningen för varje adress (ta bort bocken om servern spärrar många inloggningar i rad)
            </label>
            <BulkStatus :entries="bulk.entries" exists-text="Finns redan som konto" />
            <p class="muted small">
              Är adresserna alias till samma brevlåda är det oftast bättre att lägga till dem som extra avsändare i ett
              konto, under <strong>Fler avsändare</strong>.
            </p>
          </template>
          <p v-if="!useGoogle" class="hint muted">
            Gmail, iCloud och Yahoo kräver ett app-lösenord när tvåstegsverifiering är aktiverad.
          </p>
        </section>

        <section>
          <div class="section-title">
            <h3>Servrar</h3>
            <select class="input preset" @change="applyPreset">
              <option value="">Förinställning…</option>
              <option value="gmail">Gmail</option>
              <option value="outlook">Outlook / Microsoft 365</option>
              <option value="icloud">iCloud</option>
              <option value="yahoo">Yahoo</option>
              <option value="greenmail">Testserver (docker compose --profile dev)</option>
            </select>
          </div>
          <div class="servers">
            <span />
            <span class="muted">Server</span>
            <span class="muted">Port</span>
            <span class="muted">Säkerhet</span>

            <label>Inkommande (IMAP)</label>
            <input v-model="form.imapHost" class="input" required />
            <input v-model="form.imapPort" class="input" type="number" min="1" max="65535" required />
            <select v-model="form.imapSecurity" class="input" @change="onSecurityChange('imap')">
              <option value="tls">SSL/TLS</option>
              <option value="starttls">STARTTLS</option>
              <option value="none">Ingen</option>
            </select>

            <label>Utgående (SMTP)</label>
            <input v-model="form.smtpHost" class="input" required />
            <input v-model="form.smtpPort" class="input" type="number" min="1" max="65535" required />
            <select v-model="form.smtpSecurity" class="input" @change="onSecurityChange('smtp')">
              <option value="tls">SSL/TLS</option>
              <option value="starttls">STARTTLS</option>
              <option value="none">Ingen</option>
            </select>
          </div>
          <div class="grid">
            <label for="acc-smtpuser">SMTP-användarnamn</label>
            <input id="acc-smtpuser" v-model="form.smtpUsername" class="input" placeholder="Samma som ovan" />
          </div>
          <label class="check">
            <input v-model="form.tlsVerify" type="checkbox" />
            Verifiera servercertifikat (stäng bara av för egna servrar med självsignerade certifikat)
          </label>
        </section>

        <section>
          <h3>Signatur</h3>
          <SignatureEditor v-model:text="form.signature" v-model:html="form.signatureHtml" />
        </section>

        <section>
          <div class="section-title">
            <h3>Fler avsändare</h3>
            <button v-if="!isNew" type="button" class="btn" @click="editIdentity()">Lägg till avsändare</button>
          </div>
          <p v-if="isNew" class="muted small">Spara kontot först. Sedan kan du lägga till fler adresser och SMTP-servrar att skicka från.</p>
          <p v-else-if="!identities.length" class="muted small">
            Lägg till andra adresser att skicka som, till exempel ett alias eller en adress med egen SMTP-server.
            Du väljer avsändare i Från-fältet när du skriver.
          </p>
          <ul v-else class="identities">
            <li v-for="i in identities" :key="i.id">
              <div class="identity-text">
                <strong>{{ i.name }}</strong> <span class="muted">&lt;{{ i.email }}&gt;</span>
                <div class="muted small">
                  {{ i.useAccountSmtp ? `Skickas via kontots server (${form.smtpHost})` : `Egen SMTP-server: ${i.smtpHost}:${i.smtpPort}` }}
                </div>
              </div>
              <button type="button" class="btn" @click="editIdentity(i)">Redigera</button>
              <button type="button" class="btn danger" @click="removeIdentity(i)">Ta bort</button>
            </li>
          </ul>
        </section>

        <div v-if="testResult" class="test">
          <p :class="testResult.imap.ok ? 'ok' : 'error-text'">
            IMAP: {{ testResult.imap.ok ? 'anslutningen fungerar' : testResult.imap.error }}
          </p>
          <p :class="testResult.smtp.ok ? 'ok' : 'error-text'">
            SMTP: {{ testResult.smtp.ok ? 'anslutningen fungerar' : testResult.smtp.error }}
          </p>
        </div>
        <p v-if="error" class="error-text">{{ error }}</p>
      </div>

      <div class="modal-footer">
        <button v-if="!isNew" type="button" class="btn danger" :disabled="busy" @click="remove">Ta bort konto</button>
        <div class="spacer" />
        <button type="button" class="btn" :disabled="busy || !canSubmit" @click="test">
          {{ bulk.enabled ? 'Testa första adressen' : 'Testa anslutning' }}
        </button>
        <template v-if="bulk.enabled">
          <button
            v-if="bulkFailures.some((e) => e.canSkipTest)"
            type="button"
            class="btn"
            :disabled="busy"
            @click="addAccounts({ skipTest: true, onlyFailed: true })"
          >
            Lägg till de misslyckade ändå
          </button>
          <button type="submit" class="btn primary" :disabled="busy || !canSubmit">
            {{ busy ? 'Lägger till…' : `Lägg till ${bulkPending.length} ${bulkPending.length === 1 ? 'konto' : 'konton'}` }}
          </button>
        </template>
        <template v-else>
          <button v-if="testResult && (!testResult.imap.ok || !testResult.smtp.ok)" type="button" class="btn" :disabled="busy" @click="save(true)">
            Spara ändå
          </button>
          <button type="submit" class="btn primary" :disabled="busy || !canSubmit">
            {{ busy ? 'Kontrollerar…' : isNew ? 'Lägg till' : 'Spara' }}
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
