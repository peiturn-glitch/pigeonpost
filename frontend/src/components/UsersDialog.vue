<script setup>
import { onBeforeUnmount, onMounted, reactive, ref } from 'vue'
import { api } from '../api'
import { locale, t } from '../i18n'
import { useMailStore } from '../store'
import Icon from './Icon.vue'

const store = useMailStore()
const users = ref([])
const loading = ref(true)
const error = ref('')
const busy = ref(false)

const form = reactive({ username: '', password: '', isAdmin: false })
const resetFor = ref(null) // användar-id vars lösenord återställs
const resetPassword = ref('')

function formatDate(seconds) {
  return new Intl.DateTimeFormat(locale(), { dateStyle: 'medium' }).format(new Date(seconds * 1000))
}

async function load() {
  loading.value = true
  try {
    users.value = await api.users()
  } catch (e) {
    error.value = e.message
  } finally {
    loading.value = false
  }
}

// Slumpat lösenord utan tecken som är lätta att förväxla.
function generatePassword() {
  const alphabet = 'abcdefghjkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789'
  const bytes = crypto.getRandomValues(new Uint8Array(16))
  return Array.from(bytes, (b) => alphabet[b % alphabet.length]).join('')
}

async function run(action, successText) {
  busy.value = true
  error.value = ''
  try {
    await action()
    if (successText) store.notify(successText)
    await load()
    return true
  } catch (e) {
    error.value = e.message
    return false
  } finally {
    busy.value = false
  }
}

async function create() {
  const username = form.username.trim()
  const ok = await run(() => api.createUser({ ...form, username }), t('User {name} was created', { name: username }))
  if (ok) Object.assign(form, { username: '', password: '', isAdmin: false })
}

function startReset(user) {
  resetFor.value = user.id
  resetPassword.value = generatePassword()
}

async function saveReset(user) {
  const ok = await run(
    () => api.updateUser(user.id, { password: resetPassword.value }),
    t('The password for {name} was changed', { name: user.username }),
  )
  if (ok) resetFor.value = null
}

function toggleAdmin(user) {
  run(() => api.updateUser(user.id, { isAdmin: !user.isAdmin }))
}

function remove(user) {
  const question = t(
    'Delete the user {name}? Their email accounts are removed from PigeonPost (the mail on the servers is not affected).',
    { name: user.username },
  )
  if (!window.confirm(question)) return
  run(() => api.deleteUser(user.id), t('User {name} was deleted', { name: user.username }))
}

function onKey(event) {
  if (event.key === 'Escape') store.usersDialog = false
}

onMounted(() => {
  window.addEventListener('keydown', onKey)
  load()
})
onBeforeUnmount(() => window.removeEventListener('keydown', onKey))
</script>

<template>
  <div class="modal-backdrop">
    <div class="modal users">
      <div class="modal-header">
        <h2>{{ t('Manage users') }}</h2>
        <button class="btn ghost" :title="t('Close')" @click="store.usersDialog = false"><Icon name="close" /></button>
      </div>

      <div class="content">
        <form class="create" @submit.prevent="create">
          <h3>{{ t('New user') }}</h3>
          <div class="create-grid">
            <input v-model="form.username" class="input" :placeholder="t('Username')" autocomplete="off" required />
            <div class="password-row">
              <input v-model="form.password" class="input" type="text" :placeholder="t('Password (at least 8 characters)')" autocomplete="new-password" required />
              <button type="button" class="btn" :title="t('Generate a random password')" @click="form.password = generatePassword()">{{ t('Generate') }}</button>
            </div>
            <label class="check"><input v-model="form.isAdmin" type="checkbox" /> {{ t('Administrator') }}</label>
            <button class="btn primary" type="submit" :disabled="busy || !form.username || form.password.length < 8">{{ t('Create') }}</button>
          </div>
          <p class="muted hint">{{ t('Hand the password to the user in a secure way. They can change it themselves after signing in.') }}</p>
        </form>

        <p v-if="error" class="error-text">{{ error }}</p>
        <p v-if="loading" class="muted">{{ t('Loading…') }}</p>

        <table v-else>
          <thead>
            <tr>
              <th>{{ t('User') }}</th>
              <th>{{ t('Role') }}</th>
              <th>{{ t('Created') }}</th>
              <th />
            </tr>
          </thead>
          <tbody>
            <template v-for="u in users" :key="u.id">
              <tr>
                <td>
                  <strong>{{ u.username }}</strong>
                  <span v-if="u.id === store.user?.id" class="muted"> {{ t('(you)') }}</span>
                </td>
                <td>{{ u.isAdmin ? t('Administrator') : t('User') }}</td>
                <td class="muted">{{ formatDate(u.createdAt) }}</td>
                <td class="actions">
                  <button class="btn" :disabled="busy" @click="startReset(u)">{{ t('New password') }}</button>
                  <button v-if="u.id !== store.user?.id" class="btn" :disabled="busy" @click="toggleAdmin(u)">
                    {{ u.isAdmin ? t('Make user') : t('Make admin') }}
                  </button>
                  <button v-if="u.id !== store.user?.id" class="btn danger" :disabled="busy" @click="remove(u)">{{ t('Delete') }}</button>
                </td>
              </tr>
              <tr v-if="resetFor === u.id" class="reset-row">
                <td colspan="4">
                  <form class="password-row" @submit.prevent="saveReset(u)">
                    <input v-model="resetPassword" class="input" type="text" autocomplete="new-password" />
                    <button type="button" class="btn" @click="resetPassword = generatePassword()">{{ t('Generate') }}</button>
                    <button class="btn primary" type="submit" :disabled="busy || resetPassword.length < 8">{{ t('Save') }}</button>
                    <button type="button" class="btn ghost" @click="resetFor = null">{{ t('Cancel') }}</button>
                  </form>
                  <p class="muted hint">{{ t('The user is signed out on all devices.') }}</p>
                </td>
              </tr>
            </template>
          </tbody>
        </table>
      </div>
    </div>
  </div>
</template>

<style scoped>
.users {
  max-width: 820px;
}

.content {
  overflow-y: auto;
  padding: 12px 16px 16px;
}

h3 {
  margin: 0 0 8px;
  font-size: 13px;
}

.create {
  padding-bottom: 12px;
  margin-bottom: 12px;
  border-bottom: 1px solid var(--border);
}

.create-grid {
  display: grid;
  grid-template-columns: minmax(0, 1fr) minmax(0, 1.6fr) auto auto;
  gap: 8px;
  align-items: center;
}

.password-row {
  display: flex;
  gap: 6px;
  align-items: center;
}

.check {
  display: flex;
  gap: 6px;
  align-items: center;
  white-space: nowrap;
}

.hint {
  margin: 6px 0 0;
  font-size: 12px;
}

table {
  width: 100%;
  border-collapse: collapse;
}

th {
  text-align: left;
  font-weight: 600;
  color: var(--text-muted);
  font-size: 12px;
  padding: 6px 8px;
  border-bottom: 1px solid var(--border);
}

td {
  padding: 8px;
  border-bottom: 1px solid var(--border);
  vertical-align: middle;
}

.actions {
  display: flex;
  gap: 6px;
  justify-content: flex-end;
  flex-wrap: wrap;
}

.actions .btn {
  padding: 3px 8px;
  font-size: 12px;
}

.reset-row td {
  background: var(--pane-alt);
}

@media (max-width: 700px) {
  .create-grid {
    grid-template-columns: 1fr;
  }
}
</style>
