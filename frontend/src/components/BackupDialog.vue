<script setup>
import { computed, onBeforeUnmount, onMounted, reactive, ref, watch } from 'vue'
import { api, backupDownloadUrl } from '../api'
import { locale, t } from '../i18n'
import { isGoogleMailAddress, isGoogleMailHost } from '../oauth'
import { useMailStore } from '../store'
import { formatFullDate, formatSize } from '../utils'
import GoogleSignIn from './GoogleSignIn.vue'
import Icon from './Icon.vue'

const store = useMailStore()

const tab = ref('new') // new | jobs | runs
const overview = ref({ driveConfigured: false, driveFolderName: '', driveConnections: [], jobs: [], runs: [] })
const loading = ref(true)
const loadError = ref('')
const busy = ref(false)
const formError = ref('')
const editingJobId = ref(null)
const showFolders = ref(false)
const expandedRuns = ref(new Set())

const timezone = Intl.DateTimeFormat().resolvedOptions().timeZone || 'UTC'
// Engelska nycklar; översätts med t() vid visning.
const weekdays = ['Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday', 'Sunday']
const statusLabels = { queued: 'Queued', running: 'Running', done: 'Done', failed: 'Failed', cancelled: 'Cancelled' }

function emptyForm() {
  return {
    name: '',
    // Dialogen kan öppnas för ett visst konto, t.ex. från högerklicksmenyn i mapplistan.
    sourceAccountId: store.backupDialog?.accountId ?? store.current.accountId ?? store.accounts[0]?.id ?? null,
    type: 'zip',
    targetAccountId: null,
    targetFolder: '',
    connectionId: null,
    keep: 7,
    excludeFolders: [],
    frequency: 'none',
    time: '03:00',
    weekday: 1,
    monthday: 1,
    enabled: true,
  }
}

const form = reactive(emptyForm())

const sourceAccount = computed(() => store.account(form.sourceAccountId))
const targetAccounts = computed(() => store.accounts.filter((a) => a.id !== form.sourceAccountId))
const sourceFolders = computed(() => (store.folders[form.sourceAccountId] || []).filter((f) => f.selectable))
const includedCount = computed(() => sourceFolders.value.filter((f) => !form.excludeFolders.includes(f.path)).length)
const connections = computed(() => overview.value.driveConnections)
const activeRuns = computed(() => overview.value.runs.filter((r) => r.status === 'queued' || r.status === 'running'))
const scheduled = computed(() => form.frequency !== 'none')

// Är källkontot ett Google-konto föreslås samma adress för Drive, så att inloggningen bara behöver godkännas.
// Adressen är bara ett förslag; i Googles fönster går det att välja ett annat konto.
const driveLoginHint = computed(() => {
  const account = sourceAccount.value
  if (!account) return ''
  const isGoogle =
    account.authType === 'oauth-google' || isGoogleMailHost(account.imapHost) || isGoogleMailAddress(account.email)
  if (!isGoogle || connections.value.some((c) => c.email === account.email)) return ''
  return account.email
})

const defaultName = computed(() => {
  const source = sourceAccount.value?.email || t('Account')
  if (form.type === 'imap') return `${source} → ${store.account(form.targetAccountId)?.email || 'IMAP'}`
  if (form.type === 'gdrive') return `${source} → Google Drive`
  return `${source} → ${t('zip file')}`
})

// Gmails "Alla mail" och "Stjärnmärkt" innehåller kopior av meddelanden i andra mappar.
function defaultExcluded(accountId) {
  return (store.folders[accountId] || []).filter((f) => ['all', 'flagged', 'important'].includes(f.specialUse)).map((f) => f.path)
}

watch(
  () => form.sourceAccountId,
  async (id, previous) => {
    if (!id) return
    if (!store.folders[id]) await store.loadFolders(id)
    if (editingJobId.value !== null) return
    form.excludeFolders = defaultExcluded(id)
    if (previous !== undefined) {
      if (form.targetAccountId === id) form.targetAccountId = targetAccounts.value[0]?.id ?? null
      if (form.type === 'imap') form.targetFolder = `Backup/${store.account(id)?.email || ''}`
    }
  },
  { immediate: true },
)

watch(
  () => form.type,
  (type) => {
    if (type === 'zip') form.frequency = 'none'
    if (editingJobId.value !== null) return // Behåll den sparade backupens inställningar.
    if (type === 'imap') {
      if (!form.targetAccountId) form.targetAccountId = targetAccounts.value[0]?.id ?? null
      if (!form.targetFolder) form.targetFolder = `Backup/${sourceAccount.value?.email || ''}`
    }
    if (type === 'gdrive' && !form.connectionId) form.connectionId = connections.value[0]?.id ?? null
  },
)

function toggleFolder(path) {
  const set = new Set(form.excludeFolders)
  if (set.has(path)) set.delete(path)
  else set.add(path)
  form.excludeFolders = [...set]
}

function setAllFolders(include) {
  form.excludeFolders = include ? [] : sourceFolders.value.map((f) => f.path)
}

function buildConfig() {
  let destination = { type: 'zip' }
  if (form.type === 'imap') destination = { type: 'imap', accountId: form.targetAccountId, folder: form.targetFolder }
  if (form.type === 'gdrive') destination = { type: 'gdrive', connectionId: form.connectionId, keep: Number(form.keep) }
  return { sourceAccountId: form.sourceAccountId, destination, excludeFolders: form.excludeFolders }
}

function buildSchedule() {
  return {
    frequency: form.frequency,
    time: form.time,
    weekday: Number(form.weekday),
    monthday: Number(form.monthday),
    timezone,
  }
}

// --- Laddning och uppdatering -------------------------------------------------

let pollTimer = null
let knownStatus = new Map()

async function load() {
  try {
    const data = await api.backup()
    // Meddela när en körning som pågick blir klar.
    for (const run of data.runs) {
      const before = knownStatus.get(run.id)
      if ((before === 'queued' || before === 'running') && before !== run.status) {
        if (run.status === 'done') store.notify(t('Backup finished: {message}', { message: run.message }))
        else if (run.status === 'failed') store.notify(t('Backup failed: {message}', { message: run.message }), 'error')
      }
    }
    knownStatus = new Map(data.runs.map((r) => [r.id, r.status]))
    overview.value = data
    loadError.value = ''
    if (form.type === 'gdrive' && !connections.value.some((c) => c.id === form.connectionId)) {
      form.connectionId = connections.value[0]?.id ?? null
    }
  } catch (e) {
    loadError.value = e.message
  } finally {
    loading.value = false
    clearTimeout(pollTimer)
    pollTimer = setTimeout(load, activeRuns.value.length ? 2000 : 15000)
  }
}

onMounted(() => {
  load()
  window.addEventListener('keydown', onKey)
})

onBeforeUnmount(() => {
  clearTimeout(pollTimer)
  window.removeEventListener('keydown', onKey)
})

// --- Åtgärder -------------------------------------------------------------------

async function run(action, { reload = true } = {}) {
  busy.value = true
  formError.value = ''
  try {
    return await action()
  } catch (e) {
    // I formuläret visas felet vid knapparna, på de andra flikarna som en notis.
    if (tab.value === 'new') formError.value = e.message
    else store.notify(e.message, 'error')
    return undefined
  } finally {
    busy.value = false
    if (reload) await load()
  }
}

async function startNow() {
  const result = await run(() => api.startBackup(buildConfig()))
  if (result) {
    tab.value = 'runs'
    store.notify(t('Backup started'))
  }
}

async function saveJob({ runAfterSave = false } = {}) {
  const payload = {
    name: form.name.trim() || defaultName.value,
    config: buildConfig(),
    schedule: buildSchedule(),
    enabled: form.enabled,
  }
  const saved = await run(async () => {
    const job = editingJobId.value
      ? await api.updateBackupJob(editingJobId.value, payload)
      : await api.createBackupJob(payload)
    if (runAfterSave) await api.runBackupJob(job.id)
    return job
  })
  if (saved) {
    store.notify(editingJobId.value ? t('Backup saved') : t('Backup created'))
    resetForm()
    tab.value = runAfterSave ? 'runs' : 'jobs'
  }
}

function resetForm() {
  editingJobId.value = null
  Object.assign(form, emptyForm())
  form.excludeFolders = defaultExcluded(form.sourceAccountId)
  formError.value = ''
}

function editJob(job) {
  const c = job.config
  const s = job.schedule
  editingJobId.value = job.id
  Object.assign(form, {
    name: job.name,
    sourceAccountId: c.sourceAccountId,
    type: c.destination?.type || 'zip',
    targetAccountId: c.destination?.accountId ?? null,
    targetFolder: c.destination?.folder ?? '',
    connectionId: c.destination?.connectionId ?? null,
    keep: c.destination?.keep ?? 7,
    excludeFolders: [...(c.excludeFolders || [])],
    frequency: s.frequency || 'none',
    time: s.time || '03:00',
    weekday: s.weekday || 1,
    monthday: s.monthday || 1,
    enabled: job.enabled,
  })
  formError.value = ''
  tab.value = 'new'
}

async function runJob(job) {
  const result = await run(() => api.runBackupJob(job.id))
  if (result) {
    tab.value = 'runs'
    store.notify(t('{name} started', { name: job.name }))
  }
}

async function toggleJob(job) {
  await run(() =>
    api.updateBackupJob(job.id, { name: job.name, config: job.config, schedule: job.schedule, enabled: !job.enabled }),
  )
}

async function deleteJob(job) {
  if (!window.confirm(t('Delete the backup "{name}"? Earlier runs remain in the history.', { name: job.name }))) return
  await run(() => api.deleteBackupJob(job.id))
  if (editingJobId.value === job.id) resetForm()
}

async function cancelRun(r) {
  await run(() => api.cancelBackupRun(r.id))
}

async function deleteRun(r) {
  if (r.hasFile && !window.confirm(t('Delete the run and its zip file?'))) return
  await run(() => api.deleteBackupRun(r.id))
}

async function onDriveSignedIn(result) {
  const connection = await run(() => api.addDriveConnection(result.state))
  if (connection) {
    form.connectionId = connection.id
    store.notify(t('Google Drive ({email}) is connected', { email: connection.email }))
  }
}

async function removeConnection(connection) {
  if (!window.confirm(t('Disconnect Google Drive for {email}? Backups already in Drive are kept.', { email: connection.email }))) return
  await run(() => api.deleteDriveConnection(connection.id))
}

function toggleRunDetails(id) {
  const next = new Set(expandedRuns.value)
  if (next.has(id)) next.delete(id)
  else next.add(id)
  expandedRuns.value = next
}

function close() {
  store.backupDialog = false
}

function onKey(event) {
  if (event.key === 'Escape' && !busy.value) close()
}

// --- Text för visning ---------------------------------------------------------

function accountEmail(id, fallback) {
  const account = store.account(id)
  if (account) return account.email
  return t('{account} (deleted)', { account: fallback || t('account {id}', { id }) })
}

function destinationText(config) {
  const d = config?.destination || {}
  if (d.type === 'zip') return t('zip file for download')
  if (d.type === 'imap') return `${accountEmail(d.accountId, d.email)}${d.folder ? t(', folder {folder}', { folder: d.folder }) : ''}`
  if (d.type === 'gdrive') return `Google Drive (${d.email})${d.keep ? t(', keeps {n}', { n: d.keep }) : ''}`
  return t('unknown destination')
}

function destinationIcon(config) {
  return { zip: 'zip', imap: 'mail', gdrive: 'cloud' }[config?.destination?.type] || 'backup'
}

// Ordningstal för dag i månaden: 1:a, 2:a, 3:e … på svenska, 1st, 2nd, 3rd … på engelska.
function ordinal(day) {
  const n = Number(day)
  if (locale() === 'sv') return [1, 2, 21, 22].includes(n) ? `${n}:a` : `${n}:e`
  const suffix = n % 100 >= 11 && n % 100 <= 13 ? 'th' : { 1: 'st', 2: 'nd', 3: 'rd' }[n % 10] || 'th'
  return `${n}${suffix}`
}

function scheduleText(s) {
  if (!s || s.frequency === 'none' || !s.frequency) return t('Runs when you start it')
  if (s.frequency === 'daily') return t('Every day at {time}', { time: s.time })
  if (s.frequency === 'weekly') return t('Every {weekday} at {time}', { weekday: t(weekdays[(s.weekday || 1) - 1]), time: s.time })
  return t('On the {day} of every month at {time}', { day: ordinal(s.monthday), time: s.time })
}

function jobName(r) {
  const job = r.jobId && overview.value.jobs.find((j) => j.id === r.jobId)
  return job?.name || r.config?.jobName || t('One-off backup')
}

function progressText(r) {
  const p = r.progress || {}
  if (r.status === 'queued') return t('Waiting to start…')
  if (p.phase === 'messages') {
    const folder = p.foldersTotal
      ? t(' · folder {n} of {total}', { n: Math.min(p.foldersDone + 1, p.foldersTotal), total: p.foldersTotal })
      : ''
    return `${t('{done} of {total} messages', { done: p.messagesDone ?? 0, total: p.messagesTotal ?? 0 })}${folder}${p.folder ? ` (${p.folder})` : ''}`
  }
  if (p.phase === 'finishing') return t('Writing the zip file…')
  if (p.phase === 'uploading') {
    return t('Uploading to Google Drive: {done} of {total}', { done: formatSize(p.uploaded || 0), total: formatSize(p.uploadTotal || 0) })
  }
  if (p.phase === 'cleanup') return t('Removing older backups…')
  return t('Connecting…')
}

function progressPercent(r) {
  const p = r.progress || {}
  if (p.phase === 'uploading' && p.uploadTotal) return Math.round((100 * p.uploaded) / p.uploadTotal)
  if (p.phase === 'messages' && p.messagesTotal) return Math.round((100 * p.messagesDone) / p.messagesTotal)
  if (p.phase === 'finishing' || p.phase === 'cleanup') return 100
  return 0
}
</script>

<template>
  <div class="modal-backdrop">
    <div class="modal backup">
      <div class="modal-header">
        <h2>{{ t('Back up email accounts') }}</h2>
        <button class="btn ghost" :title="t('Close')" @click="close"><Icon name="close" /></button>
      </div>

      <nav class="tabs">
        <button :class="{ active: tab === 'new' }" @click="tab = 'new'">
          <Icon name="backup" /> {{ editingJobId ? t('Edit backup') : t('New backup') }}
        </button>
        <button :class="{ active: tab === 'jobs' }" @click="tab = 'jobs'">
          <Icon name="clock" /> {{ t('Saved backups') }} <span v-if="overview.jobs.length" class="count">{{ overview.jobs.length }}</span>
        </button>
        <button :class="{ active: tab === 'runs' }" @click="tab = 'runs'">
          <Icon name="all" /> {{ t('Runs') }} <span v-if="activeRuns.length" class="count live">{{ activeRuns.length }}</span>
        </button>
      </nav>

      <div class="content">
        <p v-if="loadError" class="error-text">{{ loadError }}</p>

        <!-- Ny eller redigerad backup -->
        <template v-if="tab === 'new'">
          <p v-if="!store.accounts.length" class="muted">{{ t('Add an email account first.') }}</p>
          <template v-else>
            <label class="field">
              <span>{{ t('Account to back up') }}</span>
              <select v-model="form.sourceAccountId" class="input">
                <option v-for="a in store.accounts" :key="a.id" :value="a.id">{{ a.name }} &lt;{{ a.email }}&gt;</option>
              </select>
            </label>

            <div class="field">
              <span>{{ t('Save to') }}</span>
              <div class="targets">
                <label class="target" :class="{ active: form.type === 'zip' }">
                  <input v-model="form.type" type="radio" value="zip" />
                  <Icon name="zip" :size="20" />
                  <strong>{{ t('Zip file') }}</strong>
                  <small class="muted">{{ t('Download all messages as .eml files, folder by folder.') }}</small>
                </label>
                <label class="target" :class="{ active: form.type === 'imap', disabled: !targetAccounts.length }">
                  <input v-model="form.type" type="radio" value="imap" :disabled="!targetAccounts.length" />
                  <Icon name="mail" :size="20" />
                  <strong>{{ t('Another IMAP account') }}</strong>
                  <small class="muted">
                    {{ targetAccounts.length ? t('Copy to one of your other accounts. Messages already copied are skipped.') : t('Add another account to copy to.') }}
                  </small>
                </label>
                <label class="target" :class="{ active: form.type === 'gdrive' }">
                  <input v-model="form.type" type="radio" value="gdrive" />
                  <Icon name="cloud" :size="20" />
                  <strong>Google Drive</strong>
                  <small class="muted">{{ t('Upload a zip file to Google Drive, for example every night.') }}</small>
                </label>
              </div>
            </div>

            <div v-if="form.type === 'imap'" class="sub">
              <label class="field">
                <span>{{ t('Target account') }}</span>
                <select v-model="form.targetAccountId" class="input">
                  <option v-for="a in targetAccounts" :key="a.id" :value="a.id">{{ a.name }} &lt;{{ a.email }}&gt;</option>
                </select>
              </label>
              <label class="field">
                <span>{{ t('Folder in the target account') }}</span>
                <input v-model="form.targetFolder" class="input" :placeholder="t('Empty = same folders as the original')" />
                <small class="muted">
                  {{ t("Subfolders are separated with /. With an empty folder, the inbox goes to the target account's inbox, and Sent, Drafts and so on to the corresponding folders.") }}
                </small>
              </label>
            </div>

            <div v-if="form.type === 'gdrive'" class="sub">
              <p v-if="!overview.driveConfigured" class="error-text">
                {{ t('Google sign-in is not configured on the server. See the section on Google sign-in in the README.') }}
              </p>
              <template v-else>
                <div v-if="connections.length" class="field">
                  <span>{{ t('Google Drive account') }}</span>
                  <div v-for="c in connections" :key="c.id" class="connection">
                    <label class="radio">
                      <input v-model="form.connectionId" type="radio" :value="c.id" />
                      {{ c.email }}
                    </label>
                    <button class="btn ghost small" :disabled="busy" :title="t('Disconnect')" @click="removeConnection(c)">
                      {{ t('Disconnect') }}
                    </button>
                  </div>
                </div>
                <GoogleSignIn
                  :start="api.driveOauthStart"
                  :login-hint="driveLoginHint"
                  :label="connections.length ? t('Connect another Google Drive') : t('Connect Google Drive')"
                  :disabled="busy"
                  @signed-in="onDriveSignedIn"
                />
                <small v-if="driveLoginHint" class="muted block">
                  {{ t("Google's window suggests {email}, the same account as the email. You can choose another Google account there if the backup should go to a different Drive.", { email: driveLoginHint }) }}
                </small>
                <small class="muted block">
                  {{ t('PigeonPost only gets access to files the app creates itself. The backups go to the folder') }}
                  <strong>{{ overview.driveFolderName }}</strong>.
                </small>
                <label v-if="scheduled" class="field inline">
                  <span>{{ t('Keep the latest') }}</span>
                  <input v-model.number="form.keep" class="input narrow" type="number" min="1" max="365" />
                  <span>{{ t('backups in Drive') }}</span>
                </label>
              </template>
            </div>

            <div class="field">
              <span>{{ t('Folders') }}</span>
              <div class="folders-summary">
                <span>{{ t('{included} of {total} folders included', { included: includedCount, total: sourceFolders.length }) }}</span>
                <button class="btn ghost small" @click="showFolders = !showFolders">
                  {{ showFolders ? t('Hide') : t('Choose folders') }}
                </button>
              </div>
              <div v-if="showFolders" class="folder-list">
                <div class="folder-actions">
                  <button class="btn ghost small" @click="setAllFolders(true)">{{ t('Select all') }}</button>
                  <button class="btn ghost small" @click="setAllFolders(false)">{{ t('Deselect all') }}</button>
                </div>
                <label v-for="f in sourceFolders" :key="f.path" class="radio">
                  <input type="checkbox" :checked="!form.excludeFolders.includes(f.path)" @change="toggleFolder(f.path)" />
                  {{ f.path }} <span class="muted">({{ f.total }})</span>
                </label>
                <p v-if="!sourceFolders.length" class="muted">{{ t('The folders could not be fetched.') }}</p>
              </div>
              <small v-if="defaultExcluded(form.sourceAccountId).length" class="muted">
                {{ t("Gmail's All Mail and Starred are deselected from the start, since they only contain copies of messages in other folders. Folders created later are included automatically.") }}
              </small>
            </div>

            <div class="field">
              <span>{{ t('Schedule') }}</span>
              <div class="schedule">
                <select v-model="form.frequency" class="input auto">
                  <option value="none">{{ t('None, I start it myself') }}</option>
                  <option value="daily" :disabled="form.type === 'zip'">{{ t('Every day') }}</option>
                  <option value="weekly" :disabled="form.type === 'zip'">{{ t('Every week') }}</option>
                  <option value="monthly" :disabled="form.type === 'zip'">{{ t('Every month') }}</option>
                </select>
                <select v-if="form.frequency === 'weekly'" v-model="form.weekday" class="input auto">
                  <option v-for="(d, i) in weekdays" :key="d" :value="i + 1">{{ t('on {weekday}s', { weekday: t(d) }) }}</option>
                </select>
                <template v-if="form.frequency === 'monthly'">
                  <span>{{ t('on the') }}</span>
                  <input v-model.number="form.monthday" class="input narrow" type="number" min="1" max="28" />
                </template>
                <template v-if="scheduled">
                  <span>{{ t('at') }}</span>
                  <input v-model="form.time" class="input auto" type="time" required />
                </template>
              </div>
              <small v-if="form.type === 'zip'" class="muted">
                {{ t('A zip file for download cannot be scheduled. Choose Google Drive or an IMAP account for regular backups.') }}
              </small>
              <small v-else-if="scheduled" class="muted">
                {{ t('The time is in the {timezone} time zone. The server needs to be running.', { timezone }) }}
              </small>
            </div>

            <label v-if="scheduled || editingJobId" class="field">
              <span>{{ t('Name') }}</span>
              <input v-model="form.name" class="input" :placeholder="defaultName" maxlength="200" />
            </label>
            <label v-if="editingJobId && scheduled" class="radio">
              <input v-model="form.enabled" type="checkbox" /> {{ t('Schedule is active') }}
            </label>

            <p v-if="formError" class="error-text">{{ formError }}</p>
          </template>
        </template>

        <!-- Sparade backuper -->
        <template v-else-if="tab === 'jobs'">
          <p v-if="loading" class="muted">{{ t('Loading…') }}</p>
          <div v-else-if="!overview.jobs.length" class="empty">
            <p class="muted">{{ t('No saved backups. Choose a schedule under New backup to run a backup regularly.') }}</p>
            <button class="btn primary" @click="tab = 'new'">{{ t('New backup') }}</button>
          </div>
          <div v-for="job in overview.jobs" :key="job.id" class="card" :class="{ paused: !job.enabled }">
            <div class="card-head">
              <Icon :name="destinationIcon(job.config)" :size="18" />
              <div class="card-title">
                <strong>{{ job.name }}</strong>
                <span class="muted">
                  {{ accountEmail(job.config.sourceAccountId, job.config.sourceEmail) }} → {{ destinationText(job.config) }}
                </span>
              </div>
              <span v-if="!job.enabled" class="badge">{{ t('Paused') }}</span>
            </div>
            <div class="card-meta muted">
              <span><Icon name="clock" :size="12" /> {{ scheduleText(job.schedule) }}</span>
              <span v-if="job.enabled && job.nextRunAt">{{ t('Next:') }} {{ formatFullDate(job.nextRunAt) }}</span>
              <span v-if="job.lastRun">
                {{ t('Last:') }} <span :class="`status-${job.lastRun.status}`">{{ t(statusLabels[job.lastRun.status]) }}</span>
                {{ formatFullDate(job.lastRun.finishedAt || job.lastRun.createdAt) }}
              </span>
            </div>
            <p v-if="job.lastRun?.status === 'failed'" class="error-text small">{{ job.lastRun.message }}</p>
            <div class="card-actions">
              <button class="btn small" :disabled="busy || ['queued', 'running'].includes(job.lastRun?.status)" @click="runJob(job)">
                <Icon name="play" :size="12" /> {{ t('Run now') }}
              </button>
              <button class="btn small" :disabled="busy" @click="editJob(job)"><Icon name="settings" :size="12" /> {{ t('Edit') }}</button>
              <button v-if="job.schedule.frequency !== 'none'" class="btn small" :disabled="busy" @click="toggleJob(job)">
                <Icon :name="job.enabled ? 'pause' : 'play'" :size="12" /> {{ job.enabled ? t('Pause') : t('Enable') }}
              </button>
              <button class="btn small danger" :disabled="busy" @click="deleteJob(job)"><Icon name="trash" :size="12" /> {{ t('Delete') }}</button>
            </div>
          </div>
        </template>

        <!-- Körningar -->
        <template v-else>
          <p v-if="loading" class="muted">{{ t('Loading…') }}</p>
          <p v-else-if="!overview.runs.length" class="muted">{{ t('No backups have run yet.') }}</p>
          <div v-for="r in overview.runs" :key="r.id" class="card" :class="`run-${r.status}`">
            <div class="card-head">
              <Icon :name="destinationIcon(r.config)" :size="18" />
              <div class="card-title">
                <strong>{{ jobName(r) }}</strong>
                <span class="muted">
                  {{ accountEmail(r.config.sourceAccountId, r.config.sourceEmail) }} → {{ destinationText(r.config) }}
                </span>
              </div>
              <span class="badge" :class="`status-${r.status}`">{{ statusLabels[r.status] ? t(statusLabels[r.status]) : r.status }}</span>
            </div>

            <template v-if="r.status === 'queued' || r.status === 'running'">
              <div class="bar"><div class="bar-fill" :style="{ width: progressPercent(r) + '%' }" /></div>
              <div class="muted small">{{ progressText(r) }}</div>
            </template>
            <p v-else-if="r.message" class="small" :class="{ 'error-text': r.status === 'failed' }">{{ r.message }}</p>

            <div class="card-meta muted">
              <span>{{ formatFullDate(r.startedAt || r.createdAt) }}</span>
              <span v-if="r.fileSize">{{ formatSize(r.fileSize) }}</span>
              <span v-if="r.hasFile && r.expiresAt">{{ t('Will be deleted {date}', { date: formatFullDate(r.expiresAt) }) }}</span>
              <button v-if="r.progress?.warningCount" class="link" @click="toggleRunDetails(r.id)">
                {{ r.progress.warningCount === 1 ? t('1 warning') : t('{n} warnings', { n: r.progress.warningCount }) }}
              </button>
            </div>
            <ul v-if="expandedRuns.has(r.id)" class="warnings">
              <li v-for="(w, i) in r.progress.warnings" :key="i">{{ w }}</li>
              <li v-if="r.progress.warningCount > r.progress.warnings.length" class="muted">
                {{ t('… and {n} more (see the server log)', { n: r.progress.warningCount - r.progress.warnings.length }) }}
              </li>
            </ul>

            <div class="card-actions">
              <a v-if="r.hasFile" class="btn small primary" :href="backupDownloadUrl(r.id)" download>
                <Icon name="download" :size="12" /> {{ t('Download') }}
              </a>
              <button v-if="r.status === 'queued' || r.status === 'running'" class="btn small" :disabled="busy" @click="cancelRun(r)">
                <Icon name="stop" :size="12" /> {{ t('Cancel') }}
              </button>
              <button v-else class="btn small ghost" :disabled="busy" @click="deleteRun(r)">
                <Icon name="trash" :size="12" /> {{ t('Delete') }}
              </button>
            </div>
          </div>
        </template>
      </div>

      <div v-if="tab === 'new' && store.accounts.length" class="modal-footer">
        <button v-if="editingJobId" class="btn" :disabled="busy" @click="resetForm">{{ t('Cancel editing') }}</button>
        <div class="spacer" />
        <template v-if="scheduled || editingJobId">
          <button class="btn" :disabled="busy" @click="saveJob({ runAfterSave: true })">{{ t('Save and run now') }}</button>
          <button class="btn primary" :disabled="busy" @click="saveJob()">
            {{ editingJobId ? t('Save changes') : t('Save schedule') }}
          </button>
        </template>
        <button v-else class="btn primary" :disabled="busy" @click="startNow">
          <Icon name="play" :size="14" /> {{ t('Start backup') }}
        </button>
      </div>
    </div>
  </div>
</template>

<style scoped>
.backup {
  max-width: 760px;
  height: min(820px, calc(100vh - 32px));
}

.tabs {
  display: flex;
  gap: 2px;
  padding: 6px 12px 0;
  border-bottom: 1px solid var(--border);
  overflow-x: auto;
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
  white-space: nowrap;
  color: var(--text-muted);
}

.tabs button.active {
  color: var(--text);
  border-bottom-color: var(--accent);
}

.count {
  font-size: 11px;
  background: var(--pane-alt);
  border-radius: 9px;
  padding: 0 6px;
}

.count.live {
  background: var(--accent);
  color: var(--accent-text);
}

.content {
  flex: 1;
  overflow-y: auto;
  padding: 16px;
  display: flex;
  flex-direction: column;
  gap: 14px;
}

.field {
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.field > span:first-child {
  font-size: 12px;
  font-weight: 600;
  color: var(--text-muted);
}

.field.inline {
  flex-direction: row;
  align-items: center;
  gap: 8px;
  flex-wrap: wrap;
}

.field.inline > span:first-child {
  font-weight: normal;
  font-size: 13px;
  color: var(--text);
}

.targets {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(190px, 1fr));
  gap: 8px;
}

.target {
  display: grid;
  grid-template-columns: auto 1fr;
  grid-template-rows: auto auto;
  column-gap: 8px;
  row-gap: 2px;
  align-items: center;
  padding: 10px;
  border: 1px solid var(--border);
  border-radius: var(--radius);
  cursor: pointer;
}

.target input {
  position: absolute;
  opacity: 0;
  pointer-events: none;
}

.target small {
  grid-column: 1 / -1;
}

.target.active {
  border-color: var(--accent);
  outline: 1px solid var(--accent);
}

.target.disabled {
  opacity: 0.6;
  cursor: default;
}

.sub {
  display: flex;
  flex-direction: column;
  gap: 10px;
  padding: 12px;
  background: var(--pane-alt);
  border-radius: var(--radius);
}

.connection {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 8px;
}

.radio {
  display: flex;
  align-items: center;
  gap: 6px;
}

.block {
  display: block;
}

.small {
  font-size: 12px;
}

.btn.small {
  padding: 3px 8px;
  font-size: 12px;
}

.input.narrow {
  width: 72px;
}

.input.auto {
  width: auto;
}

.folders-summary {
  display: flex;
  align-items: center;
  gap: 8px;
}

.folder-list {
  max-height: 220px;
  overflow-y: auto;
  border: 1px solid var(--border);
  border-radius: var(--radius);
  padding: 6px 10px;
  display: flex;
  flex-direction: column;
  gap: 2px;
}

.folder-actions {
  display: flex;
  gap: 4px;
  margin-bottom: 4px;
}

.schedule {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-wrap: wrap;
}

.card {
  border: 1px solid var(--border);
  border-radius: var(--radius);
  padding: 10px 12px;
  display: flex;
  flex-direction: column;
  gap: 6px;
}

.card.paused {
  opacity: 0.75;
}

.card.run-failed {
  border-color: var(--danger);
}

.card-head {
  display: flex;
  align-items: center;
  gap: 10px;
}

.card-title {
  flex: 1;
  min-width: 0;
  display: flex;
  flex-direction: column;
  overflow-wrap: anywhere;
}

.card-title .muted {
  font-size: 12px;
}

.card-meta {
  display: flex;
  flex-wrap: wrap;
  gap: 4px 14px;
  font-size: 12px;
  align-items: center;
}

.card-meta span {
  display: inline-flex;
  align-items: center;
  gap: 4px;
}

.card-actions {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
}

.card p {
  margin: 0;
}

a.btn {
  text-decoration: none;
}

.badge {
  font-size: 11px;
  padding: 1px 8px;
  border-radius: 9px;
  border: 1px solid var(--border);
  background: var(--pane-alt);
  white-space: nowrap;
}

.status-done {
  color: var(--success);
  border-color: var(--success);
}

.status-failed {
  color: var(--danger);
  border-color: var(--danger);
}

.status-running,
.status-queued {
  color: var(--accent);
  border-color: var(--accent);
}

.bar {
  height: 6px;
  border-radius: 3px;
  background: var(--pane-alt);
  overflow: hidden;
}

.bar-fill {
  height: 100%;
  background: var(--accent);
  transition: width 0.4s;
}

.link {
  border: 0;
  background: none;
  padding: 0;
  color: #9a6700;
  cursor: pointer;
  text-decoration: underline;
  font-size: 12px;
}

.warnings {
  margin: 0;
  padding-left: 18px;
  font-size: 12px;
  max-height: 160px;
  overflow-y: auto;
}

.empty {
  display: flex;
  flex-direction: column;
  align-items: flex-start;
  gap: 8px;
}

.empty p {
  margin: 0;
}

.spacer {
  flex: 1;
}
</style>
