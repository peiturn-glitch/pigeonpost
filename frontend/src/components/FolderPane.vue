<script setup>
import { computed, ref } from 'vue'
import { useMailStore } from '../store'
import { buildFolderTree, formatListDate } from '../utils'
import FolderNode from './FolderNode.vue'
import Icon from './Icon.vue'

const store = useMailStore()

const FOLDER_TYPE = 'application/x-brevduva-folder'
const dropAccount = ref(null)

// En mapp som släpps på kontot hamnar på översta nivån.
function acceptsFolder(account) {
  const dragged = store.draggedFolder
  return !!dragged && dragged.accountId === account.id && store.parentPath(dragged.folder) !== ''
}

function onDragOver(event, account) {
  if (!event.dataTransfer.types.includes(FOLDER_TYPE) || !acceptsFolder(account)) return
  event.preventDefault()
  event.dataTransfer.dropEffect = 'move'
  dropAccount.value = account.id
}

function onDrop(event, account) {
  dropAccount.value = null
  if (!event.dataTransfer.types.includes(FOLDER_TYPE)) return
  event.preventDefault()
  const dragged = store.draggedFolder
  store.draggedFolder = null
  if (dragged && dragged.accountId === account.id) store.moveFolder(account.id, dragged.folder, '')
}

const numberFmt = new Intl.NumberFormat('sv-SE')

// Olästa i alla konton, räknat som vyn visar dem: utan papperskorg, skräppost, skickat, utkast och virtuella mappar.
const unreadTotal = computed(() =>
  store.accounts.reduce(
    (sum, a) =>
      sum +
      (store.folders[a.id] || [])
        .filter((f) => f.selectable && !['trash', 'junk', 'sent', 'drafts', 'all', 'flagged', 'important'].includes(f.specialUse))
        .reduce((n, f) => n + (f.unread || 0), 0),
    0,
  ),
)

// En rad under kontot när mejlservern inte går att nå, eller medan den första synken till mejlcachen pågår.
function cacheNote(accountId) {
  const s = store.cacheStatus[accountId]
  if (!s) return null
  if (s.state === 'offline') {
    const since = s.lastSyncAt ? ` från ${formatListDate(s.lastSyncAt)}` : ''
    return { offline: true, text: `Ingen kontakt med mejlservern. Visar sparade mejl${since}.`, title: s.error }
  }
  if (s.complete || !s.messages) return null
  if (s.foldersComplete < s.folders) {
    return { text: `Synkar mappar: ${s.foldersComplete} av ${s.folders} klara`, title: 'Mejlens rubriker sparas i cachen. Mappar som inte är klara läses direkt från mejlservern.' }
  }
  return {
    text: `Sparar mejltext: ${numberFmt.format(s.bodies)} av ${numberFmt.format(s.messages)}`,
    title: 'Meddelandenas text sparas i cachen, nyast först. Det som inte är sparat än läses direkt från mejlservern.',
  }
}

function accountMenu(event, account) {
  store.contextMenu = {
    x: event.clientX,
    y: event.clientY,
    items: [
      { label: 'Ny mapp…', action: () => store.createFolder(account.id, '', '') },
      { label: 'Uppdatera mappar', action: () => store.loadFolders(account.id) },
      { label: 'Kontoinställningar…', action: () => (store.accountDialog = { ...account }) },
      { label: 'Backup…', action: () => (store.backupDialog = { accountId: account.id }) },
    ],
  }
}
</script>

<template>
  <nav class="pane">
    <div
      v-if="store.accounts.length"
      class="view-row"
      :class="{ active: store.current.view === 'unread' }"
      title="Olästa meddelanden i alla konton och mappar, utom papperskorg, skräppost, skickat och utkast"
      @click="store.selectView('unread')"
    >
      <Icon name="unread" :size="15" />
      <span class="view-name">Olästa</span>
      <span v-if="unreadTotal" class="view-count">{{ numberFmt.format(unreadTotal) }}</span>
    </div>
    <div v-for="account in store.accounts" :key="account.id" class="account">
      <div
        class="account-row"
        :class="{ drop: dropAccount === account.id }"
        @dragover="onDragOver($event, account)"
        @dragleave="dropAccount = null"
        @drop="onDrop($event, account)"
        @click="store.toggleExpanded(`acc:${account.id}`)"
        @contextmenu.prevent="accountMenu($event, account)"
      >
        <Icon :name="store.expanded.has(`acc:${account.id}`) ? 'chevronDown' : 'chevronRight'" :size="14" />
        <Icon name="mail" :size="15" />
        <span class="account-name" :title="account.email">{{ account.name || account.email }}</span>
        <span v-if="cacheNote(account.id)?.offline" class="offline" :title="cacheNote(account.id).title">
          <Icon name="cloudOff" :size="14" />
        </span>
      </div>
      <p v-if="cacheNote(account.id)" class="cache-note" :class="{ warn: cacheNote(account.id).offline }" :title="cacheNote(account.id).title">
        {{ cacheNote(account.id).text }}
      </p>
      <template v-if="store.expanded.has(`acc:${account.id}`)">
        <p v-if="store.folderErrors[account.id]" class="error-text folder-error">
          {{ store.folderErrors[account.id] }}
        </p>
        <FolderNode
          v-for="node in buildFolderTree(store.folders[account.id] || [])"
          :key="node.path"
          :node="node"
          :account-id="account.id"
          :depth="1"
        />
      </template>
    </div>

    <div v-if="!store.accounts.length" class="empty">
      <p class="muted">Inga konton ännu.</p>
      <button class="btn primary" @click="store.accountDialog = {}">Lägg till konto</button>
    </div>
  </nav>
</template>

<style scoped>
.account-row.drop {
  outline: 2px dashed var(--accent);
  outline-offset: -2px;
}

.pane {
  padding: 6px 0;
  user-select: none;
}

.view-row {
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 6px 10px 6px 30px;
  cursor: pointer;
  font-weight: 600;
}

.view-row:hover {
  background: var(--hover);
}

.view-row.active {
  background: var(--selection);
  color: var(--selection-text);
}

.view-name {
  flex: 1;
}

.view-count {
  font-size: 11px;
  padding: 0 7px;
  border-radius: 9px;
  background: var(--accent);
  color: var(--accent-text);
}

.view-row.active .view-count {
  background: var(--selection-text);
  color: var(--selection);
}

.account-row {
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 6px 10px;
  font-weight: 600;
  cursor: pointer;
}

.account-row:hover {
  background: var(--hover);
}

.account-name {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.offline {
  margin-left: auto;
  color: var(--danger);
  display: inline-flex;
}

.cache-note {
  margin: 0 12px 4px 30px;
  font-size: 11px;
  color: var(--text-muted);
}

.cache-note.warn {
  color: var(--danger);
}

.folder-error {
  margin: 4px 12px 8px 30px;
  font-size: 12px;
}

.empty {
  padding: 16px;
  display: flex;
  flex-direction: column;
  gap: 8px;
  align-items: flex-start;
}
</style>
