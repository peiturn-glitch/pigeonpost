<script setup>
import { computed, ref } from 'vue'
import { useMailStore } from '../store'
import { folderLabel } from '../utils'
import Icon from './Icon.vue'

const props = defineProps({
  node: { type: Object, required: true },
  accountId: { type: Number, required: true },
  depth: { type: Number, default: 1 },
})

const store = useMailStore()
const dropActive = ref(false)

const key = computed(() => `${props.accountId}:${props.node.path}`)
const isOpen = computed(() => store.expanded.has(key.value))
const isCurrent = computed(() => store.current.accountId === props.accountId && store.current.folder === props.node.path)
const iconName = computed(() => {
  const use = props.node.specialUse
  return ['inbox', 'sent', 'drafts', 'trash', 'junk', 'archive', 'all', 'flagged'].includes(use) ? use : 'folder'
})

function select() {
  if (props.node.selectable) store.selectFolder(props.accountId, props.node.path)
  else if (props.node.children.length) store.toggleExpanded(key.value)
}

function openMenu(event) {
  const items = [{ label: 'Ny undermapp…', action: () => store.createFolder(props.accountId, props.node.path, props.node.delimiter) }]
  if (!props.node.specialUse && props.node.selectable) {
    items.push({ label: 'Byt namn…', action: () => store.renameFolder(props.accountId, props.node) })
    items.push({ label: 'Ta bort mapp', danger: true, action: () => store.deleteFolder(props.accountId, props.node) })
  }
  if (props.node.selectable && props.node.unread > 0 && isCurrent.value) {
    items.unshift({
      label: 'Markera alla som lästa',
      action: () => store.setFlag('seen', true, store.messages.filter((m) => !m.seen).map((m) => m.uid)),
    })
  }
  store.contextMenu = { x: event.clientX, y: event.clientY, items }
}

function onDragOver(event) {
  if (!props.node.selectable || !event.dataTransfer.types.includes('application/x-tbk-messages')) return
  event.preventDefault()
  event.dataTransfer.dropEffect = 'move'
  dropActive.value = true
}

function onDrop(event) {
  dropActive.value = false
  const raw = event.dataTransfer.getData('application/x-tbk-messages')
  if (!raw) return
  event.preventDefault()
  const data = JSON.parse(raw)
  if (data.accountId !== props.accountId) {
    store.notify('Flytt mellan konton stöds inte ännu', 'error')
    return
  }
  store.moveMessages(props.node.path, data.uids, data.folder)
}
</script>

<template>
  <div>
    <div
      class="row"
      :class="{ current: isCurrent, drop: dropActive, disabled: !node.selectable }"
      :style="{ paddingLeft: depth * 14 + 'px' }"
      @click="select"
      @contextmenu.prevent="openMenu"
      @dragover="onDragOver"
      @dragleave="dropActive = false"
      @drop="onDrop"
    >
      <span class="twisty" @click.stop="store.toggleExpanded(key)">
        <Icon v-if="node.children.length" :name="isOpen ? 'chevronDown' : 'chevronRight'" :size="12" />
      </span>
      <Icon :name="iconName" :size="15" class="folder-icon" />
      <span class="name" :class="{ unread: node.unread > 0 }">{{ folderLabel(node) }}</span>
      <span v-if="node.unread > 0" class="badge">{{ node.unread }}</span>
    </div>
    <template v-if="isOpen">
      <FolderNode
        v-for="child in node.children"
        :key="child.path"
        :node="child"
        :account-id="accountId"
        :depth="depth + 1"
      />
    </template>
  </div>
</template>

<style scoped>
.row {
  display: flex;
  align-items: center;
  gap: 5px;
  padding: 4px 10px 4px 0;
  cursor: pointer;
  border-radius: 0;
}

.row:hover {
  background: var(--hover);
}

.row.current {
  background: var(--selection);
  color: var(--selection-text);
}

.row.drop {
  outline: 2px dashed var(--accent);
  outline-offset: -2px;
}

.row.disabled .name {
  font-style: italic;
}

.twisty {
  width: 12px;
  display: inline-flex;
}

.folder-icon {
  opacity: 0.85;
}

.name {
  flex: 1;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.name.unread {
  font-weight: 700;
}

.badge {
  font-size: 11px;
  font-weight: 600;
  padding: 0 6px;
  border-radius: 9px;
  background: var(--accent);
  color: var(--accent-text);
}

.row.current .badge {
  background: var(--selection-text);
  color: var(--selection);
}
</style>
