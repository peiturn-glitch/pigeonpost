<script setup>
import { useMailStore } from '../store'
import { buildFolderTree } from '../utils'
import FolderNode from './FolderNode.vue'
import Icon from './Icon.vue'

const store = useMailStore()

function accountMenu(event, account) {
  store.contextMenu = {
    x: event.clientX,
    y: event.clientY,
    items: [
      { label: 'Ny mapp…', action: () => store.createFolder(account.id, '', '') },
      { label: 'Uppdatera mappar', action: () => store.loadFolders(account.id) },
      { label: 'Kontoinställningar…', action: () => (store.accountDialog = { ...account }) },
    ],
  }
}
</script>

<template>
  <nav class="pane">
    <div v-for="account in store.accounts" :key="account.id" class="account">
      <div
        class="account-row"
        @click="store.toggleExpanded(`acc:${account.id}`)"
        @contextmenu.prevent="accountMenu($event, account)"
      >
        <Icon :name="store.expanded.has(`acc:${account.id}`) ? 'chevronDown' : 'chevronRight'" :size="14" />
        <Icon name="mail" :size="15" />
        <span class="account-name" :title="account.email">{{ account.name || account.email }}</span>
      </div>
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
.pane {
  padding: 6px 0;
  user-select: none;
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
