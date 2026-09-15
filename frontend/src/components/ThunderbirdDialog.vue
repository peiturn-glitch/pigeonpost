<script setup>
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
import { api, thunderbirdExportUrl } from '../api'
import { useMailStore } from '../store'
import { readPrefsFromZip } from '../zipreader'
import GoogleSignIn from './GoogleSignIn.vue'
import Icon from './Icon.vue'

const store = useMailStore()

const step = ref('choose') // choose | preview | done
const reading = ref(false)
const importing = ref(false)
const error = ref('')
const fileName = ref('')
const accounts = ref([]) // förhandsgranskade konton, med val och lösenord
const skipped = ref([])
const testConnections = ref(true)

const securityNames = { tls: 'SSL/TLS', starttls: 'STARTTLS', none: 'okrypterad' }

const selected = computed(() => accounts.value.filter((a) => a.selected))
const canImport = computed(
  () =>
    selected.value.length > 0 &&
    selected.value.every(
      (a) => (a.useGoogle ? a.oauthState : a.password) && a.identities.every((i) => !needsOwnPassword(a, i) || i.smtpPassword),
    ),
)

const googleConfigured = computed(() => store.oauthProviders.google?.configured)

// En avsändare med egen SMTP-inloggning behöver eget lösenord om den inte kan återanvända kontots lösenord.
function needsOwnPassword(account, identity) {
  if (identity.useAccountSmtp || !identity.smtpUsername) return false
  return account.useGoogle || identity.smtpUsername !== account.username
}

function onGoogleSignedIn(account, result) {
  account.oauthState = result.state
  account.googleEmail = result.email
}

async function onFile(event) {
  const file = event.target.files[0]
  event.target.value = ''
  if (!file) return
  error.value = ''
  reading.value = true
  fileName.value = file.name
  try {
    const isZip = file.name.toLowerCase().endsWith('.zip') || file.type.includes('zip')
    const prefs = isZip ? await readPrefsFromZip(file) : await file.text()
    const preview = await api.thunderbirdImportPreview(prefs)
    accounts.value = preview.accounts.map((a) => ({
      ...a,
      selected: !a.alreadyExists,
      password: '',
      // Gmail-konton loggar in med Google om servern har det aktiverat.
      useGoogle: a.oauthProvider === 'google' && !!store.oauthProviders.google?.configured,
      oauthState: '',
      googleEmail: '',
      status: '',
      message: '',
      identities: a.identities.map((i) => ({ ...i, smtpPassword: '' })),
    }))
    skipped.value = preview.skipped
    if (!accounts.value.length && !skipped.value.length) {
      error.value = 'Hittade inga e-postkonton i filen.'
      return
    }
    step.value = 'preview'
  } catch (e) {
    error.value = e.message
  } finally {
    reading.value = false
  }
}

async function importAccount(account) {
  account.status = 'working'
  account.message = ''
  try {
    // Skicka bara kontofälten; resten är förhandsgranskningens egna tillstånd.
    const { identities, warnings, sourceKey, accountName, alreadyExists, selected: _selected, status, message, canSkipTest,
      skipTestOnRetry, oauthProvider, oauthInThunderbird, useGoogle, oauthState, googleEmail, ...fields } = account
    if (useGoogle) {
      fields.authType = 'oauth-google'
      fields.oauthState = oauthState
      delete fields.password
    }
    const created = await api.createAccount({ ...fields, skipTest: !testConnections.value || account.skipTestOnRetry })

    const failures = []
    for (const identity of identities) {
      const { smtpPassword, ...identityFields } = identity
      const body = { ...identityFields, skipTest: true }
      if (!identity.useAccountSmtp && identity.smtpUsername) {
        body.smtpPassword = needsOwnPassword(account, identity) ? smtpPassword : account.password
      }
      try {
        await api.createIdentity(created.id, body)
      } catch (e) {
        failures.push(`${identity.email}: ${e.message}`)
      }
    }
    account.status = 'ok'
    account.message = failures.length ? `Importerat, men några avsändare misslyckades: ${failures.join('; ')}` : 'Importerat'
    account.selected = false
  } catch (e) {
    account.status = 'error'
    const test = e.data?.test
    const details = test ? [test.imap?.ok ? null : `IMAP: ${test.imap.error}`, test.smtp?.ok ? null : `SMTP: ${test.smtp.error}`].filter(Boolean).join(' · ') : ''
    account.message = details || e.message
    account.canSkipTest = !!test
  }
}

async function runImport() {
  importing.value = true
  error.value = ''
  for (const account of selected.value) await importAccount(account)
  importing.value = false
  await store.loadAccounts()
  await Promise.all(store.accounts.map((a) => store.loadFolders(a.id)))
  const done = accounts.value.filter((a) => a.status === 'ok').length
  if (done) store.notify(`${done} konto(n) importerades`)
}

async function retryWithoutTest(account) {
  account.skipTestOnRetry = true
  importing.value = true
  await importAccount(account)
  importing.value = false
  await store.loadAccounts()
}

function reset() {
  step.value = 'choose'
  accounts.value = []
  skipped.value = []
  error.value = ''
}

function close() {
  store.thunderbirdDialog = false
}

function onKey(event) {
  if (event.key === 'Escape' && !importing.value) close()
}
onMounted(() => window.addEventListener('keydown', onKey))
onBeforeUnmount(() => window.removeEventListener('keydown', onKey))
</script>

<template>
  <div class="modal-backdrop">
    <div class="modal tb">
      <div class="modal-header">
        <h2>Importera och exportera (Thunderbird-format)</h2>
        <button class="btn ghost" title="Stäng" :disabled="importing" @click="close"><Icon name="close" /></button>
      </div>

      <div class="content">
        <template v-if="step === 'choose'">
          <section>
            <h3><Icon name="download" /> Exportera till Thunderbird</h3>
            <p class="muted">
              Laddar ned en zip-fil med <code>prefs.js</code> som innehåller dina e-postkonton, avsändare, servrar och signaturer.
              I Thunderbird väljer du <strong>Verktyg → Importera</strong>, sedan <strong>Thunderbird</strong> och exportfilen (zip),
              och bockar i <strong>Konton och inställningar</strong>. Lösenord ingår inte, så Thunderbird frågar efter dem första gången.
            </p>
            <a class="btn primary" :href="thunderbirdExportUrl" download>Ladda ned inställningar</a>
          </section>

          <section>
            <h3><Icon name="inbox" /> Importera från Thunderbird</h3>
            <p class="muted">
              Välj en export från Thunderbird (<strong>Verktyg → Exportera</strong>, en zip-fil) eller filen <code>prefs.js</code> från
              profilmappen (<strong>Hjälp → Felsökningsinformation → Profilmapp</strong>). Zip-filen läses i webbläsaren och bara
              inställningarna skickas till servern, så din e-post laddas inte upp.
            </p>
            <label class="btn primary file" :class="{ disabled: reading }">
              {{ reading ? 'Läser…' : 'Välj fil…' }}
              <input type="file" accept=".zip,.js,application/zip" :disabled="reading" hidden @change="onFile" />
            </label>
          </section>
        </template>

        <template v-else>
          <p class="muted">
            Från <strong>{{ fileName }}</strong>. Lösenord finns inte i Thunderbirds inställningsfil, så ange lösenord eller logga in med
            Google för de konton du vill importera.
          </p>

          <div v-for="account in accounts" :key="account.sourceKey" class="account" :class="account.status">
            <label class="head">
              <input v-model="account.selected" type="checkbox" :disabled="importing || account.status === 'ok'" />
              <span class="title">
                <strong>{{ account.name || account.email }}</strong>
                <span class="muted"> &lt;{{ account.email }}&gt;</span>
              </span>
              <span v-if="account.alreadyExists && account.status !== 'ok'" class="badge">Finns redan</span>
              <span v-if="account.status === 'ok'" class="badge ok">Importerat</span>
            </label>

            <div class="details muted">
              IMAP {{ account.imapHost }}:{{ account.imapPort }} ({{ securityNames[account.imapSecurity] }}) ·
              SMTP {{ account.smtpHost }}:{{ account.smtpPort }} ({{ securityNames[account.smtpSecurity] }}) ·
              användare {{ account.username }}
            </div>
            <ul v-if="account.identities.length" class="identities">
              <li v-for="identity in account.identities" :key="identity.email">
                <span>
                  Avsändare: {{ identity.name }} &lt;{{ identity.email }}&gt;
                  <span class="muted">{{ identity.useAccountSmtp ? '' : `· egen SMTP ${identity.smtpHost}:${identity.smtpPort}` }}</span>
                </span>
                <input
                  v-if="account.selected && needsOwnPassword(account, identity)"
                  v-model="identity.smtpPassword"
                  class="input small"
                  type="password"
                  autocomplete="new-password"
                  :placeholder="`SMTP-lösenord för ${identity.smtpUsername}`"
                />
              </li>
            </ul>
            <ul v-if="account.warnings.length" class="warnings">
              <li v-for="w in account.warnings" :key="w">⚠ {{ w }}</li>
            </ul>

            <div v-if="account.selected" class="password">
              <template v-if="account.oauthProvider === 'google' && googleConfigured">
                <label class="radio"><input v-model="account.useGoogle" type="radio" :value="true" :disabled="importing" /> Logga in med Google</label>
                <label class="radio"><input v-model="account.useGoogle" type="radio" :value="false" :disabled="importing" /> Lösenord eller app-lösenord</label>
              </template>
              <div v-if="account.useGoogle" class="google">
                <p v-if="account.googleEmail" class="ok-text">✓ Inloggad som {{ account.googleEmail }}</p>
                <GoogleSignIn
                  :login-hint="account.email"
                  :label="account.googleEmail ? 'Logga in igen' : 'Logga in med Google'"
                  :disabled="importing"
                  @signed-in="(result) => onGoogleSignedIn(account, result)"
                />
              </div>
              <input
                v-else
                v-model="account.password"
                class="input"
                type="password"
                autocomplete="new-password"
                :placeholder="`Lösenord för ${account.username}`"
                :disabled="importing"
              />
            </div>

            <p v-if="account.message" :class="account.status === 'error' ? 'error-text' : 'ok-text'" class="message">
              {{ account.status === 'working' ? 'Importerar…' : account.message }}
              <button v-if="account.status === 'error' && account.canSkipTest" class="btn small-btn" :disabled="importing" @click="retryWithoutTest(account)">
                Importera ändå
              </button>
            </p>
            <p v-else-if="account.status === 'working'" class="muted message">Importerar…</p>
          </div>

          <div v-if="skipped.length" class="skipped">
            <h3>Hoppas över</h3>
            <ul>
              <li v-for="s in skipped" :key="s.name + s.type">{{ s.name }} <span class="muted">– {{ s.reason }}</span></li>
            </ul>
          </div>
        </template>

        <p v-if="error" class="error-text">{{ error }}</p>
      </div>

      <div v-if="step === 'preview'" class="modal-footer">
        <button class="btn" :disabled="importing" @click="reset">Välj annan fil</button>
        <label class="check">
          <input v-model="testConnections" type="checkbox" :disabled="importing" />
          Testa anslutningarna
        </label>
        <div class="spacer" />
        <button class="btn" :disabled="importing" @click="close">Stäng</button>
        <button class="btn primary" :disabled="importing || !canImport" @click="runImport">
          {{ importing ? 'Importerar…' : `Importera ${selected.length} konto(n)` }}
        </button>
      </div>
    </div>
  </div>
</template>

<style scoped>
.tb {
  max-width: 760px;
}

.content {
  overflow-y: auto;
  padding: 12px 16px 16px;
}

section {
  padding: 8px 0 16px;
  border-bottom: 1px solid var(--border);
}

section + section {
  padding-top: 16px;
  border-bottom: 0;
}

h3 {
  display: flex;
  align-items: center;
  gap: 8px;
  margin: 0 0 6px;
  font-size: 14px;
}

p {
  margin: 0 0 10px;
}

code {
  font-family: ui-monospace, Consolas, monospace;
  background: var(--pane-alt);
  padding: 0 4px;
  border-radius: 3px;
}

a.btn {
  text-decoration: none;
}

.file.disabled {
  opacity: 0.6;
  pointer-events: none;
}

.account {
  border: 1px solid var(--border);
  border-radius: var(--radius);
  padding: 10px 12px;
  margin-bottom: 10px;
}

.account.ok {
  border-color: var(--success);
}

.account.error {
  border-color: var(--danger);
}

.head {
  display: flex;
  align-items: center;
  gap: 8px;
}

.title {
  flex: 1;
  min-width: 0;
  overflow-wrap: anywhere;
}

.badge {
  font-size: 11px;
  padding: 1px 8px;
  border-radius: 9px;
  background: var(--pane-alt);
  border: 1px solid var(--border);
  white-space: nowrap;
}

.badge.ok {
  color: var(--success);
  border-color: var(--success);
}

.details {
  font-size: 12px;
  margin: 4px 0 0 24px;
  overflow-wrap: anywhere;
}

.identities,
.warnings {
  list-style: none;
  margin: 6px 0 0 24px;
  padding: 0;
  font-size: 12px;
}

.identities li {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-wrap: wrap;
  padding: 2px 0;
}

.warnings li {
  color: #9a6700;
}

.password {
  margin: 8px 0 0 24px;
  max-width: 320px;
}

.password .radio {
  display: flex;
  align-items: center;
  gap: 6px;
  font-size: 13px;
  padding: 2px 0;
}

.password .google {
  margin-top: 4px;
}

.password .google p {
  margin: 0 0 4px;
  font-size: 12px;
}

.input.small {
  width: 240px;
  padding: 3px 6px;
  font-size: 12px;
}

.message {
  margin: 8px 0 0 24px;
  font-size: 12px;
  display: flex;
  align-items: center;
  gap: 8px;
  flex-wrap: wrap;
}

.ok-text {
  color: var(--success);
}

.small-btn {
  padding: 2px 8px;
  font-size: 12px;
}

.skipped ul {
  margin: 0;
  padding-left: 18px;
}

.check {
  display: flex;
  align-items: center;
  gap: 6px;
}

.spacer {
  flex: 1;
}
</style>
