<script setup>
import { computed, onBeforeUnmount, onMounted, reactive, ref } from 'vue'
import { api } from '../api'
import { useMailStore } from '../store'
import { defaultPorts, mailPresets, presetForEmail } from '../utils'
import Icon from './Icon.vue'

const store = useMailStore()
const { account, identity } = store.identityDialog
const isNew = !identity

const form = reactive({
  id: identity?.id ?? null,
  name: identity?.name ?? account.name,
  email: identity?.email ?? '',
  replyTo: identity?.replyTo ?? '',
  signature: identity?.signature ?? '',
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
const canSubmit = computed(
  () => form.email.includes('@') && (form.useAccountSmtp || (form.smtpHost && !needsPassword.value)),
)

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
    testResult.value = await api.testIdentity(account.id, body())
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
    store.notify(isNew ? 'Avsändaren lades till' : 'Avsändaren sparades')
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
    <form class="modal identity" @submit.prevent="save(false)">
      <div class="modal-header">
        <h2>{{ isNew ? 'Ny avsändare' : 'Redigera avsändare' }} · {{ account.email }}</h2>
        <button type="button" class="btn ghost" title="Stäng" @click="close"><Icon name="close" /></button>
      </div>

      <div class="content">
        <section class="grid">
          <label for="id-name">Namn</label>
          <input id="id-name" v-model="form.name" class="input" placeholder="Förnamn Efternamn" />
          <label for="id-email">E-postadress</label>
          <input id="id-email" v-model="form.email" class="input" type="email" required @blur="onEmailBlur" />
          <label for="id-replyto">Svarsadress</label>
          <input id="id-replyto" v-model="form.replyTo" class="input" type="email" placeholder="Valfri, om svar ska gå till en annan adress" />
          <label for="id-sig">Signatur</label>
          <textarea id="id-sig" v-model="form.signature" class="input" rows="3" />
        </section>

        <section>
          <h3>Utgående server</h3>
          <label class="radio">
            <input v-model="form.useAccountSmtp" type="radio" :value="true" />
            Använd kontots server ({{ account.smtpHost }}:{{ account.smtpPort }})
          </label>
          <label class="radio">
            <input v-model="form.useAccountSmtp" type="radio" :value="false" @change="onCustomSmtp" />
            Egen SMTP-server
          </label>
          <p v-if="form.useAccountSmtp" class="muted small">
            Många servrar tillåter bara avsändaradresser som hör till kontot. Välj egen SMTP-server om adressen tillhör en annan tjänst.
          </p>

          <div v-if="!form.useAccountSmtp" class="smtp">
            <div class="servers">
              <span class="muted">Server</span>
              <span class="muted">Port</span>
              <span class="muted">Säkerhet</span>
              <input v-model="form.smtpHost" class="input" placeholder="smtp.exempel.se" required />
              <input v-model="form.smtpPort" class="input" type="number" min="1" max="65535" required />
              <select v-model="form.smtpSecurity" class="input" @change="form.smtpPort = defaultPorts.smtp[form.smtpSecurity]">
                <option value="tls">SSL/TLS</option>
                <option value="starttls">STARTTLS</option>
                <option value="none">Ingen</option>
              </select>
            </div>
            <div class="grid">
              <label for="id-user">Användarnamn</label>
              <input id="id-user" v-model="form.smtpUsername" class="input" placeholder="Tomt = ingen inloggning" autocomplete="off" @input="usernameTouched = true" />
              <label for="id-pass">Lösenord</label>
              <input
                id="id-pass"
                v-model="form.smtpPassword"
                class="input"
                type="password"
                autocomplete="new-password"
                :placeholder="hasStoredPassword ? 'Oförändrat' : ''"
                :disabled="!form.smtpUsername"
              />
            </div>
            <div class="row">
              <label class="check">
                <input v-model="form.tlsVerify" type="checkbox" />
                Verifiera servercertifikat
              </label>
              <select class="input preset" @change="applyPreset">
                <option value="">Förinställning…</option>
                <option value="gmail">Gmail</option>
                <option value="outlook">Outlook / Microsoft 365</option>
                <option value="icloud">iCloud</option>
                <option value="yahoo">Yahoo</option>
                <option value="greenmail">Testserver</option>
              </select>
            </div>
          </div>
        </section>

        <p v-if="testResult" :class="testResult.ok ? 'ok' : 'error-text'">
          SMTP: {{ testResult.ok ? 'anslutningen fungerar' : testResult.error }}
        </p>
        <p v-if="error" class="error-text">{{ error }}</p>
      </div>

      <div class="modal-footer">
        <button type="button" class="btn" :disabled="busy || !canSubmit" @click="test">Testa SMTP</button>
        <button v-if="testResult && !testResult.ok" type="button" class="btn" :disabled="busy" @click="save(true)">Spara ändå</button>
        <button type="submit" class="btn primary" :disabled="busy || !canSubmit">
          {{ busy ? 'Kontrollerar…' : isNew ? 'Lägg till' : 'Spara' }}
        </button>
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

.grid label {
  text-align: right;
  color: var(--text-muted);
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
