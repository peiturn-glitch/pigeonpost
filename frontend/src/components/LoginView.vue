<script setup>
import { computed, ref } from 'vue'
import { api } from '../api'
import Icon from './Icon.vue'

const props = defineProps({
  // Första start: skapa administratören i stället för att logga in.
  setup: { type: Boolean, default: false },
})
const emit = defineEmits(['authenticated'])

const username = ref('')
const password = ref('')
const confirm = ref('')
const error = ref('')
const busy = ref(false)

const canSubmit = computed(() => username.value && password.value && (!props.setup || confirm.value))

async function submit() {
  error.value = ''
  if (props.setup) {
    if (password.value.length < 8) {
      error.value = 'Lösenordet måste vara minst 8 tecken.'
      return
    }
    if (password.value !== confirm.value) {
      error.value = 'Lösenorden matchar inte.'
      return
    }
  }
  busy.value = true
  try {
    const result = props.setup
      ? await api.setup(username.value.trim(), password.value)
      : await api.login(username.value.trim(), password.value)
    emit('authenticated', result.user)
  } catch (e) {
    error.value = e.message
    password.value = ''
    confirm.value = ''
  } finally {
    busy.value = false
  }
}
</script>

<template>
  <div class="login">
    <form class="card" @submit.prevent="submit">
      <div class="logo"><Icon name="mail" :size="36" /></div>
      <h1>PigeonPost</h1>
      <p v-if="setup" class="muted">
        Välkommen! Skapa administratörskontot. Administratören kan sedan lägga till fler användare.
      </p>
      <p v-else class="muted">Logga in för att öppna din e-post.</p>

      <label>
        <span>Användarnamn</span>
        <input v-model="username" class="input" autocomplete="username" autocapitalize="none" autofocus required />
      </label>
      <label>
        <span>Lösenord</span>
        <input
          v-model="password"
          class="input"
          type="password"
          :autocomplete="setup ? 'new-password' : 'current-password'"
          required
        />
      </label>
      <label v-if="setup">
        <span>Upprepa lösenordet</span>
        <input v-model="confirm" class="input" type="password" autocomplete="new-password" required />
      </label>

      <p v-if="error" class="error-text">{{ error }}</p>
      <button class="btn primary" type="submit" :disabled="busy || !canSubmit">
        <template v-if="setup">{{ busy ? 'Skapar…' : 'Skapa administratör' }}</template>
        <template v-else>{{ busy ? 'Loggar in…' : 'Logga in' }}</template>
      </button>
    </form>
    <nav class="legal">
      <a href="/privacy.html">Integritetspolicy</a>
      <span aria-hidden="true">·</span>
      <a href="/terms.html">Användarvillkor</a>
    </nav>
  </div>
</template>

<style scoped>
.login {
  height: 100%;
  display: flex;
  flex-direction: column;
  gap: 16px;
  align-items: center;
  justify-content: center;
  padding: 16px;
}

.card {
  width: 100%;
  max-width: 360px;
  background: var(--pane);
  border: 1px solid var(--border);
  border-radius: 12px;
  padding: 28px;
  display: flex;
  flex-direction: column;
  gap: 12px;
  box-shadow: var(--shadow);
}

.logo {
  color: var(--accent);
}

h1 {
  margin: 0;
  font-size: 20px;
}

p {
  margin: 0;
}

label {
  display: flex;
  flex-direction: column;
  gap: 4px;
}

label span {
  font-size: 12px;
  color: var(--text-muted);
}

.btn {
  justify-content: center;
  padding: 8px;
}

.legal {
  display: flex;
  gap: 8px;
  font-size: 12px;
  color: var(--text-muted);
}

.legal a {
  color: inherit;
}

.legal a:hover {
  color: var(--accent);
}
</style>
