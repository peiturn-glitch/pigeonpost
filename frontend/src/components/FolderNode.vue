<script setup>
import { computed, ref } from 'vue'
import { t } from '../i18n'
import { useMailStore } from '../store'
import { isInsideFolder, specialFolderNames } from '../utils'
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
const label = computed(() => {
  const use = props.node.specialUse
  return use && specialFolderNames[use] ? t(specialFolderNames[use]) : props.node.name
})
const iconName = computed(() => {
  const use = props.node.specialUse
  return ['inbox', 'sent', 'drafts', 'trash', 'junk', 'archive', 'all', 'flagged'].includes(use) ? use : 'folder'
})

function select() {
  if (props.node.selectable) store.selectFolder(props.accountId, props.node.path)
  else if (props.node.children.length) store.toggleExpanded(key.value)
}

function openMenu(event) {
  const items = [{ label: t('New subfolder…'), action: () => store.createFolder(props.accountId, props.node.path, props.node.delimiter) }]
  if (!props.node.specialUse && props.node.selectable) {
    items.push({ label: t('Rename…'), action: () => store.renameFolder(props.accountId, props.node) })
    items.push({ label: t('Move folder…'), action: () => (store.folderMoveDialog = { accountId: props.accountId, folder: props.node }) })
    items.push({ label: t('Delete folder'), danger: true, action: () => store.deleteFolder(props.accountId, props.node) })
  }
  if (props.node.selectable && !['drafts', 'all', 'flagged', 'important'].includes(props.node.specialUse)) {
    items.push({ label: t('Sort with AI…'), action: () => (store.aiDialog = { accountId: props.accountId, folder: props.node.path }) })
  }
  if (props.node.selectable && props.node.unread > 0 && isCurrent.value) {
    items.unshift({
      label: t('Mark all as read'),
      action: () => store.setFlag('seen', true, store.messages.filter((m) => !m.seen).map((m) => m.key)),
    })
  }
  store.contextMenu = { x: event.clientX, y: event.clientY, items }
}

const FOLDER_TYPE = 'application/x-brevduva-folder'
const canDrag = computed(() => store.canMoveFolder(props.node))

function onDragStart(event) {
  event.dataTransfer.effectAllowed = 'move'
  event.dataTransfer.setData(FOLDER_TYPE, JSON.stringify({ accountId: props.accountId, path: props.node.path }))
  event.dataTransfer.setData('text/plain', props.node.name)
  store.draggedFolder = { accountId: props.accountId, folder: props.node }
}

// Innehållet i en dragning går inte att läsa under dragover, så mappen som dras finns i store.draggedFolder.
function acceptsFolder() {
  const dragged = store.draggedFolder
  if (!dragged || dragged.accountId !== props.accountId) return false
  if (['all', 'flagged', 'important'].includes(props.node.specialUse)) return false
  return !isInsideFolder(props.node.path, dragged.folder) && store.parentPath(dragged.folder) !== props.node.path
}

function onDragOver(event) {
  if (event.dataTransfer.types.includes(FOLDER_TYPE)) {
    if (!acceptsFolder()) return
    event.preventDefault()
    event.dataTransfer.dropEffect = 'move'
    dropActive.value = true
    return
  }
  if (!props.node.selectable || !event.dataTransfer.types.includes('application/x-brevduva-messages')) return
  event.preventDefault()
  event.dataTransfer.dropEffect = 'move'
  dropActive.value = true
}

function onDrop(event) {
  dropActive.value = false
  if (event.dataTransfer.types.includes(FOLDER_TYPE)) {
    event.preventDefault()
    const dragged = store.draggedFolder
    store.draggedFolder = null
    if (dragged && dragged.accountId === props.accountId) store.moveFolder(dragged.accountId, dragged.folder, props.node.path)
    return
  }
  const raw = event.dataTransfer.getData('application/x-brevduva-messages')
  if (!raw) return
  event.preventDefault()
  // Mejl från andra konton (möjligt i en samlad vy) hoppas över av moveMessages, som också säger till.
  store.moveMessages(props.node.path, JSON.parse(raw).keys, props.accountId)
}
</script>

<template>
  <div>
    <div
      class="row"
      :class="{ current: isCurrent, drop: dropActive, disabled: !node.selectable }"
      :style="{ paddingLeft: depth * 14 + 'px' }"
      :draggable="canDrag"
      @dragstart="onDragStart"
      @dragend="store.draggedFolder = null"
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
      <span class="name" :class="{ unread: node.unread > 0 }">{{ label }}</span>
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
