<script setup>
import { computed, onBeforeUnmount, onMounted, reactive, ref, watch } from 'vue'
import { api } from '../api'
import { useMailStore } from '../store'
import { displayName, formatListDate, specialFolderNames } from '../utils'
import Icon from './Icon.vue'

const store = useMailStore()
// store.aiDialog är true eller { accountId, folder, uids } när dialogen öppnas från en mapp eller markering.
const preset = typeof store.aiDialog === 'object' ? store.aiDialog : {}

const tab = ref('sort')
const loading = ref(true)
const loadError = ref('')
const ai = ref({ providers: [], settings: null, sortExcluded: {}, sortCursors: {}, configured: false, batchSize: 25, snippetChars: 500 })

// --- Inställningar ----------------------------------------------------------

const form = reactive({ baseUrl: '', model: '', apiKey: '', instructions: '' })
const models = ref([])
const testResult = ref(null)
const testing = ref(false)
const saving = ref(false)
const settingsError = ref('')

const provider = computed(() => ai.value.providers.find((p) => p.baseUrl === form.baseUrl))
const savedProvider = computed(() => ai.value.providers.find((p) => p.baseUrl === ai.value.settings?.baseUrl))
// Nyckel och modell sparas per tjänst, så att man kan byta tjänst och tillbaka utan att ange nyckeln igen.
const savedForProvider = computed(() => (ai.value.savedProviders || []).find((p) => p.baseUrl === form.baseUrl))
const keepsSavedKey = computed(() => !!savedForProvider.value?.hasApiKey)
const canSave = computed(
  () => !!provider.value && !!form.model.trim() && (!provider.value.keyRequired || keepsSavedKey.value || !!form.apiKey.trim()),
)

const keyLinks = {
  'https://generativelanguage.googleapis.com/v1beta/openai': 'https://aistudio.google.com/apikey',
  'https://api.openai.com/v1': 'https://platform.openai.com/api-keys',
  'https://api.mistral.ai/v1': 'https://console.mistral.ai/api-keys',
}

function fillForm() {
  const s = ai.value.settings
  const first = ai.value.providers[0]
  form.baseUrl = s?.providerAvailable ? s.baseUrl : first?.baseUrl || ''
  form.model = s?.providerAvailable ? s.model : first?.defaultModel || ''
  form.apiKey = ''
  form.instructions = s?.instructions || ''
}

function onProviderChange() {
  form.model = savedForProvider.value?.model || provider.value?.defaultModel || ''
  form.apiKey = ''
  models.value = []
  testResult.value = null
}

function settingsPayload() {
  return { baseUrl: form.baseUrl, model: form.model.trim(), apiKey: form.apiKey.trim(), instructions: form.instructions }
}

async function testSettings() {
  testing.value = true
  testResult.value = null
  settingsError.value = ''
  try {
    testResult.value = await api.aiTest(settingsPayload())
    models.value = testResult.value.models || []
  } catch (e) {
    settingsError.value = e.message
  } finally {
    testing.value = false
  }
}

async function saveSettings() {
  saving.value = true
  settingsError.value = ''
  try {
    ai.value = await api.aiSave(settingsPayload())
    form.apiKey = ''
    store.notify('AI-inställningarna sparades')
    tab.value = 'sort'
  } catch (e) {
    settingsError.value = e.message
  } finally {
    saving.value = false
  }
}

async function removeSettings() {
  if (!window.confirm('Ta bort AI-inställningarna och alla sparade API-nycklar?')) return
  try {
    await api.aiRemove()
    ai.value = await api.ai()
    fillForm()
    testResult.value = null
    store.notify('AI-inställningarna togs bort')
  } catch (e) {
    settingsError.value = e.message
  }
}

// --- Sortering --------------------------------------------------------------

const NEVER_TARGET = ['drafts', 'sent', 'all', 'flagged', 'important']
const EXCLUDED_KEY = 'brevduva.aiSort.excluded'

const accountId = ref(preset.accountId ?? store.current.accountId ?? store.accounts[0]?.id ?? null)
const folder = ref(preset.folder ?? store.current.folder ?? 'INBOX')
const presetUids = ref(preset.uids?.length ? [...preset.uids] : null)
const limit = ref(50)
const unreadOnly = ref(false)
// 'latest' tar de senaste i mappen. 'continue' tar de närmast äldre än dem som redan har gåtts igenom, så att en
// stor mapp går att sortera i omgångar utan att samma meddelanden kommer tillbaka.
const mode = ref('latest')
const remaining = ref(null) // Meddelanden äldre än märket; null = okänt.
const excluded = ref(new Set())

const phase = ref('setup') // setup, running, review
const progress = reactive({ done: 0, total: 0 })
const sortError = ref('')
const rows = ref([]) // { uid, from, subject, date, seen, suggested, target, reason, checked }
const moving = ref(false)
let cancelled = false

function folderName(f) {
  return specialFolderNames[f.specialUse] || f.path
}

const accountFolders = computed(() => (store.folders[accountId.value] || []).filter((f) => f.selectable))
const sourceFolders = computed(() => accountFolders.value.filter((f) => !['drafts', 'all', 'flagged', 'important'].includes(f.specialUse)))
const possibleTargets = computed(() =>
  accountFolders.value.filter((f) => f.path !== folder.value && !NEVER_TARGET.includes(f.specialUse)),
)
const targets = computed(() => possibleTargets.value.filter((f) => !excluded.value.has(f.path)))

// Valet sparas på servern, så att det finns kvar nästa gång oavsett webbläsare eller enhet.
let saveQueue = Promise.resolve()
function saveExcluded(id, paths) {
  ai.value.sortExcluded = { ...ai.value.sortExcluded, [id]: paths }
  // Ett anrop i taget, så att det senaste valet är det som blir sparat.
  saveQueue = saveQueue
    .then(() => api.aiSortTargets(id, paths))
    .catch((e) => store.notify(`Valet av mappar kunde inte sparas: ${e.message}`))
}

function loadExcluded() {
  const id = accountId.value
  if (!id) return
  const saved = ai.value.sortExcluded?.[id]
  if (Array.isArray(saved)) {
    excluded.value = new Set(saved)
    return
  }
  // Äldre versioner sparade valet i webbläsaren. Det flyttas till servern första gången.
  let legacy = null
  try {
    legacy = JSON.parse(localStorage.getItem(EXCLUDED_KEY) || '{}')[id]
  } catch {
    // Lagring otillgänglig; standardvalet används.
  }
  if (Array.isArray(legacy)) {
    excluded.value = new Set(legacy)
    saveExcluded(id, legacy)
    return
  }
  // Papperskorgen är avmarkerad från början, så att AI:n inte föreslår att något slängs.
  excluded.value = new Set(possibleTargets.value.filter((f) => f.specialUse === 'trash').map((f) => f.path))
}

function toggleTarget(path) {
  const next = new Set(excluded.value)
  if (next.has(path)) next.delete(path)
  else next.add(path)
  excluded.value = next
  saveExcluded(accountId.value, [...next])
}

// Märket för mappen: { beforeUid, date } för det äldsta meddelande som har gåtts igenom.
const cursor = computed(() => ai.value.sortCursors?.[accountId.value]?.[folder.value] || null)

let cursorCheck = 0
async function loadCursor() {
  const check = ++cursorCheck
  remaining.value = null
  if (!cursor.value) {
    mode.value = 'latest'
    return
  }
  mode.value = 'continue'
  try {
    const result = await api.messages(accountId.value, { folder: folder.value, page: 0, pageSize: 1, beforeUid: cursor.value.beforeUid })
    if (check !== cursorCheck) return
    remaining.value = result.total
    if (!result.total) mode.value = 'latest'
  } catch {
    // Antalet är bara en upplysning.
  }
}

async function saveCursor(beforeUid, date) {
  const result = await api.aiSortCursor(accountId.value, { folder: folder.value, beforeUid, date })
  ai.value.sortCursors = { ...ai.value.sortCursors, [accountId.value]: result.cursors }
}

async function restart() {
  try {
    await saveCursor(0, 0)
    loadCursor()
  } catch (e) {
    sortError.value = e.message
  }
}

watch(accountId, () => {
  presetUids.value = null
  if (!sourceFolders.value.some((f) => f.path === folder.value)) folder.value = 'INBOX'
  loadExcluded()
  loadCursor()
})
watch(folder, (value, old) => {
  if (old !== undefined) presetUids.value = null
  loadCursor()
})

// Det som hämtades senast: äldsta meddelandet (för märket) och hur många äldre som finns kvar.
const run = reactive({ oldest: null, older: 0, fromFolder: false })

async function messagesToSort() {
  const isCurrent = accountId.value === store.current.accountId && folder.value === store.current.folder
  run.fromFolder = false
  if (presetUids.value && isCurrent) {
    const wanted = new Set(presetUids.value)
    return store.messages.filter((m) => wanted.has(m.uid))
  }
  const query = { folder: folder.value, page: 0, pageSize: limit.value, unread: unreadOnly.value }
  if (mode.value === 'continue' && cursor.value) query.beforeUid = cursor.value.beforeUid
  const result = await api.messages(accountId.value, query)
  run.fromFolder = true
  run.older = Math.max(0, result.total - result.messages.length)
  return result.messages
}

async function start() {
  sortError.value = ''
  cancelled = false
  rows.value = []
  phase.value = 'running'
  progress.done = 0
  progress.total = 0
  run.oldest = null
  let messages = []
  try {
    messages = await messagesToSort()
    progress.total = messages.length
    if (!messages.length) {
      sortError.value =
        mode.value === 'continue' && cursor.value
          ? 'Hela mappen är genomgången: det finns inga äldre meddelanden kvar att sortera.'
          : 'Det finns inga meddelanden att sortera i mappen.'
      phase.value = 'setup'
      return
    }
    const byUid = new Map(messages.map((m) => [m.uid, m]))
    const targetPaths = targets.value.map((f) => f.path)
    const size = ai.value.batchSize || 25
    // En omgång i taget, så att förloppet syns och sorteringen går att avbryta mellan anropen.
    for (let i = 0; i < messages.length && !cancelled; i += size) {
      const uids = messages.slice(i, i + size).map((m) => m.uid)
      const result = await api.aiSort(accountId.value, { folder: folder.value, uids, targets: targetPaths })
      for (const s of result.suggestions) {
        const m = byUid.get(s.uid)
        if (!m) continue
        rows.value.push({
          uid: m.uid,
          from: displayName(m.from[0]),
          email: m.from[0]?.email || '',
          name: m.from[0]?.name || '',
          subject: m.subject,
          date: m.date,
          seen: m.seen,
          suggested: s.target,
          target: s.target || '',
          reason: s.reason,
          checked: !!s.target,
        })
      }
      progress.done = Math.min(messages.length, i + size)
    }
  } catch (e) {
    sortError.value = e.message
  }
  // Vid avbrott eller fel räknas bara det AI:n hann med som genomgånget; resten tas i nästa omgång.
  const handled = messages.slice(0, progress.done)
  run.older += messages.length - handled.length
  run.oldest = handled.length ? handled.reduce((a, b) => (a.uid < b.uid ? a : b)) : null
  phase.value = rows.value.length ? 'review' : 'setup'
}

function cancelRun() {
  cancelled = true
}

// Förslag att flytta överst, grupperade per målmapp; det som ligger kvar sist.
const sortedRows = computed(() =>
  [...rows.value].sort((a, b) => {
    if (!!a.suggested !== !!b.suggested) return a.suggested ? -1 : 1
    return (a.suggested || '').localeCompare(b.suggested || '', 'sv') || b.uid - a.uid
  }),
)
const toMove = computed(() => rows.value.filter((r) => r.checked && r.target))
const suggestedCount = computed(() => rows.value.filter((r) => r.suggested).length)
const allChecked = computed(() => {
  const movable = rows.value.filter((r) => r.target)
  return movable.length > 0 && movable.every((r) => r.checked)
})

// Nästa omgång erbjuds när det finns äldre meddelanden kvar i mappen.
// Inte när en sortering av de senaste gjordes ovanför ett märke längre bak: då hör nästa omgång inte ihop med den här.
const canContinue = computed(
  () => run.fromFolder && !!run.oldest && run.older > 0 && (!cursor.value || cursor.value.beforeUid >= run.oldest.uid),
)

function toggleAll(event) {
  for (const r of rows.value) r.checked = event.target.checked && !!r.target
}

function onTargetChange(row) {
  row.checked = !!row.target
}

// Flyttar märket till det äldsta meddelandet i omgången. Märket går aldrig framåt, så att en sortering av de
// senaste inte gör att äldre, redan genomgångna meddelanden kommer tillbaka.
async function advanceCursor() {
  if (!run.fromFolder || !run.oldest) return
  if (cursor.value && cursor.value.beforeUid <= run.oldest.uid) return
  await saveCursor(run.oldest.uid, run.oldest.date || 0)
}

async function applyMoves({ next = false } = {}) {
  moving.value = true
  sortError.value = ''
  const groups = new Map()
  for (const r of toMove.value) {
    if (!groups.has(r.target)) groups.set(r.target, [])
    groups.get(r.target).push(r.uid)
  }
  let moved = 0
  const movedUids = new Set()
  const reviewed = [...rows.value]
  try {
    for (const [target, uids] of groups) {
      await api.move(accountId.value, folder.value, uids, target)
      moved += uids.length
      uids.forEach((uid) => movedUids.add(uid))
    }
    await advanceCursor()
  } catch (e) {
    sortError.value = e.message
  }
  await sendFeedback(reviewed, movedUids)
  moving.value = false
  rows.value = rows.value.filter((r) => !movedUids.has(r.uid))
  if (moved) {
    store.notify(`${moved} meddelande(n) flyttades till ${groups.size} mapp(ar)`)
    store.loadFolders(accountId.value)
    if (accountId.value === store.current.accountId) store.loadMessages({ keepSelection: true })
  }
  if (sortError.value) return
  if (!next) return close()
  mode.value = 'continue'
  await start()
  // Inget äldre kvar: visa startsidan med beskedet i stället för en tom lista.
  if (phase.value === 'setup') loadCursor()
}

function backToSetup() {
  rows.value = []
  sortError.value = ''
  phase.value = 'setup'
}

// --- Inlärning -----------------------------------------------------------------

// Regler per avsändare, förslag på nya regler och rättningar som AI:n får som exempel.
const learning = ref({ rules: [], suggestions: [], corrections: [], ruleAfter: 3 })
const learningError = ref('')
const learningBusy = ref(false)
const newRule = reactive({ accountId: null, sender: '', target: '' })

async function loadLearning() {
  try {
    learning.value = await api.aiLearning()
  } catch (e) {
    learningError.value = e.message
  }
}

// Det användaren bestämde i granskningen: det som flyttades, och förslag som användaren tackade nej till.
async function sendFeedback(reviewed, movedUids) {
  const decisions = []
  for (const r of reviewed) {
    if (!r.email) continue
    const base = { email: r.email, name: r.name, subject: r.subject, suggested: r.suggested || '' }
    if (movedUids.has(r.uid)) decisions.push({ ...base, chosen: r.target })
    else if (r.suggested && (!r.checked || !r.target)) decisions.push({ ...base, chosen: '' })
  }
  if (!decisions.length) return
  try {
    learning.value = await api.aiFeedback(accountId.value, folder.value, decisions)
  } catch (e) {
    store.notify(`Besluten kunde inte sparas för inlärningen: ${e.message}`, 'error')
  }
}

async function learningCall(call) {
  learningBusy.value = true
  learningError.value = ''
  try {
    learning.value = await call()
  } catch (e) {
    learningError.value = e.message
  } finally {
    learningBusy.value = false
  }
}

const acceptSuggestion = (s) => learningCall(() => api.aiRuleSave(s.accountId, { sender: s.sender, target: s.target }))
const dismissSuggestion = (s) => learningCall(() => api.aiRuleSave(s.accountId, { sender: s.sender, dismissed: true }))
const deleteRule = (r) => learningCall(() => api.aiRuleDelete(r.id))
const deleteCorrection = (c) => learningCall(() => api.aiExampleDelete(c.id))

async function acceptAllSuggestions() {
  for (const s of [...learning.value.suggestions]) await acceptSuggestion(s)
}

async function clearCorrections() {
  if (!window.confirm('Ta bort allt som sorteringen har lärt sig av dina rättningar? Reglerna ligger kvar.')) return
  await learningCall(() => api.aiExamplesClear())
}

async function addRule() {
  await learningCall(() => api.aiRuleSave(newRule.accountId, { sender: newRule.sender, target: newRule.target }))
  if (!learningError.value) newRule.sender = ''
}

function ruleFolders(id) {
  return (store.folders[id] || []).filter((f) => f.selectable && !NEVER_TARGET.includes(f.specialUse))
}

function targetLabel(id, path) {
  return path ? folderLabelByPath(id, path) : 'ligger kvar'
}

function accountEmail(id) {
  return store.accounts.find((a) => a.id === id)?.email || ''
}

// --- Hela mappen i bakgrunden -------------------------------------------------

const jobs = ref([])
const jobsError = ref('')
const startingJob = ref(false)
const jobBusy = ref(null)
let jobTimer = null

const ACTIVE_STATES = ['running', 'paused', 'undoing']
const shownJobs = computed(() => jobs.value.filter((j, i) => ACTIVE_STATES.includes(j.state) || i < 3))
const accountHasJob = computed(() => jobs.value.some((j) => j.accountId === accountId.value && ACTIVE_STATES.includes(j.state)))
// Papperskorgen används aldrig när ingen granskar flyttarna.
const jobTargets = computed(() => targets.value.filter((f) => f.specialUse !== 'trash'))

function applyJobs(result) {
  const wasWorking = jobs.value.filter((j) => ['running', 'undoing'].includes(j.state)).map((j) => j.id)
  jobs.value = result.jobs
  jobLogDays.value = result.logDays || jobLogDays.value
  // Medan ett jobb arbetar hämtas läget ofta, annars inte alls.
  clearTimeout(jobTimer)
  if (result.jobs.some((j) => ['running', 'undoing'].includes(j.state))) jobTimer = setTimeout(loadJobs, 3000)
  // Ett jobb blev klart: mapparnas antal och listan har ändrats.
  const finished = result.jobs.filter((j) => wasWorking.includes(j.id) && !['running', 'undoing'].includes(j.state))
  for (const j of finished) {
    store.loadFolders(j.accountId)
    if (j.accountId === store.current.accountId) store.loadMessages({ keepSelection: true })
    if (j.accountId === accountId.value) api.ai().then((state) => { ai.value = state; loadCursor() }).catch(() => {})
  }
}

async function loadJobs() {
  try {
    applyJobs(await api.aiJobs())
  } catch (e) {
    jobsError.value = e.message
  }
}

async function startJob() {
  const folderLabel = folderLabelByPath(accountId.value, folder.value)
  const from = mode.value === 'continue' && cursor.value ? `meddelanden äldre än ${formatListDate(cursor.value.date)}` : 'alla meddelanden'
  const ok = window.confirm(
    `Sortera ${from} i ${folderLabel} i bakgrunden?\n\n` +
      'Servern går igenom mappen utan att du granskar förslagen. Bara det AI:n är säker på flyttas, aldrig till papperskorgen. ' +
      `Varje flytt sparas i historiken under Automatiskt, och hela sorteringen går att ångra i ${jobLogDays.value} dagar. ` +
      'Du kan stänga fönstret; sorteringen fortsätter på servern.',
  )
  if (!ok) return
  startingJob.value = true
  jobsError.value = ''
  try {
    applyJobs(
      await api.aiJobStart(accountId.value, {
        folder: folder.value,
        targets: jobTargets.value.map((f) => f.path),
        unreadOnly: unreadOnly.value,
        fromStart: mode.value !== 'continue',
      }),
    )
  } catch (e) {
    jobsError.value = e.message
  } finally {
    startingJob.value = false
  }
}

const jobLogDays = ref(30)

async function jobAction(job, action) {
  if (action === 'undo' && !window.confirm(`Flytta tillbaka de ${job.moved} meddelanden som sorteringen har flyttat från ${folderLabelByPath(job.accountId, job.folder)}?`)) return
  if (action === 'cancel' && !window.confirm('Avbryta sorteringen? Det som redan har flyttats ligger kvar.')) return
  jobBusy.value = job.id
  jobsError.value = ''
  try {
    applyJobs(await api.aiJobAction(job.id, action))
    // En omgång som redan pågick får bli klar; hämta läget en gång till när den är det.
    if (['pause', 'cancel'].includes(action)) {
      clearTimeout(jobTimer)
      jobTimer = setTimeout(loadJobs, 5000)
    }
  } catch (e) {
    jobsError.value = e.message
  } finally {
    jobBusy.value = null
  }
}

const jobStateText = {
  running: 'Pågår',
  paused: 'Pausad',
  done: 'Klar',
  cancelled: 'Avbruten',
  undoing: 'Flyttar tillbaka…',
  undone: 'Ångrad',
}

function jobMovedText(job) {
  const parts = Object.entries(job.movedTo || {}).sort((a, b) => b[1] - a[1]).map(([path, n]) => `${folderLabelByPath(job.accountId, path)} ${n}`)
  return parts.join(', ')
}

// --- Automatisk sortering -----------------------------------------------------

const auto = ref({ rules: [], log: [], logDays: 30 })
const autoForms = reactive({}) // accountId -> { enabled, intervalMinutes, targets, saving, error }
const autoError = ref('')
const undoing = ref(null)

// Papperskorgen går aldrig att välja här: ingen granskar flyttarna innan de görs.
function autoTargets(id) {
  return (store.folders[id] || []).filter(
    (f) => f.selectable && f.path !== 'INBOX' && ![...NEVER_TARGET, 'trash'].includes(f.specialUse),
  )
}

function fillAutoForms() {
  for (const a of store.accounts) {
    const rule = auto.value.rules.find((r) => r.accountId === a.id)
    // Utan sparad regel är allt utom skräpposten förvalt.
    const targets = rule ? rule.targets : autoTargets(a.id).filter((f) => f.specialUse !== 'junk').map((f) => f.path)
    autoForms[a.id] = {
      enabled: !!rule?.enabled,
      intervalMinutes: rule?.intervalMinutes || 5,
      targets: new Set(targets),
      saving: false,
      error: '',
    }
  }
}

function ruleFor(id) {
  return auto.value.rules.find((r) => r.accountId === id)
}

function toggleAutoTarget(id, path) {
  const targets = autoForms[id].targets
  if (targets.has(path)) targets.delete(path)
  else targets.add(path)
}

async function loadAuto() {
  autoError.value = ''
  try {
    auto.value = await api.aiAuto()
    fillAutoForms()
  } catch (e) {
    autoError.value = e.message
  }
}

async function saveAuto(id) {
  const f = autoForms[id]
  f.saving = true
  f.error = ''
  try {
    auto.value = await api.aiAutoSave(id, { enabled: f.enabled, intervalMinutes: f.intervalMinutes, targets: [...f.targets] })
    store.notify(f.enabled ? 'Automatisk sortering är på. Ny post i inkorgen sorteras från och med nu.' : 'Automatisk sortering är av')
  } catch (e) {
    f.error = e.message
  } finally {
    f.saving = false
  }
}

async function undo(entry) {
  undoing.value = entry.id
  autoError.value = ''
  try {
    auto.value = await api.aiUndo(entry.id)
    store.notify('Meddelandet flyttades tillbaka och sorteras inte igen')
    store.loadFolders(entry.accountId)
    if (entry.accountId === store.current.accountId) store.loadMessages({ keepSelection: true })
  } catch (e) {
    autoError.value = e.message
  } finally {
    undoing.value = null
  }
}

function folderLabelByPath(id, path) {
  const f = (store.folders[id] || []).find((x) => x.path === path)
  return f ? folderName(f) : path
}

watch(tab, (value) => {
  if (value === 'auto') loadAuto()
})

// --- Dialogen ---------------------------------------------------------------

function close() {
  cancelled = true
  store.aiDialog = false
}

function onKey(event) {
  if (event.key === 'Escape' && !moving.value) close()
}

onMounted(async () => {
  window.addEventListener('keydown', onKey)
  try {
    ai.value = await api.ai()
    fillForm()
    if (!ai.value.configured) tab.value = 'settings'
    if (accountId.value && !store.folders[accountId.value]) await store.loadFolders(accountId.value)
    loadExcluded()
    if (!presetUids.value) loadCursor()
    if (ai.value.configured) loadJobs()
    newRule.accountId = accountId.value
    loadLearning()
  } catch (e) {
    loadError.value = e.message
  } finally {
    loading.value = false
  }
})
onBeforeUnmount(() => {
  window.removeEventListener('keydown', onKey)
  clearTimeout(jobTimer)
})
</script>

<template>
  <div class="modal-backdrop">
    <div class="modal ai-sort">
      <div class="modal-header">
        <h2>Sortera med AI</h2>
        <button class="btn ghost" title="Stäng" :disabled="moving" @click="close"><Icon name="close" /></button>
      </div>

      <nav class="tabs">
        <button :class="{ active: tab === 'sort' }" @click="tab = 'sort'"><Icon name="sparkles" /> Sortera</button>
        <button :class="{ active: tab === 'auto' }" @click="tab = 'auto'"><Icon name="clock" /> Automatiskt</button>
        <button :class="{ active: tab === 'settings' }" @click="tab = 'settings'"><Icon name="settings" /> Inställningar</button>
      </nav>

      <div v-if="loading" class="body muted">Läser in…</div>
      <div v-else-if="loadError" class="body error-text">{{ loadError }}</div>

      <!-- Sortera -->
      <template v-else-if="tab === 'sort'">
        <section v-if="ai.configured && phase !== 'running' && learning.suggestions.length" class="rule-suggestions outer">
          <h3>
            Förslag på regler
            <button v-if="learning.suggestions.length > 1" class="link" :disabled="learningBusy" @click="acceptAllSuggestions">
              Skapa alla {{ learning.suggestions.length }}
            </button>
          </h3>
          <p class="muted note">
            Du har gjort samma val {{ learning.ruleAfter }} gånger i rad för de här avsändarna. Med en regel hamnar deras mejl rätt
            direkt, utan att AI:n tillfrågas.
          </p>
          <div v-for="s in learning.suggestions.slice(0, 5)" :key="`${s.accountId}:${s.sender}`" class="rule-suggestion">
            <span>
              Mejl från <strong :title="s.sender">{{ s.name || s.sender }}</strong>
              <span class="muted"> &lt;{{ s.sender }}&gt;</span>
              → <strong>{{ targetLabel(s.accountId, s.target) }}</strong>
            </span>
            <span class="spacer" />
            <button class="btn small" :disabled="learningBusy" @click="acceptSuggestion(s)">Skapa regel</button>
            <button class="btn small ghost" :disabled="learningBusy" @click="dismissSuggestion(s)">Nej tack</button>
          </div>
          <p v-if="learning.suggestions.length > 5" class="muted note">…och {{ learning.suggestions.length - 5 }} till.</p>
          <p v-if="learningError" class="error-text">{{ learningError }}</p>
        </section>

        <div v-if="!ai.configured" class="body">
          <p>Välj en AI-tjänst och ange en API-nyckel för att komma igång.</p>
          <button class="btn primary" @click="tab = 'settings'">Öppna inställningar</button>
        </div>

        <template v-else-if="phase === 'setup'">
          <div class="body">
            <section v-if="shownJobs.length" class="jobs">
              <h3>Hela mappar i bakgrunden</h3>
              <div v-for="j in shownJobs" :key="j.id" class="job" :class="`state-${j.state}`">
                <div class="job-head">
                  <strong>{{ folderLabelByPath(j.accountId, j.folder) }}</strong>
                  <span v-if="store.accounts.length > 1" class="muted">{{ store.accounts.find((a) => a.id === j.accountId)?.email }}</span>
                  <span class="job-state">{{ jobStateText[j.state] || j.state }}</span>
                  <span class="spacer" />
                  <button v-if="j.state === 'running'" class="btn small" :disabled="jobBusy === j.id" @click="jobAction(j, 'pause')">Pausa</button>
                  <button v-if="j.state === 'paused'" class="btn small" :disabled="jobBusy === j.id" @click="jobAction(j, 'resume')">Fortsätt</button>
                  <button v-if="['running', 'paused'].includes(j.state)" class="btn small" :disabled="jobBusy === j.id" @click="jobAction(j, 'cancel')">Avbryt</button>
                  <button v-if="['paused', 'done', 'cancelled'].includes(j.state) && j.moved > 0" class="btn small" :disabled="jobBusy === j.id" @click="jobAction(j, 'undo')">
                    Ångra allt
                  </button>
                </div>
                <progress v-if="['running', 'paused'].includes(j.state)" :value="j.processed" :max="j.total || 1" />
                <p class="muted note">
                  {{ Math.min(j.processed, j.total) }} av {{ j.total }} genomgångna, {{ j.moved }} flyttade<template v-if="jobMovedText(j)">: {{ jobMovedText(j) }}</template>.
                  <template v-if="j.state === 'undone' && j.moved > 0">{{ j.moved }} meddelanden hittades inte längre och ligger kvar där de är.</template>
                  <template v-if="j.finishedAt">{{ formatListDate(j.finishedAt) }}.</template>
                </p>
                <p v-if="j.lastError && ['running', 'paused', 'undoing', 'cancelled'].includes(j.state)" class="error-text note">
                  {{ j.lastError }}<template v-if="['running', 'undoing'].includes(j.state)">. Nytt försök {{ formatListDate(j.nextRunAt) }}.</template>
                </p>
              </div>
            </section>

            <div class="grid">
              <label>
                <span>Konto</span>
                <select v-model="accountId" class="input">
                  <option v-for="a in store.accounts" :key="a.id" :value="a.id">{{ a.name }} &lt;{{ a.email }}&gt;</option>
                </select>
              </label>
              <label>
                <span>Mapp att sortera</span>
                <select v-model="folder" class="input">
                  <option v-for="f in sourceFolders" :key="f.path" :value="f.path">{{ folderName(f) }}</option>
                </select>
              </label>
            </div>

            <p v-if="presetUids" class="scope">
              <strong>{{ presetUids.length }} markerade meddelanden</strong> sorteras.
              <button class="link" @click="presetUids = null">Välj de senaste i mappen i stället</button>
            </p>
            <template v-else>
              <div class="scope-row">
                <label class="inline">
                  <span>Sortera</span>
                  <select v-model="limit" class="input auto">
                    <option :value="25">25</option>
                    <option :value="50">50</option>
                    <option :value="100">100</option>
                    <option :value="200">200</option>
                    <option :value="500">500</option>
                  </select>
                  <span>meddelanden i taget</span>
                </label>
                <label class="inline"><input v-model="unreadOnly" type="checkbox" /> Bara olästa</label>
              </div>
              <div class="scope-modes">
                <label class="inline"><input v-model="mode" type="radio" value="latest" /> De senaste i mappen</label>
                <label class="inline" :class="{ muted: !cursor || remaining === 0 }">
                  <input v-model="mode" type="radio" value="continue" :disabled="!cursor || remaining === 0" />
                  <span v-if="!cursor">Fortsätt bakåt där du slutade (inget är sorterat i mappen ännu)</span>
                  <span v-else-if="remaining === 0">Hela mappen är genomgången</span>
                  <span v-else>
                    Fortsätt bakåt där du slutade: äldre än {{ formatListDate(cursor.date) }}
                    <template v-if="remaining !== null">({{ remaining }} kvar)</template>
                  </span>
                </label>
                <button v-if="cursor" class="link" @click="restart">Börja om från början</button>
              </div>
              <p class="muted note">
                För att gå igenom en hel mapp: hämta förslag, godkänn och välj <strong>Flytta och hämta nästa</strong>.
                Varje omgång tar de närmast äldre meddelandena, även om en del fick ligga kvar, och du kan stänga och
                fortsätta en annan dag.
              </p>
            </template>

            <details class="targets">
              <summary>
                Mappar som AI:n får föreslå
                <span class="muted">({{ targets.length }} av {{ possibleTargets.length }})</span>
              </summary>
              <div class="target-list">
                <label v-for="f in possibleTargets" :key="f.path" class="inline">
                  <input type="checkbox" :checked="!excluded.has(f.path)" @change="toggleTarget(f.path)" />
                  {{ folderName(f) }}
                </label>
              </div>
            </details>
            <p v-if="!targets.length" class="error-text">
              Det finns inga mappar att sortera till. Skapa mappar genom att högerklicka på kontot, eller välj fler ovan.
            </p>

            <p class="muted note">
              Avsändare, mottagare, ämne och de första {{ ai.snippetChars }} tecknen av texten skickas till
              {{ savedProvider?.name }} ({{ ai.settings.model }}). Inget flyttas förrän du har godkänt förslagen.
              <template v-if="learning.rules.length || learning.corrections.length">
                Sorteringen använder {{ learning.rules.length }} avsändarregler och lär sig av {{ learning.corrections.length }} rättningar (se Inställningar).
              </template>
              <template v-if="ai.settings.instructions">Dina egna regler används.</template>
              <template v-else>Du kan skriva egna regler under Inställningar.</template>
            </p>
            <p v-if="sortError" class="error-text">{{ sortError }}</p>
            <p v-if="jobsError" class="error-text">{{ jobsError }}</p>
          </div>
          <div class="modal-footer">
            <button class="btn" @click="close">Stäng</button>
            <button
              v-if="!presetUids"
              class="btn"
              :disabled="!accountId || !jobTargets.length || accountHasJob || startingJob"
              :title="accountHasJob ? 'Kontot har redan en sortering i bakgrunden' : 'Servern sorterar hela mappen utan granskning. Går att pausa och ångra.'"
              @click="startJob"
            >
              <Icon name="clock" /> {{ startingJob ? 'Startar…' : 'Sortera hela mappen i bakgrunden' }}
            </button>
            <button class="btn primary" :disabled="!accountId || !targets.length" @click="start">
              <Icon name="sparkles" /> Hämta förslag
            </button>
          </div>
        </template>

        <template v-else-if="phase === 'running'">
          <div class="body running">
            <p v-if="progress.total">AI:n läser meddelandena… {{ progress.done }} av {{ progress.total }}</p>
            <p v-else>Hämtar meddelanden…</p>
            <progress :value="progress.done" :max="progress.total || 1" />
            <p v-if="rows.length" class="muted">{{ suggestedCount }} förslag hittills</p>
          </div>
          <div class="modal-footer">
            <button class="btn" @click="cancelRun">Avbryt</button>
          </div>
        </template>

        <template v-else>
          <div class="summary">
            <span>
              <strong>{{ suggestedCount }}</strong> av {{ rows.length }} meddelanden föreslås flyttas.
              Ändra målmapp eller bocka ur det du inte håller med om.
              <template v-if="run.fromFolder && run.oldest">
                Omgången går tillbaka till {{ formatListDate(run.oldest.date) }}; {{ run.older }} äldre återstår.
              </template>
            </span>
          </div>
          <p v-if="sortError" class="error-text summary">{{ sortError }}</p>
          <div class="table-wrap">
            <table>
              <thead>
                <tr>
                  <th class="check"><input type="checkbox" :checked="allChecked" title="Markera alla" @change="toggleAll" /></th>
                  <th>Från</th>
                  <th>Ämne</th>
                  <th>Flytta till</th>
                  <th>Motivering</th>
                </tr>
              </thead>
              <tbody>
                <tr v-for="r in sortedRows" :key="r.uid" :class="{ stay: !r.target, unread: !r.seen }">
                  <td class="check"><input v-model="r.checked" type="checkbox" :disabled="!r.target" /></td>
                  <td class="from" :title="r.from">{{ r.from }}</td>
                  <td class="subject" :title="r.subject">
                    {{ r.subject || '(inget ämne)' }}
                    <span class="muted date">{{ formatListDate(r.date) }}</span>
                  </td>
                  <td class="target">
                    <select v-model="r.target" class="input" @change="onTargetChange(r)">
                      <option value="">– Ligger kvar –</option>
                      <option v-for="f in possibleTargets" :key="f.path" :value="f.path">{{ folderName(f) }}</option>
                    </select>
                  </td>
                  <td class="reason muted" :title="r.reason">{{ r.target === (r.suggested || '') ? r.reason : 'Ändrad av dig' }}</td>
                </tr>
              </tbody>
            </table>
          </div>
          <div class="modal-footer">
            <button class="btn" :disabled="moving" @click="backToSetup">Tillbaka</button>
            <button class="btn" :class="{ primary: !canContinue }" :disabled="moving || !toMove.length" @click="applyMoves()">
              {{ moving ? 'Flyttar…' : `Flytta ${toMove.length} meddelande(n)` }}
            </button>
            <button v-if="canContinue" class="btn primary" :disabled="moving" @click="applyMoves({ next: true })">
              {{ toMove.length ? `Flytta ${toMove.length} och hämta nästa ${limit}` : `Hämta nästa ${limit}` }}
            </button>
          </div>
        </template>
      </template>

      <!-- Automatiskt -->
      <template v-else-if="tab === 'auto'">
        <div class="body">
          <div v-if="!ai.configured">
            <p>Välj en AI-tjänst och ange en API-nyckel för att komma igång.</p>
            <button class="btn primary" @click="tab = 'settings'">Öppna inställningar</button>
          </div>
          <template v-else>
            <p class="muted note">
              Servern sorterar ny post i inkorgen även när du inte är inloggad. Bara post som kommer efter att du slagit på
              sorteringen berörs, och AI:n flyttar bara det den är säker på. Inget flyttas till papperskorgen, och varje
              flytt går att ångra i historiken nedan. Dina egna regler under Inställningar används.
            </p>
            <p v-if="autoError" class="error-text">{{ autoError }}</p>

            <section v-for="a in store.accounts" :key="a.id" class="auto-account">
              <template v-if="autoForms[a.id]">
                <div class="auto-head">
                  <label class="inline">
                    <input v-model="autoForms[a.id].enabled" type="checkbox" />
                    <strong>{{ a.name }} &lt;{{ a.email }}&gt;</strong>
                  </label>
                  <label class="inline">
                    <span>Kontrollera var</span>
                    <select v-model="autoForms[a.id].intervalMinutes" class="input auto">
                      <option :value="2">2 min</option>
                      <option :value="5">5 min</option>
                      <option :value="15">15 min</option>
                      <option :value="60">60 min</option>
                    </select>
                  </label>
                  <button class="btn" :disabled="autoForms[a.id].saving" @click="saveAuto(a.id)">
                    {{ autoForms[a.id].saving ? 'Sparar…' : 'Spara' }}
                  </button>
                </div>
                <details class="targets" :open="autoForms[a.id].enabled && !ruleFor(a.id)?.enabled">
                  <summary>
                    Mappar som AI:n får flytta till
                    <span class="muted">({{ autoForms[a.id].targets.size }} av {{ autoTargets(a.id).length }})</span>
                  </summary>
                  <div class="target-list">
                    <label v-for="f in autoTargets(a.id)" :key="f.path" class="inline">
                      <input type="checkbox" :checked="autoForms[a.id].targets.has(f.path)" @change="toggleAutoTarget(a.id, f.path)" />
                      {{ folderName(f) }}
                    </label>
                  </div>
                </details>
                <p v-if="autoForms[a.id].error" class="error-text">{{ autoForms[a.id].error }}</p>
                <p v-else-if="ruleFor(a.id)?.enabled && ruleFor(a.id).lastError" class="error-text">
                  Senaste kontrollen misslyckades: {{ ruleFor(a.id).lastError }}. Ett nytt försök görs om en stund.
                </p>
                <p v-else-if="ruleFor(a.id)?.enabled" class="muted note">
                  På.
                  <template v-if="ruleFor(a.id).lastRunAt">Senast kontrollerad {{ formatListDate(ruleFor(a.id).lastRunAt) }}.</template>
                  <template v-else>Första kontrollen görs strax.</template>
                </p>
              </template>
            </section>

            <h3>Historik <span class="muted">(sparas i {{ auto.logDays }} dagar)</span></h3>
            <p v-if="!auto.log.length" class="muted">Inget har sorterats automatiskt ännu.</p>
            <div v-else class="table-wrap history">
              <table>
                <thead>
                  <tr>
                    <th>När</th>
                    <th>Från</th>
                    <th>Ämne</th>
                    <th>Flyttades till</th>
                    <th>Motivering</th>
                    <th class="undo" />
                  </tr>
                </thead>
                <tbody>
                  <tr v-for="e in auto.log" :key="e.id" :class="{ stay: e.undoneAt }">
                    <td>{{ formatListDate(e.movedAt) }}</td>
                    <td :title="e.sender">{{ e.sender }}</td>
                    <td :title="e.subject">{{ e.subject || '(inget ämne)' }}</td>
                    <td :title="e.targetFolder">{{ folderLabelByPath(e.accountId, e.targetFolder) }}</td>
                    <td class="muted" :title="e.reason">{{ e.reason }}</td>
                    <td class="undo target">
                      <span v-if="e.undoneAt" class="muted">Ångrad</span>
                      <button v-else class="btn small" :disabled="undoing === e.id" @click="undo(e)">Ångra</button>
                    </td>
                  </tr>
                </tbody>
              </table>
            </div>
          </template>
        </div>
      </template>

      <!-- Inställningar -->
      <template v-else>
        <form class="body" @submit.prevent="saveSettings">
          <p v-if="ai.settings && !ai.settings.providerAvailable" class="error-text">
            Den sparade tjänsten ({{ ai.settings.baseUrl }}) är inte längre tillgänglig på servern. Välj en annan.
          </p>
          <div class="grid">
            <label>
              <span>AI-tjänst</span>
              <select v-model="form.baseUrl" class="input" @change="onProviderChange">
                <option v-for="p in ai.providers" :key="p.baseUrl" :value="p.baseUrl">{{ p.name }}</option>
              </select>
            </label>
            <label>
              <span>Modell</span>
              <input v-model="form.model" class="input" list="ai-models" autocomplete="off" spellcheck="false" required />
              <datalist id="ai-models">
                <option v-for="m in models" :key="m" :value="m" />
              </datalist>
            </label>
          </div>
          <label>
            <span>
              API-nyckel
              <a v-if="keyLinks[form.baseUrl]" :href="keyLinks[form.baseUrl]" target="_blank" rel="noopener noreferrer">Skapa en nyckel</a>
            </span>
            <input
              v-model="form.apiKey"
              class="input"
              type="password"
              autocomplete="off"
              :placeholder="keepsSavedKey ? 'Sparad för den här tjänsten – lämna tomt för att behålla' : provider?.keyRequired ? '' : 'Behövs inte för den här tjänsten'"
            />
          </label>
          <label>
            <span>Egna regler (valfritt)</span>
            <textarea
              v-model="form.instructions"
              class="input"
              rows="6"
              maxlength="4000"
              placeholder="Till exempel:&#10;Kvitton och fakturor ska till Ekonomi.&#10;Nyhetsbrev ska till Läs senare.&#10;Mejl från skolan ska ligga kvar i inkorgen."
            />
          </label>

          <p class="muted note">
            Nyckeln sparas krypterad på servern, en per tjänst, och visas aldrig igen. Byter du tjänst finns den gamla tjänstens
            nyckel kvar tills du väljer Ta bort. När du sorterar skickas avsändare, mottagare, ämne
            och de första {{ ai.snippetChars }} tecknen av varje meddelande till tjänsten du har valt. Läs tjänstens
            villkor: på Geminis gratisnivå får Google använda innehållet för att förbättra sina produkter, men inte på
            betalnivån.
          </p>

          <p v-if="testResult?.ok" class="ok-text">
            Anslutningen fungerar. {{ testResult.models.length }} modeller finns att välja i fältet Modell.
            <template v-if="testResult.warning"><br />{{ testResult.warning }}</template>
          </p>
          <p v-else-if="testResult" class="error-text">{{ testResult.error }}</p>
          <p v-if="settingsError" class="error-text">{{ settingsError }}</p>

          <section class="learned">
            <h3>Inlärt</h3>
            <p class="muted note">
              När du byter mapp på ett förslag, tackar nej till det eller ångrar en automatisk flytt sparas det som en rättning.
              AI:n får de mest närliggande rättningarna som exempel vid varje sortering. Avsändarregler gäller före AI:n, även
              vid automatisk sortering och i bakgrunden (dock aldrig till papperskorgen utan granskning).
            </p>
            <p v-if="learningError" class="error-text">{{ learningError }}</p>

            <h4>Avsändarregler <span class="muted">({{ learning.rules.length }})</span></h4>
            <div v-if="learning.rules.length" class="learned-list">
              <div v-for="r in learning.rules" :key="r.id" class="learned-row">
                <span class="grow" :title="accountEmail(r.accountId)">
                  <strong>{{ r.sender }}</strong> → {{ targetLabel(r.accountId, r.target) }}
                  <span v-if="r.target && !store.folderInfo(r.accountId, r.target)" class="error-text">(mappen finns inte längre)</span>
                  <span v-if="store.accounts.length > 1" class="muted"> · {{ accountEmail(r.accountId) }}</span>
                </span>
                <button class="btn small ghost" type="button" title="Ta bort regeln" :disabled="learningBusy" @click="deleteRule(r)"><Icon name="close" :size="12" /></button>
              </div>
            </div>
            <div class="rule-form">
              <select v-if="store.accounts.length > 1" v-model="newRule.accountId" class="input auto">
                <option v-for="a in store.accounts" :key="a.id" :value="a.id">{{ a.email }}</option>
              </select>
              <input v-model="newRule.sender" class="input" placeholder="namn@exempel.se eller @exempel.se" spellcheck="false" @keydown.enter.prevent="addRule" />
              <select v-model="newRule.target" class="input auto">
                <option value="">– Ligger kvar –</option>
                <option v-for="f in ruleFolders(newRule.accountId)" :key="f.path" :value="f.path">{{ folderName(f) }}</option>
              </select>
              <button class="btn" type="button" :disabled="learningBusy || !newRule.sender.trim() || !newRule.accountId" @click="addRule">Lägg till regel</button>
            </div>

            <h4>
              Rättningar <span class="muted">({{ learning.corrections.length }}{{ learning.corrections.length >= 200 ? '+' : '' }})</span>
              <button v-if="learning.corrections.length" class="link" type="button" :disabled="learningBusy" @click="clearCorrections">Ta bort alla</button>
            </h4>
            <p v-if="!learning.corrections.length" class="muted note">Inga rättningar ännu.</p>
            <div v-else class="learned-list">
              <div v-for="c in learning.corrections" :key="c.id" class="learned-row">
                <span class="grow" :title="`${c.name} <${c.email}> – ${c.subject}`">
                  <strong>{{ c.name || c.email }}</strong> <span class="muted">{{ c.subject || '(inget ämne)' }}</span>
                </span>
                <span class="verdict">
                  <s>{{ targetLabel(c.accountId, c.suggested) }}</s> → {{ targetLabel(c.accountId, c.chosen) }}
                </span>
                <button class="btn small ghost" type="button" title="Ta bort rättningen" :disabled="learningBusy" @click="deleteCorrection(c)"><Icon name="close" :size="12" /></button>
              </div>
            </div>
          </section>
        </form>
        <div class="modal-footer">
          <button v-if="ai.settings" class="btn danger left" type="button" @click="removeSettings">Ta bort</button>
          <button class="btn" type="button" :disabled="testing || !canSave" @click="testSettings">
            {{ testing ? 'Testar…' : 'Testa' }}
          </button>
          <button class="btn primary" type="button" :disabled="saving || !canSave" @click="saveSettings">
            {{ saving ? 'Sparar…' : 'Spara' }}
          </button>
        </div>
      </template>
    </div>
  </div>
</template>

<style scoped>
.ai-sort {
  max-width: 900px;
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

.body {
  padding: 16px;
  display: flex;
  flex-direction: column;
  gap: 12px;
  overflow: auto;
}

.body p {
  margin: 0;
}

.grid {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 12px;
}

label {
  display: flex;
  flex-direction: column;
  gap: 4px;
}

label > span {
  font-size: 12px;
  color: var(--text-muted);
  display: flex;
  justify-content: space-between;
  gap: 8px;
}

label.inline {
  flex-direction: row;
  align-items: center;
  gap: 6px;
}

label.inline > span {
  font-size: inherit;
  color: inherit;
}

.input.auto {
  width: auto;
}

.scope-row {
  display: flex;
  flex-wrap: wrap;
  gap: 8px 24px;
}

.scope-modes {
  display: flex;
  flex-direction: column;
  align-items: flex-start;
  gap: 4px;
}

.link {
  border: 0;
  background: none;
  padding: 0;
  color: var(--accent);
  cursor: pointer;
  text-decoration: underline;
}

.targets summary {
  cursor: pointer;
}

.target-list {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(200px, 1fr));
  gap: 4px 12px;
  margin-top: 8px;
  max-height: 180px;
  overflow: auto;
}

.note {
  font-size: 12px;
}

.rule-suggestions {
  border: 1px solid var(--border);
  border-radius: var(--radius);
  padding: 8px 12px;
  display: flex;
  flex-direction: column;
  gap: 6px;
}

.rule-suggestions.outer {
  margin: 12px 16px 0;
}

.rule-suggestions h3,
.learned h4 {
  margin: 0;
  display: flex;
  align-items: baseline;
  gap: 10px;
}

.rule-suggestions h3 .link,
.learned h4 .link {
  font-weight: 400;
  font-size: 12px;
}

.rule-suggestion,
.learned-row,
.rule-form {
  display: flex;
  align-items: center;
  gap: 8px;
  min-width: 0;
}

.rule-suggestion > span:first-child,
.learned-row .grow {
  min-width: 0;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.learned {
  border-top: 1px solid var(--border);
  padding-top: 12px;
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.learned h3 {
  margin: 0;
}

.learned h4 {
  font-size: 12px;
  margin-top: 4px;
}

.learned-list {
  border: 1px solid var(--border);
  border-radius: var(--radius);
  max-height: 200px;
  overflow: auto;
}

.learned-row {
  padding: 3px 4px 3px 10px;
  border-top: 1px solid var(--border);
}

.learned-row:first-child {
  border-top: 0;
}

.learned-row .grow {
  flex: 1;
}

.verdict {
  flex-shrink: 0;
  font-size: 12px;
  max-width: 45%;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.rule-form {
  flex-wrap: wrap;
}

.rule-form input {
  flex: 1;
  min-width: 200px;
}

.jobs {
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.jobs h3 {
  margin: 0;
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
.job.state-undoing .job-state {
  background: var(--accent);
  color: var(--accent-text);
}

.job progress {
  width: 100%;
}

.spacer {
  flex: 1;
}

.auto-account {
  border: 1px solid var(--border);
  border-radius: var(--radius);
  padding: 10px 12px;
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.auto-head {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 8px 16px;
}

.auto-head > label:first-child {
  flex: 1;
  min-width: 200px;
}

h3 {
  margin: 8px 0 0;
  font-size: 13px;
}

h3 .muted {
  font-weight: 400;
}

.history {
  margin-top: 0;
  border: 1px solid var(--border);
  border-radius: var(--radius);
  max-height: 260px;
  flex-shrink: 0;
}

.history th:first-child {
  width: 96px;
}

.undo {
  width: 76px;
  text-align: right;
}

.btn.small {
  padding: 2px 8px;
}

.ok-text {
  color: var(--success);
}

.running {
  align-items: center;
  padding: 40px 16px;
}

.running progress {
  width: min(420px, 100%);
}

.summary {
  padding: 10px 16px 0;
  margin: 0;
}

.table-wrap {
  overflow: auto;
  margin-top: 10px;
  border-top: 1px solid var(--border);
  min-height: 120px;
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

.check {
  width: 32px;
  text-align: center;
}

th:nth-child(2) {
  width: 18%;
}

th:nth-child(4) {
  width: 22%;
}

th:nth-child(5) {
  width: 24%;
}

tr.unread .from,
tr.unread .subject {
  font-weight: 600;
}

tr.stay td:not(.target) {
  opacity: 0.6;
}

.date {
  font-weight: 400;
  margin-left: 6px;
  font-size: 12px;
}

.target .input {
  padding: 3px 6px;
}

.modal-footer .left {
  margin-right: auto;
}

@media (max-width: 640px) {
  .grid {
    grid-template-columns: 1fr;
  }

  th:nth-child(5),
  td.reason {
    display: none;
  }
}
</style>
