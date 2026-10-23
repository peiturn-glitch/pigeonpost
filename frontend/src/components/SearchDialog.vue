<script setup>
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { api } from '../api'
import { useMailStore } from '../store'
import { SORT_KEYS, displayName, formatListDate, specialFolderNames } from '../utils'
import Icon from './Icon.vue'

const store = useMailStore()
// Filtren och träffarna ligger i store.search, så att de finns kvar när dialogen öppnas igen.
const search = store.search
const form = computed(() => search.form)

const PAGE_SIZE = 50
const aiConfigured = ref(null) // null = inte kontrollerat än
const interpreting = ref(false)
const searching = ref(false)
const error = ref('')
const questionInput = ref(null)
let searchRequest = 0
const sort = store.sort.search

function setSort(key, desc) {
  Object.assign(sort, { key, desc })
  if (search.searched) runSearch()
}

const sortLabel = computed(() => {
  const name = SORT_KEYS.find((s) => s.key === sort.key)?.label.toLowerCase() || sort.key
  if (sort.key === 'date') return sort.desc ? 'nyast först' : 'äldst först'
  if (sort.key === 'size') return sort.desc ? 'störst först' : 'minst först'
  return `${name} ${sort.desc ? 'Ö–A' : 'A–Ö'}`
})

const accountFolders = computed(() =>
  form.value.accountId ? (store.folders[form.value.accountId] || []).filter((f) => f.selectable && !['all', 'flagged', 'important'].includes(f.specialUse)) : [],
)

function folderName(f) {
  return specialFolderNames[f.specialUse] || f.path
}

function hitFolder(hit) {
  const f = store.folderInfo(hit.accountId, hit.folder)
  return f ? folderName(f) : hit.folder
}

function accountName(id) {
  return store.accounts.find((a) => a.id === id)?.email || ''
}

// Skickat och Utkast visar mottagare i stället för avsändare, som i meddelandelistan.
function correspondent(hit) {
  const use = store.folderInfo(hit.accountId, hit.folder)?.specialUse
  const list = ['sent', 'drafts'].includes(use) ? hit.to : hit.from
  return list.map(displayName).join(', ') || '(okänd)'
}

watch(
  () => form.value.accountId,
  async (id) => {
    if (id && !store.folders[id]) await store.loadFolders(id)
    if (!accountFolders.value.some((f) => f.path === form.value.folder)) form.value.folder = ''
  },
)

// Datumfälten gäller hela dagar i webbläsarens tidszon; servern räknar i unixtid.
function dayStart(text, addDays = 0) {
  const m = /^(\d{4})-(\d{2})-(\d{2})$/.exec(text || '')
  return m ? Math.floor(new Date(+m[1], +m[2] - 1, +m[3] + addDays).getTime() / 1000) : 0
}

function today() {
  const d = new Date()
  const pad = (n) => String(n).padStart(2, '0')
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}`
}

async function runSearch({ append = false } = {}) {
  const token = ++searchRequest
  const f = form.value
  searching.value = true
  error.value = ''
  try {
    const page = append ? search.page + 1 : 0
    const result = await api.search({
      accountId: f.accountId || 0,
      folder: f.accountId ? f.folder : '',
      q: f.q,
      from: f.from,
      to: f.to,
      after: dayStart(f.after),
      before: dayStart(f.before, 1),
      unread: f.unread,
      flagged: f.flagged,
      attachments: f.attachments,
      includeTrash: f.includeTrash,
      page,
      pageSize: PAGE_SIZE,
      ...store.sortParams(sort),
    })
    if (token !== searchRequest) return
    search.results = append ? [...search.results, ...result.messages] : result.messages
    search.total = result.total
    search.page = page
    search.searched = true
  } catch (e) {
    if (token === searchRequest) error.value = e.message
  } finally {
    if (token === searchRequest) searching.value = false
  }
}

// AI:n gör om frågan till filter. Filtren visas och går att ändra, så att det syns vad som faktiskt söktes.
async function interpret() {
  const question = search.question.trim()
  if (!question) return
  interpreting.value = true
  error.value = ''
  try {
    const result = await api.aiSearch(question, today())
    search.form = {
      ...store.emptySearchForm(),
      q: result.q,
      from: result.from,
      to: result.to,
      after: result.after,
      before: result.before,
      accountId: result.accountId,
      folder: result.accountId ? result.folder : '',
      unread: result.unread,
      flagged: result.flagged,
      attachments: result.attachments,
    }
    search.explanation = result.explanation
    await runSearch()
  } catch (e) {
    error.value = e.message
  } finally {
    interpreting.value = false
  }
}

function clear() {
  searchRequest++
  Object.assign(search, { question: '', explanation: '', form: store.emptySearchForm(), results: [], total: 0, page: 0, searched: false })
  error.value = ''
}

function openAiSettings() {
  store.searchDialog = false
  store.aiDialog = true
}

function close() {
  store.searchDialog = false
}

function onKey(event) {
  if (event.key === 'Escape') close()
}

onMounted(async () => {
  window.addEventListener('keydown', onKey)
  questionInput.value?.focus()
  // En söktext från meddelandelistan söks direkt.
  if (form.value.q && !search.searched) runSearch()
  try {
    aiConfigured.value = (await api.ai()).configured
  } catch {
    aiConfigured.value = false
  }
})
onBeforeUnmount(() => window.removeEventListener('keydown', onKey))
</script>

<template>
  <div class="modal-backdrop">
    <div class="modal search-dialog">
      <div class="modal-header">
        <h2>Sök i alla mappar</h2>
        <button class="btn ghost" title="Stäng (Esc)" @click="close"><Icon name="close" /></button>
      </div>

      <div class="filters">
        <form v-if="aiConfigured" class="ask" @submit.prevent="interpret">
          <label class="grow">
            <span>Beskriv vad du letar efter</span>
            <input
              ref="questionInput"
              v-model="search.question"
              class="input"
              maxlength="500"
              placeholder="Till exempel: fakturan från elbolaget i våras, eller olästa mejl med bilaga från Anna förra veckan"
            />
          </label>
          <button class="btn primary" type="submit" :disabled="interpreting || !search.question.trim()">
            <Icon name="sparkles" /> {{ interpreting ? 'Tolkar…' : 'Sök med AI' }}
          </button>
        </form>
        <p v-else-if="aiConfigured === false" class="muted note">
          Med en AI-tjänst kan du beskriva vad du letar efter med egna ord.
          <button class="link" @click="openAiSettings">Ställ in AI-tjänst</button>
        </p>
        <p v-if="search.explanation" class="explanation"><Icon name="sparkles" :size="12" /> {{ search.explanation }} Filtren nedan går att ändra.</p>

        <form class="fields" @submit.prevent="runSearch()">
          <label class="wide">
            <span>Sökord <em>alla ska finnas med; skriv | mellan alternativ, t.ex. kvitto|faktura</em></span>
            <input v-model="form.q" class="input" maxlength="1000" spellcheck="false" />
          </label>
          <label>
            <span>Från</span>
            <input v-model="form.from" class="input" maxlength="200" placeholder="Namn eller adress" spellcheck="false" />
          </label>
          <label>
            <span>Till</span>
            <input v-model="form.to" class="input" maxlength="200" placeholder="Namn eller adress" spellcheck="false" />
          </label>
          <label>
            <span>Från datum</span>
            <input v-model="form.after" class="input" type="date" />
          </label>
          <label>
            <span>Till datum</span>
            <input v-model="form.before" class="input" type="date" />
          </label>
          <label>
            <span>Konto</span>
            <select v-model="form.accountId" class="input">
              <option :value="null">Alla konton</option>
              <option v-for="a in store.accounts" :key="a.id" :value="a.id">{{ a.name }} &lt;{{ a.email }}&gt;</option>
            </select>
          </label>
          <label>
            <span>Mapp</span>
            <select v-model="form.folder" class="input" :disabled="!form.accountId" :title="form.accountId ? '' : 'Välj ett konto för att söka i en viss mapp'">
              <option value="">Alla mappar</option>
              <option v-for="f in accountFolders" :key="f.path" :value="f.path">{{ folderName(f) }}</option>
            </select>
          </label>
          <div class="checks wide">
            <label class="inline"><input v-model="form.unread" type="checkbox" /> Bara olästa</label>
            <label class="inline"><input v-model="form.flagged" type="checkbox" /> Bara stjärnmärkta</label>
            <label class="inline"><input v-model="form.attachments" type="checkbox" /> Med bilaga</label>
            <label v-if="!form.folder" class="inline"><input v-model="form.includeTrash" type="checkbox" /> Ta med papperskorg och skräppost</label>
            <span class="spacer" />
            <button class="btn" type="button" @click="clear">Rensa</button>
            <button class="btn" :class="{ primary: !aiConfigured }" type="submit" :disabled="searching"><Icon name="search" /> Sök</button>
          </div>
        </form>
        <p v-if="error" class="error-text">{{ error }}</p>
      </div>

      <div class="results">
        <p v-if="searching && !search.results.length" class="notice muted">Söker…</p>
        <p v-else-if="search.searched && !search.results.length" class="notice muted">
          Inga träffar. Prova färre sökord, fler alternativ med | eller ett större datumintervall.
        </p>
        <p v-else-if="!search.searched" class="notice muted">
          Sökningen går i ämne, avsändare, mottagare och meddelandenas text, i alla konton och mappar om du inte väljer något annat.
        </p>
        <template v-else>
          <p class="count muted">
            <span>{{ search.total }} träff(ar), {{ sortLabel }}. Klicka på en träff för att öppna den i sin mapp.</span>
            <span class="sort-control">
              <select :value="sort.key" aria-label="Sortera efter" :disabled="searching" @change="setSort($event.target.value, $event.target.value === sort.key ? sort.desc : ['date', 'size'].includes($event.target.value))">
                <option v-for="s in SORT_KEYS" :key="s.key" :value="s.key">{{ s.label }}</option>
              </select>
              <button class="btn ghost" :disabled="searching" :title="sort.desc ? 'Fallande ordning. Klicka för stigande.' : 'Stigande ordning. Klicka för fallande.'" @click="setSort(sort.key, !sort.desc)">
                <Icon :name="sort.desc ? 'arrowDown' : 'arrowUp'" :size="12" />
              </button>
            </span>
          </p>
          <table>
            <thead>
              <tr>
                <th>Ämne</th>
                <th>Korrespondent</th>
                <th>Mapp</th>
                <th class="date">Datum</th>
              </tr>
            </thead>
            <tbody>
              <tr v-for="hit in search.results" :key="`${hit.accountId}:${hit.folder}:${hit.uid}`" :class="{ unread: !hit.seen }" @click="store.openSearchResult(hit)">
                <td :title="hit.subject">
                  <Icon v-if="hit.flagged" name="star" :size="12" />
                  <Icon v-if="hit.hasAttachments" name="paperclip" :size="12" />
                  {{ hit.subject || '(inget ämne)' }}
                </td>
                <td :title="correspondent(hit)">{{ correspondent(hit) }}</td>
                <td :title="`${hit.folder} · ${accountName(hit.accountId)}`">
                  {{ hitFolder(hit) }}<span v-if="store.accounts.length > 1" class="muted"> · {{ accountName(hit.accountId) }}</span>
                </td>
                <td class="date">{{ formatListDate(hit.date) }}</td>
              </tr>
            </tbody>
          </table>
          <p v-if="search.results.length < search.total" class="more">
            <button class="btn" :disabled="searching" @click="runSearch({ append: true })">{{ searching ? 'Hämtar…' : 'Visa fler' }}</button>
          </p>
        </template>
      </div>
    </div>
  </div>
</template>

<style scoped>
.search-dialog {
  max-width: 980px;
  height: calc(100vh - 32px);
}

.filters {
  padding: 12px 16px;
  display: flex;
  flex-direction: column;
  gap: 10px;
  border-bottom: 1px solid var(--border);
}

.filters p {
  margin: 0;
}

.ask {
  display: flex;
  align-items: flex-end;
  gap: 8px;
}

.grow {
  flex: 1;
}

.fields {
  display: grid;
  grid-template-columns: repeat(4, minmax(0, 1fr));
  gap: 8px 12px;
}

.wide {
  grid-column: 1 / -1;
}

label {
  display: flex;
  flex-direction: column;
  gap: 4px;
  min-width: 0;
}

label > span {
  font-size: 12px;
  color: var(--text-muted);
}

label > span em {
  font-style: normal;
  opacity: 0.8;
  margin-left: 6px;
}

label.inline {
  flex-direction: row;
  align-items: center;
  gap: 6px;
}

.checks {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 8px 16px;
}

.spacer {
  flex: 1;
}

.note,
.explanation {
  font-size: 12px;
}

.explanation {
  color: var(--text-muted);
}

.link {
  border: 0;
  background: none;
  padding: 0;
  color: var(--accent);
  cursor: pointer;
  text-decoration: underline;
}

.results {
  flex: 1;
  overflow: auto;
  min-height: 120px;
}

.notice,
.count,
.more {
  margin: 0;
  padding: 10px 16px;
}

.count {
  font-size: 12px;
  display: flex;
  align-items: center;
  gap: 8px;
  flex-wrap: wrap;
}

.count > span:first-child {
  flex: 1;
}

.sort-control {
  display: flex;
  align-items: center;
  gap: 4px;
}

.sort-control select {
  font-size: 12px;
  padding: 2px 4px;
}

.sort-control .btn {
  padding: 2px 6px;
}

.more {
  text-align: center;
}

table {
  width: 100%;
  border-collapse: collapse;
  table-layout: fixed;
}

th {
  position: sticky;
  top: 0;
  background: var(--pane-alt);
  text-align: left;
  font-weight: 600;
  font-size: 12px;
  padding: 6px 8px;
  z-index: 1;
}

th:first-child,
td:first-child {
  padding-left: 16px;
}

th:nth-child(2) {
  width: 24%;
}

th:nth-child(3) {
  width: 22%;
}

.date {
  width: 120px;
}

td {
  padding: 5px 8px;
  border-top: 1px solid var(--border);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

tbody tr {
  cursor: pointer;
}

tbody tr:hover {
  background: var(--pane-alt);
}

tr.unread td {
  font-weight: 600;
}

@media (max-width: 700px) {
  .fields {
    grid-template-columns: 1fr 1fr;
  }

  .ask {
    flex-direction: column;
    align-items: stretch;
  }

  th:nth-child(2),
  td:nth-child(2) {
    display: none;
  }
}
</style>
