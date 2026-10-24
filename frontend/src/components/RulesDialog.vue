<script setup>
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { api } from '../api'
import { useMailStore } from '../store'
import { formatListDate, specialFolderNames } from '../utils'
import { t } from '../i18n'
import Icon from './Icon.vue'

const store = useMailStore()
// store.rulesDialog är true eller { accountId, prefill: { name, conditions } } när dialogen öppnas från ett meddelande.
const preset = typeof store.rulesDialog === 'object' ? store.rulesDialog : {}

const tab = ref('rules') // rules, suggestions, history
const loading = ref(true)
const loadError = ref('')
const data = ref({ rules: [], accounts: [], jobs: [], log: [], cache: false, aiConfigured: false, maxRules: 50, maxConditions: 10 })
const accountId = ref(preset.accountId ?? store.current.accountId ?? store.accounts[0]?.id ?? null)
const busy = ref(false)
const error = ref('')

// Mappar som en regel aldrig flyttar till: utkast och skickat förbjuds av servern, resten är virtuella (Gmail).
const NEVER_TARGET = ['drafts', 'sent', 'all', 'flagged', 'important']

// Etiketterna översätts med t() där de visas.
const FIELDS = [
  { value: 'from', label: 'From' },
  { value: 'to', label: 'To' },
  { value: 'subject', label: 'Subject' },
]
const OPS = [
  { value: 'contains', label: 'contains' },
  { value: 'is', label: 'is' },
  { value: 'starts', label: 'starts with' },
  { value: 'ends', label: 'ends with' },
  { value: 'domain', label: 'has domain' },
]

function folderName(f) {
  const special = specialFolderNames[f.specialUse]
  return special ? t(special) : f.path
}

function folderLabelByPath(id, path) {
  const f = (store.folders[id] || []).find((x) => x.path === path)
  return f ? folderName(f) : path
}

function accountEmail(id) {
  return store.accounts.find((a) => a.id === id)?.email || ''
}

// --- Regler för valt konto -----------------------------------------------------

const accountRules = computed(() =>
  data.value.rules.filter((r) => r.accountId === accountId.value).sort((a, b) => a.position - b.position),
)
const accountState = computed(() => data.value.accounts.find((a) => a.accountId === accountId.value))
const enabledCount = computed(() => accountRules.value.filter((r) => r.enabled).length)
const atMax = computed(() => accountRules.value.length >= (data.value.maxRules || Infinity))
const accountLog = computed(() => data.value.log.filter((e) => e.accountId === accountId.value))

const isActiveJob = (j) => ['queued', 'running'].includes(j.state)
const accountJob = computed(() =>
  data.value.jobs
    .filter((j) => j.accountId === accountId.value)
    .sort((a, b) => (b.startedAt || 0) - (a.startedAt || 0))[0],
)
const jobActive = computed(() => !!accountJob.value && isActiveJob(accountJob.value))

const jobStateText = computed(() => ({
  queued: t('Queued'),
  running: t('Running'),
  done: t('Done'),
  failed: t('Failed'),
}))

const moveFolders = computed(() =>
  (store.folders[accountId.value] || []).filter((f) => f.selectable && f.path !== 'INBOX' && !NEVER_TARGET.includes(f.specialUse)),
)

let pollTimer = null

// Tar emot serverns svar (alla ändringar svarar med hela läget) och hämtar om var tredje sekund medan ett jobb pågår.
function applyData(result) {
  const wasActive = new Set(data.value.jobs.filter(isActiveJob).map((j) => `${j.accountId}:${j.folder}`))
  data.value = { ...data.value, ...result }
  clearTimeout(pollTimer)
  if (data.value.jobs.some(isActiveJob)) pollTimer = setTimeout(reload, 3000)
  // Ett jobb blev klart: mapparnas antal och listan har ändrats.
  for (const j of data.value.jobs) {
    if (!wasActive.has(`${j.accountId}:${j.folder}`) || isActiveJob(j)) continue
    store.loadFolders(j.accountId)
    if (j.accountId === store.current.accountId) store.loadMessages({ keepSelection: true })
  }
}

async function reload() {
  try {
    applyData(await api.rules())
  } catch (e) {
    error.value = e.message
  }
}

async function call(fn, target = error) {
  busy.value = true
  target.value = ''
  try {
    applyData(await fn())
    return true
  } catch (e) {
    target.value = e.message
    return false
  } finally {
    busy.value = false
  }
}

// --- Sammanfattning av en regel -------------------------------------------------

function fieldLabel(field) {
  return t(FIELDS.find((f) => f.value === field)?.label || field)
}

function conditionText(c) {
  const params = { field: fieldLabel(c.field), value: c.value }
  switch (c.op) {
    case 'is':
      return t('{field} is {value}', params)
    case 'starts':
      return t('{field} starts with {value}', params)
    case 'ends':
      return t('{field} ends with {value}', params)
    case 'domain':
      return t('{field} has domain {value}', params)
    default:
      return t('{field} contains {value}', params)
  }
}

function actionsText(id, actions) {
  const parts = []
  if (actions.move) parts.push(t('move to {folder}', { folder: folderLabelByPath(id, actions.move) }))
  if (actions.junk) parts.push(t('mark as junk'))
  if (actions.trash) parts.push(t('delete'))
  if (actions.read) parts.push(t('mark as read'))
  if (actions.star) parts.push(t('star'))
  if (!parts.length) parts.push(actions.stop ? t('leave in place, stop processing') : t('leave in place'))
  return parts.join(', ')
}

function ruleSummary(id, rule) {
  const glue = ` ${rule.matchAll ? t('and') : t('or')} `
  return `${(rule.conditions || []).map(conditionText).join(glue)} → ${actionsText(id, rule.actions || {})}`
}

function missingFolder(rule) {
  return !!rule.actions?.move && !store.folderInfo(rule.accountId, rule.actions.move)
}

// --- Redigeraren -----------------------------------------------------------------

const editor = ref(null) // null eller formuläret nedan
const editorError = ref('')
const saving = ref(false)

const emptyCondition = () => ({ field: 'from', op: 'contains', value: '' })

function formFromRule(rule) {
  const a = rule?.actions || {}
  return {
    id: rule?.id ?? null,
    name: rule?.name || '',
    enabled: rule ? rule.enabled !== false : true,
    matchAll: rule ? rule.matchAll !== false : true,
    conditions: rule?.conditions?.length ? rule.conditions.map((c) => ({ ...c })) : [emptyCondition()],
    destination: a.move ? 'move' : a.junk ? 'junk' : a.trash ? 'trash' : 'none',
    move: a.move || '',
    read: !!a.read,
    star: !!a.star,
    // En sparad regel har det den har; en ny eller förifylld stoppar vidare regler som standard.
    stop: rule?.id ? !!a.stop : true,
  }
}

function openEditor(rule) {
  editor.value = formFromRule(rule)
  editorError.value = ''
  preview.value = null
  previewError.value = ''
}

function closeEditor() {
  editor.value = null
  clearTimeout(previewTimer)
}

function opsFor(field) {
  return field === 'subject' ? OPS.filter((o) => o.value !== 'domain') : OPS
}

function onFieldChange(c) {
  if (!opsFor(c.field).some((o) => o.value === c.op)) c.op = 'contains'
}

function placeholderFor(c) {
  if (c.op === 'domain') return '@example.com'
  if (c.field === 'subject') return t('e.g. Invoice')
  return 'name@example.com'
}

function formActions(f) {
  const actions = {}
  if (f.destination === 'move' && f.move) actions.move = f.move
  else if (f.destination === 'junk') actions.junk = true
  else if (f.destination === 'trash') actions.trash = true
  if (f.read) actions.read = true
  if (f.star) actions.star = true
  if (f.stop) actions.stop = true
  return actions
}

function formConditions(f) {
  return f.conditions.map((c) => ({ field: c.field, op: c.op, value: c.value.trim() }))
}

function formPayload(f) {
  return { name: f.name.trim(), enabled: f.enabled, matchAll: f.matchAll, conditions: formConditions(f), actions: formActions(f) }
}

const conditionsComplete = computed(() => {
  const f = editor.value
  return !!f && f.conditions.length > 0 && f.conditions.every((c) => c.value.trim())
})
const editorValid = computed(() => {
  const f = editor.value
  if (!f || !f.name.trim() || !conditionsComplete.value) return false
  if (f.destination === 'move' && !f.move) return false
  return Object.keys(formActions(f)).length > 0
})
// Mappen en sparad regel flyttar till kan ha tagits bort; då visas den ändå så att valet går att se och ändra.
const moveOptions = computed(() => {
  const f = editor.value
  if (f?.move && !moveFolders.value.some((x) => x.path === f.move)) {
    return [...moveFolders.value, { path: f.move, missing: true }]
  }
  return moveFolders.value
})

async function saveRule() {
  const f = editor.value
  saving.value = true
  editorError.value = ''
  try {
    const payload = formPayload(f)
    applyData(f.id ? await api.ruleUpdate(f.id, payload) : await api.ruleSave(accountId.value, payload))
    closeEditor()
    store.notify(t('Rule saved'))
  } catch (e) {
    editorError.value = e.message
  } finally {
    saving.value = false
  }
}

// Förhandsvisning mot inkorgen: hur många meddelanden regeln skulle träffa. Kräver mejlcachen.
const preview = ref(null) // { total, matches, sample, capped }
const previewing = ref(false)
const previewError = ref('')
let previewTimer = null
let previewSeq = 0

watch(
  () => {
    const f = editor.value
    if (!f || !conditionsComplete.value) return null
    return JSON.stringify({ matchAll: f.matchAll, conditions: formConditions(f) })
  },
  (key) => {
    clearTimeout(previewTimer)
    if (!key || !data.value.cache) {
      preview.value = null
      previewing.value = false
      return
    }
    previewTimer = setTimeout(runPreview, 400)
  },
)

async function runPreview() {
  const f = editor.value
  if (!f) return
  const seq = ++previewSeq
  previewing.value = true
  previewError.value = ''
  try {
    const actions = formActions(f)
    const rule = { name: f.name.trim(), matchAll: f.matchAll, conditions: formConditions(f), actions: Object.keys(actions).length ? actions : { stop: true } }
    const result = await api.ruleTest(accountId.value, rule, 'INBOX')
    if (seq === previewSeq) preview.value = result
  } catch (e) {
    if (seq === previewSeq) {
      preview.value = null
      previewError.value = e.message
    }
  } finally {
    if (seq === previewSeq) previewing.value = false
  }
}

// --- Listan -----------------------------------------------------------------------

function rulePayload(rule, changes = {}) {
  return { name: rule.name, enabled: rule.enabled, matchAll: rule.matchAll, conditions: rule.conditions, actions: rule.actions, ...changes }
}

const toggleEnabled = (rule, enabled) => call(() => api.ruleUpdate(rule.id, rulePayload(rule, { enabled })))

async function moveRule(rule, delta) {
  const ids = accountRules.value.map((r) => r.id)
  const i = ids.indexOf(rule.id)
  const j = i + delta
  if (i === -1 || j < 0 || j >= ids.length) return
  ;[ids[i], ids[j]] = [ids[j], ids[i]]
  await call(() => api.ruleOrder(accountId.value, ids))
}

async function deleteRule(rule) {
  if (!window.confirm(t('Delete the rule "{name}"?', { name: rule.name }))) return
  if (await call(() => api.ruleDelete(rule.id))) store.notify(t('The rule was deleted'))
}

async function runNow() {
  const ok = window.confirm(
    t('Apply the enabled rules to all messages currently in the Inbox of {email}?', { email: accountEmail(accountId.value) }) +
      '\n\n' +
      t('The rules run on the server in the background. Moves can be undone from the history.'),
  )
  if (!ok) return
  await call(() => api.ruleRun(accountId.value, 'INBOX'))
}

// --- Förslag ------------------------------------------------------------------------

const suggestions = ref(null) // null = inte hämtade ännu
const suggesting = ref(false)
const suggestError = ref('')
const useAi = ref(false)
const aiUsed = ref(false)

async function suggest() {
  suggesting.value = true
  suggestError.value = ''
  try {
    const result = await api.ruleSuggest(accountId.value, useAi.value && data.value.aiConfigured)
    suggestions.value = result.suggestions || []
    aiUsed.value = !!result.aiUsed
  } catch (e) {
    suggestError.value = e.message
  } finally {
    suggesting.value = false
  }
}

function hideSuggestion(s) {
  suggestions.value = (suggestions.value || []).filter((x) => x.key !== s.key)
}

async function addSuggestion(s) {
  const rule = { name: s.name, enabled: true, matchAll: s.matchAll !== false, conditions: s.conditions, actions: s.actions }
  if (await call(() => api.ruleSave(accountId.value, rule), suggestError)) {
    hideSuggestion(s)
    store.notify(t('Rule saved'))
  }
}

// Ett avböjt förslag om en enskild avsändare (eller domän) ur historiken ska inte komma tillbaka; annat döljs bara.
async function declineSuggestion(s) {
  hideSuggestion(s)
  const c = s.conditions?.length === 1 ? s.conditions[0] : null
  if (s.source !== 'history' || !c || c.field !== 'from' || !['is', 'domain'].includes(c.op)) return
  try {
    await api.ruleDismiss(accountId.value, c.value)
  } catch (e) {
    suggestError.value = e.message
  }
}

function suggestionCounts(s) {
  return t('{n} in {folder}, {m} elsewhere, {k} still in Inbox', {
    n: s.inFolder ?? 0,
    folder: folderLabelByPath(accountId.value, s.folder),
    m: s.elsewhere ?? 0,
    k: s.inInbox ?? 0,
  })
}

// --- Historik -------------------------------------------------------------------------

const undoing = ref(null)

async function undo(entry) {
  undoing.value = entry.id
  error.value = ''
  try {
    await api.aiUndo(entry.id)
    await reload()
    store.notify(t('The message was moved back'))
    store.loadFolders(entry.accountId)
    if (entry.accountId === store.current.accountId) store.loadMessages({ keepSelection: true })
  } catch (e) {
    error.value = e.message
  } finally {
    undoing.value = null
  }
}

function ruleNameFor(entry) {
  return data.value.rules.find((r) => r.id === entry.ruleId)?.name || ''
}

// --- Dialogen ---------------------------------------------------------------------------

watch(accountId, async (id) => {
  closeEditor()
  suggestions.value = null
  suggestError.value = ''
  error.value = ''
  if (id && !store.folders[id]) await store.loadFolders(id)
})

function close() {
  store.rulesDialog = false
}

function onKey(event) {
  if (event.key !== 'Escape') return
  if (editor.value) closeEditor()
  else close()
}

onMounted(async () => {
  window.addEventListener('keydown', onKey)
  try {
    if (accountId.value && !store.folders[accountId.value]) await store.loadFolders(accountId.value)
    applyData(await api.rules())
    if (preset.prefill) openEditor({ name: preset.prefill.name || '', conditions: preset.prefill.conditions || [] })
  } catch (e) {
    loadError.value = e.message
  } finally {
    loading.value = false
  }
})

onBeforeUnmount(() => {
  window.removeEventListener('keydown', onKey)
  clearTimeout(pollTimer)
  clearTimeout(previewTimer)
})
</script>

<template>
  <div class="modal-backdrop">
    <div class="modal rules-dialog">
      <div class="modal-header">
        <h2>{{ t('Rules') }}</h2>
        <button class="btn ghost" :title="t('Close')" @click="close"><Icon name="close" /></button>
      </div>

      <nav class="tabs">
        <button :class="{ active: tab === 'rules' }" @click="tab = 'rules'"><Icon name="filter" /> {{ t('Rules') }}</button>
        <button :class="{ active: tab === 'suggestions' }" @click="tab = 'suggestions'"><Icon name="sparkles" /> {{ t('Suggestions') }}</button>
        <button :class="{ active: tab === 'history' }" @click="tab = 'history'"><Icon name="clock" /> {{ t('History') }}</button>
      </nav>

      <div v-if="loading" class="body muted">{{ t('Loading…') }}</div>
      <div v-else-if="loadError" class="body error-text">{{ loadError }}</div>
      <div v-else-if="!store.accounts.length" class="body muted">{{ t('Add an email account first.') }}</div>

      <template v-else>
        <!-- Konto och läge: gemensamt för alla flikar -->
        <div class="account-row">
          <label v-if="store.accounts.length > 1" class="inline">
            <span>{{ t('Account') }}</span>
            <select v-model="accountId" class="input auto" :disabled="!!editor">
              <option v-for="a in store.accounts" :key="a.id" :value="a.id">{{ a.email }}</option>
            </select>
          </label>
          <span class="muted note status">
            <template v-if="accountState?.lastError">
              <span class="error-text">{{ t('The last run failed: {error}', { error: accountState.lastError }) }}</span>
            </template>
            <template v-else-if="accountState?.lastRunAt">{{ t('Last run {date}.', { date: formatListDate(accountState.lastRunAt) }) }}</template>
            <template v-else>{{ t('The rules have not run yet.') }}</template>
            <template v-if="!data.cache"> {{ t('The mail cache is off, so the preview and suggestions are unavailable.') }}</template>
          </span>
        </div>

        <!-- Regler -->
        <template v-if="tab === 'rules'">
          <!-- Redigeraren -->
          <form v-if="editor" class="body" @submit.prevent="editorValid && !saving && saveRule()">
            <h3>{{ editor.id ? t('Edit rule') : t('New rule') }}</h3>
            <label>
              <span>{{ t('Name') }}</span>
              <input v-model="editor.name" class="input" maxlength="100" required />
            </label>

            <div class="match-row">
              <span class="muted">{{ t('Apply when') }}</span>
              <label class="inline"><input v-model="editor.matchAll" type="radio" :value="true" /> {{ t('all of the conditions match') }}</label>
              <label class="inline"><input v-model="editor.matchAll" type="radio" :value="false" /> {{ t('any of the conditions match') }}</label>
            </div>

            <div class="conditions">
              <div v-for="(c, i) in editor.conditions" :key="i" class="condition">
                <select v-model="c.field" class="input auto" @change="onFieldChange(c)">
                  <option v-for="f in FIELDS" :key="f.value" :value="f.value">{{ t(f.label) }}</option>
                </select>
                <select v-model="c.op" class="input auto">
                  <option v-for="o in opsFor(c.field)" :key="o.value" :value="o.value">{{ t(o.label) }}</option>
                </select>
                <input v-model="c.value" class="input" :placeholder="placeholderFor(c)" spellcheck="false" />
                <button
                  class="btn ghost small"
                  type="button"
                  :title="t('Remove condition')"
                  :disabled="editor.conditions.length === 1"
                  @click="editor.conditions.splice(i, 1)"
                >
                  <Icon name="close" :size="12" />
                </button>
              </div>
              <button class="link" type="button" :disabled="editor.conditions.length >= data.maxConditions" @click="editor.conditions.push(emptyCondition())">
                {{ t('Add condition') }}
              </button>
            </div>

            <fieldset class="actions">
              <legend>{{ t('Then') }}</legend>
              <label class="inline"><input v-model="editor.destination" type="radio" value="none" /> {{ t('Leave in place') }}</label>
              <label class="inline">
                <input v-model="editor.destination" type="radio" value="move" /> {{ t('Move to folder') }}
                <select v-model="editor.move" class="input auto" :disabled="editor.destination !== 'move'" @focus="editor.destination = 'move'">
                  <option value="" disabled>{{ t('Choose a folder…') }}</option>
                  <option v-for="f in moveOptions" :key="f.path" :value="f.path">
                    {{ f.missing ? t('{folder} (the folder no longer exists)', { folder: f.path }) : folderName(f) }}
                  </option>
                </select>
              </label>
              <label class="inline"><input v-model="editor.destination" type="radio" value="junk" /> {{ t('Mark as junk|rule') }}</label>
              <label class="inline"><input v-model="editor.destination" type="radio" value="trash" /> {{ t('Delete (to trash)') }}</label>
              <div class="flags">
                <label class="inline"><input v-model="editor.read" type="checkbox" /> {{ t('Mark as read') }}</label>
                <label class="inline"><input v-model="editor.star" type="checkbox" /> {{ t('Star') }}</label>
                <label class="inline"><input v-model="editor.stop" type="checkbox" /> {{ t('Stop processing further rules') }}</label>
              </div>
            </fieldset>

            <div v-if="data.cache" class="preview">
              <template v-if="!conditionsComplete">
                <span class="muted note">{{ t('Fill in the conditions to see how many messages in the Inbox match.') }}</span>
              </template>
              <template v-else-if="previewing && !preview">
                <span class="muted note">{{ t('Checking the Inbox…') }}</span>
              </template>
              <template v-else-if="previewError">
                <span class="error-text note">{{ previewError }}</span>
              </template>
              <template v-else-if="preview">
                <strong>{{ t('Matches {n} of {total} messages in Inbox', { n: preview.matches, total: preview.total }) }}</strong>
                <span v-if="preview.capped" class="muted note"> {{ t('(only part of the folder was checked; the real number may be higher)') }}</span>
                <ul v-if="preview.sample?.length" class="sample muted">
                  <li v-for="m in preview.sample.slice(0, 5)" :key="m.uid" :title="m.from">
                    {{ m.subject || t('(no subject)') }} <span class="date">{{ formatListDate(m.date) }}</span>
                  </li>
                </ul>
              </template>
            </div>

            <p v-if="editorError" class="error-text">{{ editorError }}</p>

            <div class="editor-buttons">
              <label v-if="editor.id" class="inline"><input v-model="editor.enabled" type="checkbox" /> {{ t('Enabled') }}</label>
              <span class="spacer" />
              <button class="btn" type="button" @click="closeEditor">{{ t('Cancel') }}</button>
              <button class="btn primary" type="submit" :disabled="!editorValid || saving">{{ saving ? t('Saving…') : t('Save') }}</button>
            </div>
          </form>

          <!-- Listan -->
          <div v-else class="body">
            <p class="muted note">
              {{ t('Rules are applied on the server to new mail in the Inbox, in the order below, before the AI sorting. A rule that moves or deletes a message normally stops the rules after it.') }}
            </p>

            <p v-if="!accountRules.length" class="muted">{{ t('No rules yet. Create one, or let PigeonPost suggest rules from how you have sorted your mail so far.') }}</p>
            <div v-else class="rule-list">
              <div v-for="(r, i) in accountRules" :key="r.id" class="rule" :class="{ off: !r.enabled }">
                <input type="checkbox" :checked="r.enabled" :disabled="busy" :title="t('Enabled')" @change="toggleEnabled(r, $event.target.checked)" />
                <div class="rule-text">
                  <strong>{{ r.name }}</strong>
                  <span class="muted summary" :title="ruleSummary(r.accountId, r)">{{ ruleSummary(r.accountId, r) }}</span>
                  <span v-if="missingFolder(r)" class="error-text note">{{ t('The folder the rule moves to no longer exists.') }}</span>
                </div>
                <button class="btn ghost small" type="button" :title="t('Move up')" :disabled="busy || i === 0" @click="moveRule(r, -1)"><Icon name="arrowUp" :size="14" /></button>
                <button class="btn ghost small" type="button" :title="t('Move down')" :disabled="busy || i === accountRules.length - 1" @click="moveRule(r, 1)"><Icon name="arrowDown" :size="14" /></button>
                <button class="btn small" type="button" :disabled="busy" @click="openEditor(r)">{{ t('Edit') }}</button>
                <button class="btn ghost small danger" type="button" :title="t('Delete rule')" :disabled="busy" @click="deleteRule(r)"><Icon name="trash" :size="14" /></button>
              </div>
            </div>

            <div class="toolbar-row">
              <button class="btn primary" type="button" :disabled="atMax || busy" :title="atMax ? t('At most {n} rules per account', { n: data.maxRules }) : ''" @click="openEditor(null)">
                <Icon name="compose" /> {{ t('New rule') }}
              </button>
              <button class="btn" type="button" :disabled="busy || jobActive || !enabledCount" :title="t('Apply the enabled rules to everything in the Inbox now')" @click="runNow">
                <Icon name="play" /> {{ t('Run rules now') }}
              </button>
              <span v-if="atMax" class="muted note">{{ t('At most {n} rules per account', { n: data.maxRules }) }}</span>
            </div>

            <section v-if="accountJob" class="job" :class="`state-${accountJob.state}`">
              <div class="job-head">
                <strong>{{ t('Run in {folder}', { folder: folderLabelByPath(accountJob.accountId, accountJob.folder) }) }}</strong>
                <span class="job-state">{{ jobStateText[accountJob.state] || accountJob.state }}</span>
                <span class="spacer" />
                <span class="muted note">
                  {{ t('{done} of {total} checked, {moved} moved, {changed} changed', { done: accountJob.processed ?? 0, total: accountJob.total ?? 0, moved: accountJob.moved ?? 0, changed: accountJob.changed ?? 0 }) }}
                </span>
              </div>
              <progress v-if="isActiveJob(accountJob)" :value="accountJob.processed || 0" :max="accountJob.total || 1" />
              <p v-if="accountJob.error" class="error-text">{{ accountJob.error }}</p>
              <p v-else-if="accountJob.finishedAt" class="muted note">{{ t('Finished {date}.', { date: formatListDate(accountJob.finishedAt) }) }}</p>
            </section>

            <p v-if="error" class="error-text">{{ error }}</p>
          </div>
        </template>

        <!-- Förslag -->
        <div v-else-if="tab === 'suggestions'" class="body">
          <p class="muted note">
            {{ t('PigeonPost looks at the folders you have sorted mail into and suggests rules for senders that usually end up in the same place.') }}
          </p>
          <div class="toolbar-row">
            <button class="btn primary" type="button" :disabled="suggesting || !data.cache" @click="suggest">
              <Icon name="sparkles" /> {{ suggesting ? t('Looking through your folders…') : t('Suggest rules from my folders') }}
            </button>
            <label v-if="data.aiConfigured" class="inline"><input v-model="useAi" type="checkbox" :disabled="suggesting" /> {{ t('Also ask the AI') }}</label>
          </div>
          <p v-if="data.aiConfigured" class="muted note">
            {{ t('With the AI, sender addresses and subject lines per folder are sent to the AI service, never message bodies. This can take up to half a minute.') }}
          </p>
          <p v-if="!data.cache" class="muted note">{{ t('Suggestions need the mail cache, which is off for this server.') }}</p>
          <p v-if="suggestError" class="error-text">{{ suggestError }}</p>

          <template v-if="suggestions">
            <p v-if="!suggestions.length" class="muted">{{ t('No suggestions right now. Sort some mail into folders and try again later.') }}</p>
            <div v-for="s in suggestions" :key="s.key" class="suggestion">
              <div class="suggestion-head">
                <strong>{{ s.name }}</strong>
                <span class="badge" :class="s.source">{{ s.source === 'ai' ? t('AI') : t('from your folders') }}</span>
                <span class="spacer" />
                <button class="btn small primary" type="button" :disabled="busy || atMax" @click="addSuggestion(s)">{{ t('Add rule') }}</button>
                <button class="btn small ghost" type="button" :disabled="busy" @click="declineSuggestion(s)">{{ t('No thanks') }}</button>
              </div>
              <div class="summary">{{ ruleSummary(accountId, s) }}</div>
              <div class="muted note">{{ suggestionCounts(s) }}</div>
              <div v-if="s.explanation" class="muted note">{{ s.explanation }}</div>
            </div>
            <p v-if="aiUsed && suggestions.length" class="muted note">{{ t('Some suggestions come from the AI. Check them before adding.') }}</p>
          </template>
        </div>

        <!-- Historik -->
        <div v-else class="body">
          <p class="muted note">{{ t('Messages the rules have moved. Undo moves a message back to where it was.') }}</p>
          <p v-if="error" class="error-text">{{ error }}</p>
          <p v-if="!accountLog.length" class="muted">{{ t('The rules have not moved anything yet.') }}</p>
          <div v-else class="table-wrap history">
            <table>
              <thead>
                <tr>
                  <th>{{ t('When') }}</th>
                  <th>{{ t('From') }}</th>
                  <th>{{ t('Subject') }}</th>
                  <th>{{ t('Moved') }}</th>
                  <th>{{ t('Rule') }}</th>
                  <th class="undo" />
                </tr>
              </thead>
              <tbody>
                <tr v-for="e in accountLog" :key="e.id" :class="{ undone: e.undoneAt }">
                  <td>{{ formatListDate(e.movedAt) }}</td>
                  <td :title="e.sender">{{ e.sender }}</td>
                  <td :title="e.subject">{{ e.subject || t('(no subject)') }}</td>
                  <td :title="`${e.sourceFolder} → ${e.targetFolder}`">
                    {{ folderLabelByPath(e.accountId, e.sourceFolder) }} → {{ folderLabelByPath(e.accountId, e.targetFolder) }}
                  </td>
                  <td class="muted" :title="e.reason">{{ ruleNameFor(e) || e.reason }}</td>
                  <td class="undo">
                    <span v-if="e.undoneAt" class="muted">{{ t('Undone') }}</span>
                    <button v-else class="btn small" type="button" :disabled="undoing === e.id" @click="undo(e)">{{ t('Undo') }}</button>
                  </td>
                </tr>
              </tbody>
            </table>
          </div>
        </div>
      </template>

      <div class="modal-footer">
        <button class="btn" type="button" @click="close">{{ t('Close') }}</button>
      </div>
    </div>
  </div>
</template>

<style scoped>
.rules-dialog {
  max-width: 860px;
}

.tabs {
  display: flex;
  gap: 2px;
  padding: 6px 12px 0;
  border-bottom: 1px solid var(--border);
}

.tabs button {
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 8px 12px;
  border: 0;
  border-bottom: 2px solid transparent;
  background: none;
  cursor: pointer;
  color: var(--text-muted);
}

.tabs button.active {
  color: var(--text);
  border-bottom-color: var(--accent);
}

.account-row {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 8px 16px;
  padding: 12px 16px 0;
}

.status {
  flex: 1;
  min-width: 200px;
}

.body {
  padding: 16px;
  display: flex;
  flex-direction: column;
  gap: 12px;
  overflow: auto;
  min-height: 200px;
}

.body p,
.body h3 {
  margin: 0;
}

h3 {
  font-size: 13px;
}

.note {
  font-size: 12px;
}

label {
  display: flex;
  flex-direction: column;
  gap: 4px;
}

label > span {
  font-size: 12px;
  color: var(--text-muted);
}

label.inline {
  flex-direction: row;
  align-items: center;
  gap: 6px;
  flex-wrap: wrap;
}

label.inline > span {
  font-size: inherit;
  color: inherit;
}

.input.auto {
  width: auto;
}

.link {
  border: 0;
  background: none;
  padding: 0;
  color: var(--accent);
  cursor: pointer;
  text-decoration: underline;
  align-self: flex-start;
}

.link:disabled {
  opacity: 0.5;
  cursor: default;
}

.btn.small {
  padding: 2px 8px;
}

.spacer {
  flex: 1;
}

.match-row {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 6px 16px;
}

.conditions {
  display: flex;
  flex-direction: column;
  gap: 6px;
}

.condition {
  display: flex;
  align-items: center;
  gap: 6px;
}

.condition input {
  flex: 1;
  min-width: 120px;
}

fieldset.actions {
  border: 1px solid var(--border);
  border-radius: var(--radius);
  margin: 0;
  padding: 8px 12px 10px;
  display: flex;
  flex-direction: column;
  gap: 6px;
}

legend {
  font-weight: 600;
  padding: 0 4px;
}

.flags {
  display: flex;
  flex-wrap: wrap;
  gap: 6px 16px;
  margin-top: 4px;
  padding-top: 8px;
  border-top: 1px solid var(--border);
}

.preview {
  border: 1px solid var(--border);
  border-radius: var(--radius);
  padding: 8px 12px;
  background: var(--pane-alt);
}

.sample {
  margin: 6px 0 0;
  padding-left: 18px;
  font-size: 12px;
}

.sample li {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.date {
  margin-left: 6px;
}

.editor-buttons {
  display: flex;
  align-items: center;
  gap: 8px;
}

.rule-list {
  border: 1px solid var(--border);
  border-radius: var(--radius);
}

.rule {
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 6px 8px 6px 10px;
  border-top: 1px solid var(--border);
}

.rule:first-child {
  border-top: 0;
}

.rule.off .rule-text {
  opacity: 0.6;
}

.rule-text {
  flex: 1;
  min-width: 0;
  display: flex;
  flex-direction: column;
}

.rule-text .summary,
.rule-text strong {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.summary {
  font-size: 12px;
}

.toolbar-row {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 8px 16px;
}

.job {
  border: 1px solid var(--border);
  border-radius: var(--radius);
  padding: 8px 12px;
  display: flex;
  flex-direction: column;
  gap: 6px;
}

.job-head {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 6px 10px;
}

.job-state {
  font-size: 12px;
  padding: 1px 8px;
  border-radius: 10px;
  background: var(--pane-alt);
}

.job.state-running .job-state,
.job.state-queued .job-state {
  background: var(--accent);
  color: var(--accent-text);
}

.job.state-failed .job-state {
  background: var(--danger);
  color: #fff;
}

.job progress {
  width: 100%;
}

.suggestion {
  border: 1px solid var(--border);
  border-radius: var(--radius);
  padding: 8px 12px;
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.suggestion-head {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 6px 10px;
}

.badge {
  font-size: 11px;
  padding: 1px 8px;
  border-radius: 10px;
  background: var(--pane-alt);
  color: var(--text-muted);
}

.badge.ai {
  background: var(--accent);
  color: var(--accent-text);
}

.table-wrap {
  overflow: auto;
  border: 1px solid var(--border);
  border-radius: var(--radius);
  max-height: 360px;
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

td {
  padding: 4px 8px;
  border-top: 1px solid var(--border);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

th:first-child {
  width: 96px;
}

th:nth-child(2) {
  width: 20%;
}

.undo {
  width: 76px;
  text-align: right;
}

tr.undone td:not(.undo) {
  text-decoration: line-through;
  opacity: 0.6;
}

@media (max-width: 640px) {
  th:nth-child(5),
  td:nth-child(5) {
    display: none;
  }
}
</style>
