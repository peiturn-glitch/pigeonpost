<script setup>
import { onBeforeUnmount, onMounted, ref } from 'vue'
import { api } from '../api'
import { t } from '../i18n'
import { useMailStore } from '../store'
import Icon from './Icon.vue'

const store = useMailStore()
const current = ref('')
const next = ref('')
const confirm = ref('')
const error = ref('')
const busy = ref(false)

async function submit() {
  error.value = ''
  if (next.value.length < 8) {
    error.value = t('The new password must be at least 8 characters.')
    return
  }
  if (next.value !== confirm.value) {
    error.value = t('The new passwords do not match.')
    return
  }
  busy.value = true
  try {
    await api.changePassword(current.value, next.value)
    store.notify(t('Password changed. Other signed-in devices have been signed out.'))
    store.passwordDialog = false
  } catch (e) {
    error.value = e.message
  } finally {
    busy.value = false
  }
}

function onKey(event) {
  if (event.key === 'Escape') store.passwordDialog = false
}
onMounted(() => window.addEventListener('keydown', onKey))
onBeforeUnmount(() => window.removeEventListener('keydown', onKey))
</script>

<template>
  <div class="modal-backdrop">
    <form class="modal password" @submit.prevent="submit">
      <div class="modal-header">
        <h2>{{ t('Change password') }}</h2>
        <button type="button" class="btn ghost" :title="t('Close')" @click="store.passwordDialog = false"><Icon name="close" /></button>
      </div>
      <div class="content">
        <input type="text" :value="store.user?.username" autocomplete="username" hidden readonly />
        <label>
          <span>{{ t('Current password') }}</span>
          <input v-model="current" class="input" type="password" autocomplete="current-password" autofocus required />
        </label>
        <label>
          <span>{{ t('New password') }}</span>
          <input v-model="next" class="input" type="password" autocomplete="new-password" required />
        </label>
        <label>
          <span>{{ t('Repeat new password') }}</span>
          <input v-model="confirm" class="input" type="password" autocomplete="new-password" required />
        </label>
        <p v-if="error" class="error-text">{{ error }}</p>
      </div>
      <div class="modal-footer">
        <button type="button" class="btn" @click="store.passwordDialog = false">{{ t('Cancel') }}</button>
        <button type="submit" class="btn primary" :disabled="busy || !current || !next || !confirm">
          {{ busy ? t('Saving…') : t('Change password') }}
        </button>
      </div>
    </form>
  </div>
</template>

<style scoped>
.password {
  max-width: 400px;
}

.content {
  padding: 16px;
  display: flex;
  flex-direction: column;
  gap: 12px;
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

p {
  margin: 0;
}
</style>
