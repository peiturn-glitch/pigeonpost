<script setup>
import { computed, ref } from 'vue'
import { api } from '../api'
import { LANGUAGES, lang, setLanguage, t } from '../i18n'
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
      error.value = t('The password must be at least 8 characters.')
      return
    }
    if (password.value !== confirm.value) {
      error.value = t('The passwords do not match.')
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
        {{ t('Welcome! Create the administrator account. The administrator can then add more users.') }}
      </p>
      <p v-else class="muted">{{ t('Sign in to open your email.') }}</p>

      <label>
        <span>{{ t('Username') }}</span>
        <input v-model="username" class="input" autocomplete="username" autocapitalize="none" autofocus required />
      </label>
      <label>
        <span>{{ t('Password') }}</span>
        <input
          v-model="password"
          class="input"
          type="password"
          :autocomplete="setup ? 'new-password' : 'current-password'"
          required
        />
      </label>
      <label v-if="setup">
        <span>{{ t('Repeat the password') }}</span>
        <input v-model="confirm" class="input" type="password" autocomplete="new-password" required />
      </label>

      <p v-if="error" class="error-text">{{ error }}</p>
      <button class="btn primary" type="submit" :disabled="busy || !canSubmit">
        <template v-if="setup">{{ busy ? t('Creating…') : t('Create administrator') }}</template>
        <template v-else>{{ busy ? t('Signing in…') : t('Sign in') }}</template>
      </button>
    </form>
    <nav class="languages" :aria-label="t('Language')">
      <template v-for="(l, i) in LANGUAGES" :key="l.code">
        <span v-if="i > 0" aria-hidden="true">·</span>
        <button type="button" class="language" :class="{ active: l.code === lang }" :lang="l.code" @click="setLanguage(l.code)">
          {{ l.name }}
        </button>
      </template>
    </nav>
    <nav class="legal">
      <a href="/privacy.html">{{ t('Privacy policy') }}</a>
      <span aria-hidden="true">·</span>
      <a href="/terms.html">{{ t('Terms of service') }}</a>
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

.languages,
.legal {
  display: flex;
  gap: 8px;
  font-size: 12px;
  color: var(--text-muted);
}

.languages {
  margin-bottom: -8px;
}

.language {
  border: 0;
  background: none;
  padding: 0;
  color: inherit;
  cursor: pointer;
}

.language.active {
  color: var(--text);
  font-weight: 600;
}

.language:hover,
.legal a:hover {
  color: var(--accent);
}

.legal a {
  color: inherit;
}
</style>
