<script setup>
import { computed, ref, watch } from 'vue'
import { t } from '../i18n'
import { useMailStore } from '../store'
import { SORT_KEYS, displayName, formatListDate, specialFolderNames } from '../utils'
import Icon from './Icon.vue'

const store = useMailStore()
const scroller = ref(null)
const searchText = ref('')
let searchTimer = null

// Skickat och Utkast visar mottagare i stället för avsändare, som i Thunderbird.
const showRecipient = computed(() => ['sent', 'drafts'].includes(store.currentFolderInfo?.specialUse))

const title = computed(() => {
  if (store.current.view === 'unread') return t('Unread in all folders')
  const f = store.currentFolderInfo
  if (!f) return store.current.folder || ''
  return f.specialUse && specialFolderNames[f.specialUse] ? t(specialFolderNames[f.specialUse]) : f.name
})

const countText = computed(() => (store.total === 1 ? t('1 message') : t('{n} messages', { n: store.total })))

watch(
  () => [store.current.folder, store.current.view],
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

// Sökningen i alla mappar tar med sig det som står i sökfältet.
function openSearch() {
  const text = searchText.value.trim()
  if (text && text !== store.search.form.q) {
    Object.assign(store.search, { question: '', explanation: '', form: { ...store.emptySearchForm(), q: text }, results: [], total: 0, page: 0, searched: false })
  }
  store.searchDialog = true
}

function onSearchEnter(event) {
  if (event.shiftKey) openSearch()
}

function toggleFilter(name) {
  store.filter[name] = !store.filter[name]
  store.loadMessages()
}

// Kolumnen Korrespondent sorterar på det den visar: mottagare i Skickat och Utkast, annars avsändare.
const correspondentKey = computed(() => (showRecipient.value ? 'to' : 'from'))
const sortIcon = (key) => (store.currentSort.key === key ? (store.currentSort.desc ? 'arrowDown' : 'arrowUp') : null)

// Ett klick på en kolumnrubrik sorterar på den; ett klick till vänder ordningen. Datum och storlek börjar med
// störst först, text med A först.
function sortBy(key) {
  const s = store.currentSort
  store.setSort(key, s.key === key ? !s.desc : ['date', 'size'].includes(key))
}

// Väljaren i filterraden, för den smala listan där kolumnrubrikerna inte syns.
const sortOptions = computed(() => SORT_KEYS.filter((s) => (s.key === 'to' ? showRecipient.value : s.key !== 'from' || !showRecipient.value)))
function onSortSelect(event) {
  const key = event.target.value
  store.setSort(key, key === store.currentSort.key ? store.currentSort.desc : ['date', 'size'].includes(key))
}

function correspondent(m) {
  const list = showRecipient.value ? m.to : m.from
  return list.map(displayName).join(', ') || t('(unknown)')
}

// I en samlad vy visas var varje mejl ligger.
function folderTag(m) {
  const f = store.folderInfo(m.accountId, m.folder)
  // Hela sökvägen, så att undermappar med samma namn går att skilja åt.
  const name = f?.specialUse && specialFolderNames[f.specialUse] ? t(specialFolderNames[f.specialUse]) : m.folder
  const account = store.accounts.length > 1 ? store.accounts.find((a) => a.id === m.accountId) : null
  return account ? `${name} · ${account.name || account.email}` : name
}

function onRowClick(event, m) {
  store.selectMessage(m.key, { ctrl: event.ctrlKey || event.metaKey, shift: event.shiftKey })
}

function onDoubleClick(m) {
  if (store.folderInfo(m.accountId, m.folder)?.specialUse === 'drafts' && store.openMessage?.key === m.key) {
    store.startCompose('editDraft')
  } else if (store.layout.preview === 'hidden') {
    // Utan läsruta öppnas meddelandet i ett eget fönster.
    store.readerOpen = true
  }
}

function onDragStart(event, m) {
  if (!store.selection.has(m.key)) store.selectMessage(m.key)
  const keys = [...store.selection]
  event.dataTransfer.effectAllowed = 'move'
  event.dataTransfer.setData('application/x-brevduva-messages', JSON.stringify({ keys }))
  event.dataTransfer.setData('text/plain', keys.length === 1 ? t('1 message') : t('{n} messages', { n: keys.length }))
}

function onScroll() {
  const el = scroller.value
  if (!el || store.loadingMessages || !store.hasMore) return
  if (el.scrollTop + el.clientHeight > el.scrollHeight - 200) store.loadMessages({ append: true })
}

function rowMenu(event, m) {
  if (!store.selection.has(m.key)) store.selectMessage(m.key)
  const selected = store.selectedMessages
  const anyUnread = selected.some((x) => !x.seen)
  const anyUnflagged = selected.some((x) => !x.flagged)
  store.contextMenu = {
    x: event.clientX,
    y: event.clientY,
    items: [
      { label: t('Reply'), action: () => store.startCompose('reply') },
      { label: t('Forward'), action: () => store.startCompose('forward') },
      { label: anyUnread ? t('Mark as read') : t('Mark as unread'), action: () => store.setFlag('seen', anyUnread) },
      { label: anyUnflagged ? t('Star') : t('Remove star'), action: () => store.setFlag('flagged', anyUnflagged) },
      // AI-sorteringen tar en mapp i taget, så den erbjuds inte i en samlad vy.
      ...(store.current.view
        ? [{ label: t('Show in folder'), action: () => store.openSearchResult(m) }]
        : [
            {
              label: t('Sort with AI…'),
              action: () =>
                (store.aiDialog = { accountId: store.current.accountId, folder: store.current.folder, uids: selected.map((x) => x.uid) }),
            },
          ]),
      { label: t('Move to archive'), action: () => store.archive() },
      { label: t('Mark as junk'), action: () => store.markJunk() },
      { label: t('Delete'), danger: true, action: () => store.deleteMessages() },
    ],
  }
}
</script>

<template>
  <div class="message-list">
    <div class="filter-bar">
      <strong class="title" :title="store.current.folder">{{ title }}</strong>
      <span class="muted count">{{ countText }}</span>
      <div class="spacer" />
      <button v-if="!store.current.view" class="btn ghost chip" :class="{ active: store.filter.unread }" :title="t('Show unread only')" @click="toggleFilter('unread')">
        <Icon name="unread" :size="12" /> {{ t('Unread') }}
      </button>
      <button class="btn ghost chip" :class="{ active: store.filter.flagged }" :title="t('Show starred only')" @click="toggleFilter('flagged')">
        <Icon name="star" :size="12" /> {{ t('Starred') }}
      </button>
      <label class="search">
        <Icon name="search" :size="14" />
        <input v-model="searchText" type="search" :placeholder="store.current.view ? t('Search unread…') : t('Search this folder…')" @input="onSearchInput" @keydown.enter="onSearchEnter" />
      </label>
      <button class="btn ghost chip" :title="t('Search all folders and accounts, with filters and AI support (Shift+Enter in the search box)')" @click="openSearch">
        <Icon name="search" :size="12" /> {{ t('All folders') }}
      </button>
      <span class="sort-control" :title="t('Sort order')">
        <select :value="store.currentSort.key" :aria-label="t('Sort by')" @change="onSortSelect">
          <option v-for="s in sortOptions" :key="s.key" :value="s.key">{{ t(s.label) }}</option>
        </select>
        <button
          class="btn ghost chip"
          :title="store.currentSort.desc ? t('Descending (newest, largest or Z first). Click for ascending.') : t('Ascending (oldest, smallest or A first). Click for descending.')"
          @click="store.setSort(store.currentSort.key, !store.currentSort.desc)"
        >
          <Icon :name="store.currentSort.desc ? 'arrowDown' : 'arrowUp'" :size="12" />
        </button>
      </span>
    </div>

    <div class="header-row">
      <span class="col-star"><Icon name="star" :size="12" /></span>
      <span class="col-attach"><Icon name="paperclip" :size="12" /></span>
      <button class="col-subject sortable" :class="{ sorted: store.currentSort.key === 'subject' }" :title="t('Sort by subject')" @click="sortBy('subject')">
        {{ t('Subject') }} <Icon v-if="sortIcon('subject')" :name="sortIcon('subject')" :size="11" />
      </button>
      <button class="col-from sortable" :class="{ sorted: store.currentSort.key === correspondentKey }" :title="showRecipient ? t('Sort by recipient') : t('Sort by sender')" @click="sortBy(correspondentKey)">
        {{ showRecipient ? t('Recipient') : t('Correspondent') }} <Icon v-if="sortIcon(correspondentKey)" :name="sortIcon(correspondentKey)" :size="11" />
      </button>
      <button class="col-date sortable" :class="{ sorted: store.currentSort.key === 'date' }" :title="t('Sort by date')" @click="sortBy('date')">
        <Icon v-if="sortIcon('date')" :name="sortIcon('date')" :size="11" /> {{ t('Date') }}
      </button>
    </div>

    <div ref="scroller" class="rows" tabindex="0" @scroll="onScroll">
      <p v-if="store.listError" class="notice error-text">{{ store.listError }}</p>
      <p v-else-if="!store.current.accountId && !store.current.view" class="notice muted">{{ t('Select a folder.') }}</p>
      <p v-else-if="!store.loadingMessages && !store.messages.length" class="notice muted">
        {{ store.current.view ? t('No unread messages.') : t('No messages.') }}
      </p>

      <div
        v-for="m in store.messages"
        :key="m.key"
        class="row"
        :class="{ selected: store.selection.has(m.key), unread: !m.seen }"
        draggable="true"
        @click="onRowClick($event, m)"
        @dblclick="onDoubleClick(m)"
        @contextmenu.prevent="rowMenu($event, m)"
        @dragstart="onDragStart($event, m)"
      >
        <span class="col-star" @click.stop="store.setFlag('flagged', !m.flagged, [m.key])">
          <Icon name="star" :size="13" class="star" :class="{ on: m.flagged }" />
        </span>
        <span class="col-attach"><Icon v-if="m.hasAttachments" name="paperclip" :size="13" /></span>
        <span class="col-subject">
          <Icon v-if="m.answered" name="reply" :size="12" class="answered" />
          {{ m.subject || t('(no subject)') }}
        </span>
        <span class="col-from">
          {{ correspondent(m) }}
          <span v-if="store.current.view" class="folder-tag" :title="m.folder">{{ folderTag(m) }}</span>
        </span>
        <span class="col-date">{{ formatListDate(m.date) }}</span>
      </div>

      <p v-if="store.loadingMessages" class="notice muted">{{ t('Loading messages…') }}</p>
    </div>
  </div>
</template>

<style scoped>
.message-list {
  display: flex;
  flex-direction: column;
  height: 100%;
  background: var(--pane);
  container-type: inline-size;
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

.sortable {
  border: 0;
  background: none;
  padding: 0;
  cursor: pointer;
  color: inherit;
  text-align: left;
  display: flex;
  align-items: center;
  gap: 4px;
  min-width: 0;
}

.sortable:hover,
.sortable.sorted {
  color: var(--text);
}

.col-date.sortable {
  justify-content: flex-end;
}

.sort-control {
  display: none;
  align-items: center;
  gap: 4px;
}

.sort-control select {
  font-size: 12px;
  padding: 2px 4px;
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

.folder-tag {
  margin-left: 6px;
  padding: 0 6px;
  border-radius: 8px;
  background: var(--pane-alt);
  color: var(--text-muted);
  font-size: 11px;
  font-weight: 400;
}

.row.selected .folder-tag {
  background: transparent;
  color: inherit;
  opacity: 0.85;
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

/* Smal lista (t.ex. läsrutan bredvid): två rader per meddelande, som korten i Thunderbird. */
@container (max-width: 560px) {
  .header-row {
    display: none;
  }

  .row {
    height: auto;
    grid-template-columns: 20px minmax(0, 1fr) auto;
    grid-template-areas:
      'star from date'
      'attach subject subject';
    row-gap: 1px;
    padding-top: 5px;
    padding-bottom: 5px;
    border-bottom-color: var(--border);
  }

  .row .col-star {
    grid-area: star;
  }

  .row .col-attach {
    grid-area: attach;
  }

  .row .col-from {
    grid-area: from;
  }

  .row .col-date {
    grid-area: date;
    font-size: 12px;
  }

  .row .col-subject {
    grid-area: subject;
    font-size: 12px;
    color: var(--text-muted);
  }

  .row.selected .col-subject {
    color: inherit;
  }

  .search {
    min-width: 0;
    flex: 1 1 100%;
  }

  /* Utan kolumnrubriker väljs sorteringen i filterraden. */
  .sort-control {
    display: flex;
  }
}
</style>
