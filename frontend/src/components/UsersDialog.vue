<script setup>
import { onBeforeUnmount, onMounted, reactive, ref } from 'vue'
import { api } from '../api'
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

const dateFmt = new Intl.DateTimeFormat('sv-SE', { dateStyle: 'medium' })

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
  const ok = await run(() => api.createUser({ ...form, username }), `Användaren ${username} skapades`)
  if (ok) Object.assign(form, { username: '', password: '', isAdmin: false })
}

function startReset(user) {
  resetFor.value = user.id
  resetPassword.value = generatePassword()
}

async function saveReset(user) {
  const ok = await run(
    () => api.updateUser(user.id, { password: resetPassword.value }),
    `Lösenordet för ${user.username} ändrades`,
  )
  if (ok) resetFor.value = null
}

function toggleAdmin(user) {
  run(() => api.updateUser(user.id, { isAdmin: !user.isAdmin }))
}

function remove(user) {
  const question = `Ta bort användaren ${user.username}? Användarens e-postkonton tas bort från Brevduva (e-posten på servrarna påverkas inte).`
  if (!window.confirm(question)) return
  run(() => api.deleteUser(user.id), `Användaren ${user.username} togs bort`)
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
        <h2>Hantera användare</h2>
        <button class="btn ghost" title="Stäng" @click="store.usersDialog = false"><Icon name="close" /></button>
      </div>

      <div class="content">
        <form class="create" @submit.prevent="create">
          <h3>Ny användare</h3>
          <div class="create-grid">
            <input v-model="form.username" class="input" placeholder="Användarnamn" autocomplete="off" required />
            <div class="password-row">
              <input v-model="form.password" class="input" type="text" placeholder="Lösenord (minst 8 tecken)" autocomplete="new-password" required />
              <button type="button" class="btn" title="Generera ett slumpat lösenord" @click="form.password = generatePassword()">Generera</button>
            </div>
            <label class="check"><input v-model="form.isAdmin" type="checkbox" /> Administratör</label>
            <button class="btn primary" type="submit" :disabled="busy || !form.username || form.password.length < 8">Skapa</button>
          </div>
          <p class="muted hint">Lämna över lösenordet till användaren på ett säkert sätt. Hen kan byta det själv efter inloggning.</p>
        </form>

        <p v-if="error" class="error-text">{{ error }}</p>
        <p v-if="loading" class="muted">Laddar…</p>

        <table v-else>
          <thead>
            <tr>
              <th>Användare</th>
              <th>Roll</th>
              <th>Skapad</th>
              <th />
            </tr>
          </thead>
          <tbody>
            <template v-for="u in users" :key="u.id">
              <tr>
                <td>
                  <strong>{{ u.username }}</strong>
                  <span v-if="u.id === store.user?.id" class="muted"> (du)</span>
                </td>
                <td>{{ u.isAdmin ? 'Administratör' : 'Användare' }}</td>
                <td class="muted">{{ dateFmt.format(new Date(u.createdAt * 1000)) }}</td>
                <td class="actions">
                  <button class="btn" :disabled="busy" @click="startReset(u)">Nytt lösenord</button>
                  <button v-if="u.id !== store.user?.id" class="btn" :disabled="busy" @click="toggleAdmin(u)">
                    {{ u.isAdmin ? 'Gör till användare' : 'Gör till admin' }}
                  </button>
                  <button v-if="u.id !== store.user?.id" class="btn danger" :disabled="busy" @click="remove(u)">Ta bort</button>
                </td>
              </tr>
              <tr v-if="resetFor === u.id" class="reset-row">
                <td colspan="4">
                  <form class="password-row" @submit.prevent="saveReset(u)">
                    <input v-model="resetPassword" class="input" type="text" autocomplete="new-password" />
                    <button type="button" class="btn" @click="resetPassword = generatePassword()">Generera</button>
                    <button class="btn primary" type="submit" :disabled="busy || resetPassword.length < 8">Spara</button>
                    <button type="button" class="btn ghost" @click="resetFor = null">Avbryt</button>
                  </form>
                  <p class="muted hint">Användaren loggas ut på alla enheter.</p>
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
