<script setup>
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { api } from '../api'
import { t } from '../i18n'
import { useMailStore } from '../store'
import AccountDialog from './AccountDialog.vue'
import AiSortDialog from './AiSortDialog.vue'
import SearchDialog from './SearchDialog.vue'
import BackupDialog from './BackupDialog.vue'
import ComposeDialog from './ComposeDialog.vue'
import ContextMenu from './ContextMenu.vue'
import FolderMoveDialog from './FolderMoveDialog.vue'
import FolderPane from './FolderPane.vue'
import IdentityDialog from './IdentityDialog.vue'
import Icon from './Icon.vue'
import SettingsDialog from './SettingsDialog.vue'
import MessageList from './MessageList.vue'
import MessageView from './MessageView.vue'
import PasswordDialog from './PasswordDialog.vue'
import RulesDialog from './RulesDialog.vue'
import ThunderbirdDialog from './ThunderbirdDialog.vue'
import UsersDialog from './UsersDialog.vue'

const emit = defineEmits(['logout'])
const store = useMailStore()

const refreshing = ref(false)
const menuOpen = ref(false)
const layout = store.layout

// Läsrutan bredvid listan (i stället för under eller ovanför).
const sideBySide = computed(() => layout.preview === 'left' || layout.preview === 'right')

const listStyle = computed(() => {
  if (layout.preview === 'hidden') return {}
  return sideBySide.value ? { width: layout.listWidth + 'px' } : { height: layout.listHeight + 'px' }
})

function startResize(kind, event) {
  const startX = event.clientX
  const startY = event.clientY
  const start = { ...layout }
  // Panelerna kan ligga i omvänd ordning; då växer de åt andra hållet.
  const folderSign = layout.folders === 'right' ? -1 : 1
  const listSign = layout.preview === 'top' || layout.preview === 'left' ? -1 : 1
  const onMove = (e) => {
    const dx = e.clientX - startX
    const dy = e.clientY - startY
    if (kind === 'folders') {
      layout.folderWidth = Math.round(Math.min(500, Math.max(160, start.folderWidth + folderSign * dx)))
    } else if (sideBySide.value) {
      const folders = layout.folders === 'hidden' ? 0 : layout.folderWidth
      const max = Math.max(240, window.innerWidth - folders - 240)
      layout.listWidth = Math.round(Math.min(max, Math.max(240, start.listWidth + listSign * dx)))
    } else {
      layout.listHeight = Math.round(Math.min(window.innerHeight - 200, Math.max(100, start.listHeight + listSign * dy)))
    }
  }
  const onUp = () => {
    window.removeEventListener('pointermove', onMove)
    window.removeEventListener('pointerup', onUp)
    document.body.style.userSelect = ''
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
  if (
    store.compose ||
    store.accountDialog ||
    store.identityDialog ||
    store.usersDialog ||
    store.passwordDialog ||
    store.thunderbirdDialog ||
    store.settingsDialog ||
    store.backupDialog ||
    store.aiDialog ||
    store.rulesDialog ||
    store.searchDialog ||
    store.folderMoveDialog
  ) {
    return
  }
  if (event.key === 'F8' || event.key === 'F9') {
    // Som i Thunderbird: F8 visar eller döljer läsrutan, F9 mappfältet.
    event.preventDefault()
    togglePane(event.key === 'F8' ? 'preview' : 'folders')
    return
  }
  if (event.key === 'Escape' && store.readerOpen) {
    store.readerOpen = false
    return
  }
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
  } else if (event.key === 'Enter' && store.openMessage && layout.preview === 'hidden') {
    store.readerOpen = true
  } else if (event.key === 'F5') {
    event.preventDefault()
    refresh()
  }
}

// Senast använda placering, så att F8/F9 återställer panelen där den var.
const lastPosition = {
  folders: layout.folders === 'hidden' ? 'left' : layout.folders,
  preview: layout.preview === 'hidden' ? 'bottom' : layout.preview,
}

function togglePane(pane) {
  if (layout[pane] === 'hidden') {
    layout[pane] = lastPosition[pane]
  } else {
    lastPosition[pane] = layout[pane]
    layout[pane] = 'hidden'
  }
}

watch(
  () => layout.preview,
  (value) => {
    if (value !== 'hidden') store.readerOpen = false
  },
)

let pollTimer = null
let statusTimer = null

onMounted(async () => {
  window.addEventListener('keydown', onKeydown)
  try {
    await store.init()
    if (!store.accounts.length) store.accountDialog = {}
  } catch (e) {
    store.notify(e.message, 'error')
  }
  // Hämta nya meddelanden regelbundet, som Thunderbird gör.
  // Under den första synken av ett konto uppdateras förloppet oftare.
  statusTimer = setInterval(() => {
    if (document.hidden) return
    if (Object.values(store.cacheStatus).some((s) => !s.complete || s.state !== 'idle')) store.loadCacheStatus()
  }, 10000)
  pollTimer = setInterval(() => {
    if (document.hidden) return
    store.loadCacheStatus()
    store.accounts.forEach((a) => store.loadFolders(a.id))
    if (!store.filter.q && (store.current.view || store.messages.length <= 100)) store.loadMessages({ keepSelection: true })
  }, 30000) // Läsningen går mot mejlcachen och är billig, så ny post kan visas oftare än förr.
})

onBeforeUnmount(() => {
  window.removeEventListener('keydown', onKeydown)
  clearInterval(pollTimer)
  clearInterval(statusTimer)
})
</script>

<template>
  <div class="layout" @click="menuOpen = false">
    <header class="toolbar">
      <button class="btn ghost" :title="t('Get messages (F5)')" :disabled="refreshing" @click="refresh">
        <Icon name="refresh" :class="{ spin: refreshing }" /> {{ t('Get Mail') }}
      </button>
      <button class="btn ghost" :title="t('Write a new message (Ctrl+N)')" @click="store.startCompose('new', null)">
        <Icon name="drafts" /> {{ t('Write') }}
      </button>
      <span class="sep" />
      <button class="btn ghost" :disabled="!canReply" :title="t('Reply (Ctrl+R)')" @click="store.startCompose('reply')">
        <Icon name="reply" /> {{ t('Reply') }}
      </button>
      <button class="btn ghost" :disabled="!canReply" :title="t('Reply all (Ctrl+Shift+R)')" @click="store.startCompose('replyAll')">
        <Icon name="replyAll" /> {{ t('Reply all') }}
      </button>
      <button class="btn ghost" :disabled="!canReply" :title="t('Forward (Ctrl+L)')" @click="store.startCompose('forward')">
        <Icon name="forward" /> {{ t('Forward') }}
      </button>
      <span class="sep" />
      <button class="btn ghost" :disabled="!hasSelection" :title="t('Archive (A)')" @click="store.archive()">
        <Icon name="archive" /> {{ t('Archive|action') }}
      </button>
      <button class="btn ghost" :disabled="!hasSelection" :title="t('Mark as junk (J)')" @click="store.markJunk()">
        <Icon name="junk" /> {{ t('Junk|action') }}
      </button>
      <button class="btn ghost" :disabled="!hasSelection" :title="t('Delete (Delete)')" @click="store.deleteMessages()">
        <Icon name="trash" /> {{ t('Delete') }}
      </button>

      <div class="spacer" />

      <button class="btn ghost" :title="t('Layout: arrange panes (F8 reading pane, F9 folders)')" @click="store.settingsDialog = true">
        <Icon name="layout" />
      </button>
      <div class="menu-wrap" @click.stop>
        <button class="btn ghost" :title="t('Menu')" @click="menuOpen = !menuOpen"><Icon name="settings" /></button>
        <div v-if="menuOpen" class="menu">
          <div class="menu-user">
            <Icon name="account" />
            <div>
              <strong>{{ store.user?.username }}</strong>
              <span v-if="store.user?.isAdmin" class="muted"> · {{ t('administrator') }}</span>
            </div>
          </div>
          <button @click="store.accountDialog = {}; menuOpen = false"><Icon name="mail" /> {{ t('Add email account') }}</button>
          <button
            v-for="a in store.accounts"
            :key="a.id"
            @click="store.accountDialog = { ...a }; menuOpen = false"
          >
            <Icon name="settings" /> {{ t('Settings for {email}', { email: a.email }) }}
          </button>
          <button @click="store.thunderbirdDialog = true; menuOpen = false">
            <Icon name="download" /> {{ t('Import/export (Thunderbird)') }}
          </button>
          <button @click="store.backupDialog = true; menuOpen = false">
            <Icon name="backup" /> {{ t('Back up email accounts') }}
          </button>
          <button @click="store.searchDialog = true; menuOpen = false">
            <Icon name="search" /> {{ t('Search all folders') }}
          </button>
          <button @click="store.aiDialog = true; menuOpen = false">
            <Icon name="sparkles" /> {{ t('Sort with AI') }}
          </button>
          <button @click="store.rulesDialog = true; menuOpen = false">
            <Icon name="filter" /> {{ t('Rules') }}
          </button>
          <button @click="store.settingsDialog = true; menuOpen = false">
            <Icon name="layout" /> {{ t('Settings') }}
          </button>
          <hr />
          <button v-if="store.user?.isAdmin" @click="store.usersDialog = true; menuOpen = false">
            <Icon name="users" /> {{ t('Manage users') }}
          </button>
          <button @click="store.passwordDialog = true; menuOpen = false"><Icon name="key" /> {{ t('Change password') }}</button>
          <button @click="logout"><Icon name="logout" /> {{ t('Sign out') }}</button>
        </div>
      </div>
    </header>

    <div class="main" :class="{ 'folders-right': layout.folders === 'right' }">
      <template v-if="layout.folders !== 'hidden'">
        <aside class="folders" :style="{ width: layout.folderWidth + 'px' }">
          <FolderPane />
        </aside>
        <div class="splitter vertical" @pointerdown.prevent="startResize('folders', $event)" />
      </template>
      <section class="content" :class="`preview-${layout.preview}`">
        <div class="list" :style="listStyle">
          <MessageList />
        </div>
        <template v-if="layout.preview !== 'hidden'">
          <div
            class="splitter"
            :class="sideBySide ? 'vertical' : 'horizontal'"
            @pointerdown.prevent="startResize('list', $event)"
          />
          <div class="preview">
            <MessageView />
          </div>
        </template>
      </section>
    </div>

    <div v-if="store.readerOpen && layout.preview === 'hidden'" class="modal-backdrop" @click.self="store.readerOpen = false">
      <div class="modal reader">
        <div class="modal-header">
          <h2>{{ store.openMessage?.subject || t('Message') }}</h2>
          <button class="btn ghost" :title="t('Close (Esc)')" @click="store.readerOpen = false"><Icon name="close" /></button>
        </div>
        <div class="reader-body">
          <MessageView />
        </div>
      </div>
    </div>

    <ComposeDialog v-if="store.compose" :key="store.compose.key" />
    <AccountDialog v-if="store.accountDialog" />
    <IdentityDialog v-if="store.identityDialog" />
    <UsersDialog v-if="store.usersDialog" />
    <ThunderbirdDialog v-if="store.thunderbirdDialog" />
    <PasswordDialog v-if="store.passwordDialog" />
    <SettingsDialog v-if="store.settingsDialog" />
    <BackupDialog v-if="store.backupDialog" />
    <AiSortDialog v-if="store.aiDialog" />
    <RulesDialog v-if="store.rulesDialog" />
    <SearchDialog v-if="store.searchDialog" />
    <FolderMoveDialog v-if="store.folderMoveDialog" />
    <ContextMenu v-if="store.contextMenu" />

    <div class="toasts">
      <div v-for="toast in store.toasts" :key="toast.id" class="toast" :class="toast.type">{{ toast.text }}</div>
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

.main.folders-right {
  flex-direction: row-reverse;
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
  min-height: 0;
}

.content.preview-top {
  flex-direction: column-reverse;
}

.content.preview-right {
  flex-direction: row;
}

.content.preview-left {
  flex-direction: row-reverse;
}

.list {
  flex-shrink: 0;
  min-height: 100px;
  min-width: 0;
  display: flex;
  flex-direction: column;
}

.content.preview-right .list,
.content.preview-left .list {
  min-width: 240px;
}

.content.preview-hidden .list {
  flex: 1;
}

.preview {
  flex: 1;
  min-height: 0;
  min-width: 0;
  display: flex;
  flex-direction: column;
}

.reader {
  max-width: 960px;
  height: calc(100vh - 32px);
}

.reader .modal-header h2 {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.reader-body {
  flex: 1;
  min-height: 0;
  display: flex;
  flex-direction: column;
  border-radius: 0 0 10px 10px;
  overflow: hidden;
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
