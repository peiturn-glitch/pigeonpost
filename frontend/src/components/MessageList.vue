<script setup>
import { computed, ref, watch } from 'vue'
import { useMailStore } from '../store'
import { displayName, folderLabel, formatListDate } from '../utils'
import Icon from './Icon.vue'

const store = useMailStore()
const scroller = ref(null)
const searchText = ref('')
let searchTimer = null

// Skickat och Utkast visar mottagare i stället för avsändare, som i Thunderbird.
const showRecipient = computed(() => ['sent', 'drafts'].includes(store.currentFolderInfo?.specialUse))

const title = computed(() => {
  const f = store.currentFolderInfo
  return f ? folderLabel(f) : store.current.folder || ''
})

watch(
  () => store.current.folder,
  () => {
    searchText.value = ''
    if (scroller.value) scroller.value.scrollTop = 0
  },
)

function onSearchInput() {
  clearTimeout(searchTimer)
  searchTimer = setTimeout(() => {
    store.filter.q = searchText.value.trim()
    store.loadMessages()
  }, 400)
}

function toggleFilter(name) {
  store.filter[name] = !store.filter[name]
  store.loadMessages()
}

function correspondent(m) {
  const list = showRecipient.value ? m.to : m.from
  return list.map(displayName).join(', ') || '(okänd)'
}

function onRowClick(event, m) {
  store.selectMessage(m.uid, { ctrl: event.ctrlKey || event.metaKey, shift: event.shiftKey })
}

function onDoubleClick(m) {
  if (store.currentFolderInfo?.specialUse === 'drafts' && store.openMessage?.uid === m.uid) {
    store.startCompose('editDraft')
  }
}

function onDragStart(event, m) {
  if (!store.selection.has(m.uid)) store.selectMessage(m.uid)
  const uids = [...store.selection]
  event.dataTransfer.effectAllowed = 'move'
  event.dataTransfer.setData(
    'application/x-brevduva-messages',
    JSON.stringify({ accountId: store.current.accountId, folder: store.current.folder, uids }),
  )
  event.dataTransfer.setData('text/plain', `${uids.length} meddelande(n)`)
}

function onScroll() {
  const el = scroller.value
  if (!el || store.loadingMessages || !store.hasMore) return
  if (el.scrollTop + el.clientHeight > el.scrollHeight - 200) store.loadMessages({ append: true })
}

function rowMenu(event, m) {
  if (!store.selection.has(m.uid)) store.selectMessage(m.uid)
  const selected = store.selectedMessages
  const anyUnread = selected.some((x) => !x.seen)
  const anyUnflagged = selected.some((x) => !x.flagged)
  store.contextMenu = {
    x: event.clientX,
    y: event.clientY,
    items: [
      { label: 'Svara', action: () => store.startCompose('reply') },
      { label: 'Vidarebefordra', action: () => store.startCompose('forward') },
      { label: anyUnread ? 'Markera som läst' : 'Markera som oläst', action: () => store.setFlag('seen', anyUnread) },
      { label: anyUnflagged ? 'Stjärnmärk' : 'Ta bort stjärna', action: () => store.setFlag('flagged', anyUnflagged) },
      { label: 'Arkivera', action: () => store.archive() },
      { label: 'Skräppost', action: () => store.markJunk() },
      { label: 'Ta bort', danger: true, action: () => store.deleteMessages() },
    ],
  }
}
</script>

<template>
  <div class="message-list">
    <div class="filter-bar">
      <strong class="title" :title="store.current.folder">{{ title }}</strong>
      <span class="muted count">{{ store.total }} meddelanden</span>
      <div class="spacer" />
      <button class="btn ghost chip" :class="{ active: store.filter.unread }" title="Visa endast olästa" @click="toggleFilter('unread')">
        <Icon name="unread" :size="12" /> Olästa
      </button>
      <button class="btn ghost chip" :class="{ active: store.filter.flagged }" title="Visa endast stjärnmärkta" @click="toggleFilter('flagged')">
        <Icon name="star" :size="12" /> Stjärnmärkta
      </button>
      <label class="search">
        <Icon name="search" :size="14" />
        <input v-model="searchText" type="search" placeholder="Filtrera meddelanden…" @input="onSearchInput" />
      </label>
    </div>

    <div class="header-row">
      <span class="col-star"><Icon name="star" :size="12" /></span>
      <span class="col-attach"><Icon name="paperclip" :size="12" /></span>
      <span class="col-subject">Ämne</span>
      <span class="col-from">{{ showRecipient ? 'Mottagare' : 'Korrespondent' }}</span>
      <span class="col-date">Datum</span>
    </div>

    <div ref="scroller" class="rows" tabindex="0" @scroll="onScroll">
      <p v-if="store.listError" class="notice error-text">{{ store.listError }}</p>
      <p v-else-if="!store.current.accountId" class="notice muted">Välj en mapp.</p>
      <p v-else-if="!store.loadingMessages && !store.messages.length" class="notice muted">Inga meddelanden.</p>

      <div
        v-for="m in store.messages"
        :key="m.uid"
        class="row"
        :class="{ selected: store.selection.has(m.uid), unread: !m.seen }"
        draggable="true"
        @click="onRowClick($event, m)"
        @dblclick="onDoubleClick(m)"
        @contextmenu.prevent="rowMenu($event, m)"
        @dragstart="onDragStart($event, m)"
      >
        <span class="col-star" @click.stop="store.setFlag('flagged', !m.flagged, [m.uid])">
          <Icon name="star" :size="13" class="star" :class="{ on: m.flagged }" />
        </span>
        <span class="col-attach"><Icon v-if="m.hasAttachments" name="paperclip" :size="13" /></span>
        <span class="col-subject">
          <Icon v-if="m.answered" name="reply" :size="12" class="answered" />
          {{ m.subject || '(inget ämne)' }}
        </span>
        <span class="col-from">{{ correspondent(m) }}</span>
        <span class="col-date">{{ formatListDate(m.date) }}</span>
      </div>

      <p v-if="store.loadingMessages" class="notice muted">Hämtar meddelanden…</p>
    </div>
  </div>
</template>

<style scoped>
.message-list {
  display: flex;
  flex-direction: column;
  height: 100%;
  background: var(--pane);
}

.filter-bar {
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 6px 8px;
  border-bottom: 1px solid var(--border);
  flex-wrap: wrap;
}

.title {
  font-size: 14px;
}

.count {
  font-size: 12px;
}

.spacer {
  flex: 1;
}

.chip {
  padding: 3px 8px;
  font-size: 12px;
}

.chip.active {
  background: var(--accent);
  color: var(--accent-text);
}

.search {
  display: flex;
  align-items: center;
  gap: 6px;
  border: 1px solid var(--border);
  border-radius: var(--radius);
  padding: 3px 8px;
  background: var(--pane);
  min-width: 200px;
}

.search input {
  border: 0;
  outline: 0;
  background: transparent;
  width: 100%;
}

.header-row,
.row {
  display: grid;
  grid-template-columns: 24px 20px minmax(0, 3fr) minmax(0, 2fr) 130px;
  align-items: center;
  gap: 6px;
  padding: 0 10px 0 6px;
}

.header-row {
  font-size: 12px;
  color: var(--text-muted);
  border-bottom: 1px solid var(--border);
  padding-top: 4px;
  padding-bottom: 4px;
  background: var(--pane-alt);
}

.rows {
  flex: 1;
  overflow-y: auto;
  outline: none;
}

.row {
  height: 26px;
  cursor: default;
  user-select: none;
  border-bottom: 1px solid transparent;
}

.row:hover {
  background: var(--hover);
}

.row.selected {
  background: var(--selection);
  color: var(--selection-text);
}

.rows:not(:focus-within) .row.selected {
  background: var(--selection-inactive);
  color: var(--text);
}

.row.unread {
  font-weight: 700;
}

.col-subject,
.col-from {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.col-date {
  text-align: right;
  font-variant-numeric: tabular-nums;
  white-space: nowrap;
}

.col-star,
.col-attach {
  display: flex;
  justify-content: center;
}

.star {
  color: var(--text-muted);
  opacity: 0.35;
  cursor: pointer;
}

.row.selected .star {
  color: inherit;
  opacity: 0.6;
}

.star.on {
  color: var(--star);
  fill: var(--star);
  opacity: 1;
}

.answered {
  opacity: 0.6;
  margin-right: 2px;
}

.notice {
  padding: 16px;
  margin: 0;
}
</style>
