<script setup>
import { onMounted, ref } from 'vue'
import { api, onUnauthorized } from './api'
import LoginView from './components/LoginView.vue'
import MailLayout from './components/MailLayout.vue'
import { useMailStore } from './store'

const store = useMailStore()
const state = ref('loading') // loading | setup | login | ready | error
const error = ref('')

onUnauthorized(() => {
  store.reset()
  state.value = 'login'
})

async function checkSession() {
  state.value = 'loading'
  try {
    const session = await api.session()
    if (session.setupRequired) {
      state.value = 'setup'
    } else if (session.authenticated) {
      store.user = session.user
      state.value = 'ready'
    } else {
      state.value = 'login'
    }
  } catch (e) {
    error.value = e.message
    state.value = 'error'
  }
}

function onAuthenticated(user) {
  // Rensa allt från en eventuell tidigare användare i samma flik.
  store.reset()
  store.user = user
  state.value = 'ready'
}

function onLogout() {
  store.reset()
  state.value = 'login'
}

onMounted(checkSession)
</script>

<template>
  <div v-if="state === 'loading'" class="center muted">Laddar…</div>
  <div v-else-if="state === 'error'" class="center">
    <p class="error-text">Kunde inte nå servern: {{ error }}</p>
    <button class="btn" @click="checkSession">Försök igen</button>
  </div>
  <LoginView v-else-if="state === 'setup' || state === 'login'" :setup="state === 'setup'" @authenticated="onAuthenticated" />
  <MailLayout v-else :key="store.user?.id" @logout="onLogout" />
</template>

<style scoped>
.center {
  height: 100%;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 12px;
}
</style>
