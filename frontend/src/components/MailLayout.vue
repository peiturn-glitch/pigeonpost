<script setup>
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
import { api } from '../api'
import { useMailStore } from '../store'
import AccountDialog from './AccountDialog.vue'
import ComposeDialog from './ComposeDialog.vue'
import ContextMenu from './ContextMenu.vue'
import FolderPane from './FolderPane.vue'
import IdentityDialog from './IdentityDialog.vue'
import Icon from './Icon.vue'
import MessageList from './MessageList.vue'
import MessageView from './MessageView.vue'
import PasswordDialog from './PasswordDialog.vue'
import ThunderbirdDialog from './ThunderbirdDialog.vue'
import UsersDialog from './UsersDialog.vue'

const emit = defineEmits(['logout'])
const store = useMailStore()

const folderWidth = ref(readNumber('tbk.folderWidth', 240))
const listHeight = ref(readNumber('tbk.listHeight', 320))
const refreshing = ref(false)
const menuOpen = ref(false)

function readNumber(key, fallback) {
  try {
    const v = Number(localStorage.getItem(key))
    return Number.isFinite(v) && v > 0 ? v : fallback
  } catch {
    return fallback
  }
}

function persist(key, value) {
  try {
    localStorage.setItem(key, String(Math.round(value)))
  } catch {
    // Ignoreras; storleken gäller bara denna flik.
  }
}

function startResize(kind, event) {
  const startX = event.clientX
  const startY = event.clientY
  const startWidth = folderWidth.value
  const startHeight = listHeight.value
  const onMove = (e) => {
    if (kind === 'folders') {
      folderWidth.value = Math.min(500, Math.max(160, startWidth + e.clientX - startX))
    } else {
      listHeight.value = Math.min(window.innerHeight - 200, Math.max(100, startHeight + e.clientY - startY))
    }
  }
  const onUp = () => {
    window.removeEventListener('pointermove', onMove)
    window.removeEventListener('pointerup', onUp)
    document.body.style.userSelect = ''
    persist('tbk.folderWidth', folderWidth.value)
    persist('tbk.listHeight', listHeight.value)
  }
  document.body.style.userSelect = 'none'
  window.addEventListener('pointermove', onMove)
  window.addEventListener('pointerup', onUp)
}

async function refresh() {
  refreshing.value = true
  try {
    await store.refreshAll()
  } finally {
    refreshing.value = false
  }
}

const hasSelection = computed(() => store.selection.size > 0)
const canReply = computed(() => !!store.openMessage)
const firstSelected = computed(() => store.selectedMessages[0])

async function logout() {
  menuOpen.value = false
  await api.logout().catch(() => {})
  emit('logout')
}

function isTyping(event) {
  const el = event.target
  return el.isContentEditable || ['INPUT', 'TEXTAREA', 'SELECT'].includes(el.tagName)
}

function onKeydown(event) {
  if (store.compose || store.accountDialog || store.identityDialog || store.usersDialog || store.passwordDialog || store.thunderbirdDialog) return
  const ctrl = event.ctrlKey || event.metaKey
  if (ctrl && event.key.toLowerCase() === 'n') {
    event.preventDefault()
    store.startCompose('new', null)
    return
  }
  if (isTyping(event)) return

  if (ctrl && event.key.toLowerCase() === 'r' && store.openMessage) {
    event.preventDefault()
    store.startCompose(event.shiftKey ? 'replyAll' : 'reply')
  } else if (ctrl && event.key.toLowerCase() === 'l' && store.openMessage) {
    event.preventDefault()
    store.startCompose('forward')
  } else if (ctrl && event.key.toLowerCase() === 'a') {
    event.preventDefault()
    store.selectAll()
  } else if (event.key === 'Delete') {
    event.preventDefault()
    store.deleteMessages()
  } else if (event.key === 'ArrowDown' || event.key === 'ArrowUp') {
    event.preventDefault()
    store.moveSelection(event.key === 'ArrowDown' ? 1 : -1)
  } else if (event.key === 's' && firstSelected.value) {
    store.setFlag('flagged', !firstSelected.value.flagged)
  } else if (event.key === 'm' && firstSelected.value) {
    store.setFlag('seen', !firstSelected.value.seen)
  } else if (event.key === 'j' && hasSelection.value) {
    store.markJunk()
  } else if (event.key === 'a' && hasSelection.value) {
    store.archive()
  } else if (event.key === 'F5') {
    event.preventDefault()
    refresh()
  }
}

let pollTimer = null

onMounted(async () => {
  window.addEventListener('keydown', onKeydown)
  try {
    await store.init()
    if (!store.accounts.length) store.accountDialog = {}
  } catch (e) {
    store.notify(e.message, 'error')
  }
  // Hämta nya meddelanden regelbundet, som Thunderbird gör.
  pollTimer = setInterval(() => {
    if (document.hidden) return
    store.accounts.forEach((a) => store.loadFolders(a.id))
    if (!store.filter.q && store.messages.length <= 100) store.loadMessages({ keepSelection: true })
  }, 60000)
})

onBeforeUnmount(() => {
  window.removeEventListener('keydown', onKeydown)
  clearInterval(pollTimer)
})
</script>

<template>
  <div class="layout" @click="menuOpen = false">
    <header class="toolbar">
      <button class="btn ghost" title="Hämta meddelanden (F5)" :disabled="refreshing" @click="refresh">
        <Icon name="refresh" :class="{ spin: refreshing }" /> Hämta
      </button>
      <button class="btn ghost" title="Skriv nytt meddelande (Ctrl+N)" @click="store.startCompose('new', null)">
        <Icon name="drafts" /> Skriv
      </button>
      <span class="sep" />
      <button class="btn ghost" :disabled="!canReply" title="Svara (Ctrl+R)" @click="store.startCompose('reply')">
        <Icon name="reply" /> Svara
      </button>
      <button class="btn ghost" :disabled="!canReply" title="Svara alla (Ctrl+Shift+R)" @click="store.startCompose('replyAll')">
        <Icon name="replyAll" /> Svara alla
      </button>
      <button class="btn ghost" :disabled="!canReply" title="Vidarebefordra (Ctrl+L)" @click="store.startCompose('forward')">
        <Icon name="forward" /> Vidarebefordra
      </button>
      <span class="sep" />
      <button class="btn ghost" :disabled="!hasSelection" title="Arkivera (A)" @click="store.archive()">
        <Icon name="archive" /> Arkivera
      </button>
      <button class="btn ghost" :disabled="!hasSelection" title="Skräppost (J)" @click="store.markJunk()">
        <Icon name="junk" /> Skräp
      </button>
      <button class="btn ghost" :disabled="!hasSelection" title="Ta bort (Delete)" @click="store.deleteMessages()">
        <Icon name="trash" /> Ta bort
      </button>

      <div class="spacer" />

      <div class="menu-wrap" @click.stop>
        <button class="btn ghost" title="Meny" @click="menuOpen = !menuOpen"><Icon name="settings" /></button>
        <div v-if="menuOpen" class="menu">
          <div class="menu-user">
            <Icon name="account" />
            <div>
              <strong>{{ store.user?.username }}</strong>
              <span v-if="store.user?.isAdmin" class="muted"> · administratör</span>
            </div>
          </div>
          <button @click="store.accountDialog = {}; menuOpen = false"><Icon name="mail" /> Lägg till e-postkonto</button>
          <button
            v-for="a in store.accounts"
            :key="a.id"
            @click="store.accountDialog = { ...a }; menuOpen = false"
          >
            <Icon name="settings" /> Inställningar för {{ a.email }}
          </button>
          <button @click="store.thunderbirdDialog = true; menuOpen = false">
            <Icon name="download" /> Importera/exportera (Thunderbird)
          </button>
          <hr />
          <button v-if="store.user?.isAdmin" @click="store.usersDialog = true; menuOpen = false">
            <Icon name="users" /> Hantera användare
          </button>
          <button @click="store.passwordDialog = true; menuOpen = false"><Icon name="key" /> Byt lösenord</button>
          <button @click="logout"><Icon name="logout" /> Logga ut</button>
        </div>
      </div>
    </header>

    <div class="main">
      <aside class="folders" :style="{ width: folderWidth + 'px' }">
        <FolderPane />
      </aside>
      <div class="splitter vertical" @pointerdown.prevent="startResize('folders', $event)" />
      <section class="content">
        <div class="list" :style="{ height: listHeight + 'px' }">
          <MessageList />
        </div>
        <div class="splitter horizontal" @pointerdown.prevent="startResize('list', $event)" />
        <div class="preview">
          <MessageView />
        </div>
      </section>
    </div>

    <ComposeDialog v-if="store.compose" :key="store.compose.key" />
    <AccountDialog v-if="store.accountDialog" />
    <IdentityDialog v-if="store.identityDialog" />
    <UsersDialog v-if="store.usersDialog" />
    <ThunderbirdDialog v-if="store.thunderbirdDialog" />
    <PasswordDialog v-if="store.passwordDialog" />
    <ContextMenu v-if="store.contextMenu" />

    <div class="toasts">
      <div v-for="t in store.toasts" :key="t.id" class="toast" :class="t.type">{{ t.text }}</div>
    </div>
  </div>
</template>

<style scoped>
.layout {
  height: 100%;
  display: flex;
  flex-direction: column;
}

.toolbar {
  display: flex;
  align-items: center;
  gap: 2px;
  padding: 6px 8px;
  background: var(--toolbar);
  border-bottom: 1px solid var(--border);
  overflow-x: auto;
  flex-shrink: 0;
}

.sep {
  width: 1px;
  height: 20px;
  background: var(--border);
  margin: 0 6px;
}

.spacer {
  flex: 1;
}

.spin {
  animation: spin 1s linear infinite;
}

@keyframes spin {
  to {
    transform: rotate(360deg);
  }
}

.menu-wrap {
  position: relative;
}

/* Fast position: verktygsfältet scrollar horisontellt och skulle annars klippa menyn. */
.menu {
  position: fixed;
  right: 8px;
  top: 44px;
  background: var(--pane);
  border: 1px solid var(--border);
  border-radius: var(--radius);
  box-shadow: var(--shadow);
  min-width: 240px;
  padding: 4px;
  z-index: 40;
  display: flex;
  flex-direction: column;
}

.menu button {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 7px 10px;
  border: 0;
  background: none;
  text-align: left;
  border-radius: 4px;
  cursor: pointer;
}

.menu button:hover {
  background: var(--hover);
}

.menu-user {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 8px 10px;
  border-bottom: 1px solid var(--border);
  margin-bottom: 4px;
}

.menu hr {
  border: 0;
  border-top: 1px solid var(--border);
  margin: 4px 0;
}

.main {
  flex: 1;
  display: flex;
  min-height: 0;
}

.folders {
  background: var(--pane-alt);
  overflow: auto;
  flex-shrink: 0;
}

.content {
  flex: 1;
  display: flex;
  flex-direction: column;
  min-width: 0;
}

.list {
  flex-shrink: 0;
  min-height: 100px;
  display: flex;
  flex-direction: column;
}

.preview {
  flex: 1;
  min-height: 0;
  display: flex;
  flex-direction: column;
}

.splitter {
  background: var(--border);
  flex-shrink: 0;
}

.splitter.vertical {
  width: 1px;
  cursor: col-resize;
  position: relative;
}

.splitter.horizontal {
  height: 1px;
  cursor: row-resize;
  position: relative;
}

.splitter::after {
  content: '';
  position: absolute;
  inset: -3px;
}

.toasts {
  position: fixed;
  bottom: 16px;
  right: 16px;
  display: flex;
  flex-direction: column;
  gap: 8px;
  z-index: 100;
  max-width: 420px;
}

.toast {
  background: var(--text);
  color: var(--bg);
  padding: 10px 14px;
  border-radius: var(--radius);
  box-shadow: var(--shadow);
}

.toast.error {
  background: var(--danger);
  color: #fff;
}
</style>
