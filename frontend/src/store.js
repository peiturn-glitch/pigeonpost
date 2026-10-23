import { defineStore } from 'pinia'
import { computed, reactive, ref, watch } from 'vue'
import { api, attachmentUrl } from './api'
import {
  DEFAULT_SORT,
  isInsideFolder,
  isSortKey,
  formatAddress,
  formatAddressList,
  formatFullDate,
  displayName,
  messageComparator,
  messagePlainText,
  prefixSubject,
  quoteText,
} from './utils'
import { QUOTE_STYLE, escapeHtml, messageHtml, sanitizeHtml, signatureHtml, textToHtml } from './richtext'

const PAGE_SIZE = 100

function loadExpanded() {
  try {
    return new Set(JSON.parse(localStorage.getItem('brevduva.expanded') || '[]'))
  } catch {
    return new Set()
  }
}

// Sorteringen sparas i webbläsaren, en per slags lista: mappar, vyn över olästa och sökningen i alla mappar.
const SORT_KEY = 'brevduva.sort'
const SORT_VIEWS = ['folder', 'unread', 'search']

function loadSort() {
  const sort = Object.fromEntries(SORT_VIEWS.map((view) => [view, { ...DEFAULT_SORT }]))
  try {
    const saved = JSON.parse(localStorage.getItem(SORT_KEY) || 'null')
    for (const view of SORT_VIEWS) {
      const s = saved?.[view]
      if (s && isSortKey(s.key)) Object.assign(sort[view], { key: s.key, desc: s.desc !== false })
    }
  } catch {
    // Ogiltigt eller otillgängligt: standardsorteringen gäller.
  }
  return sort
}

const LAYOUT_KEY = 'brevduva.layout'
const FOLDER_POSITIONS = ['left', 'right', 'hidden']
const PREVIEW_POSITIONS = ['bottom', 'top', 'right', 'left', 'hidden']
export const DEFAULT_LAYOUT = { folders: 'left', preview: 'bottom', folderWidth: 240, listHeight: 320, listWidth: 460 }

// Panelernas placering och storlek sparas i webbläsaren. Äldre versioner sparade bara storlekarna.
function loadLayout() {
  const layout = { ...DEFAULT_LAYOUT }
  try {
    const saved = JSON.parse(localStorage.getItem(LAYOUT_KEY) || 'null')
    const legacyWidth = Number(localStorage.getItem('brevduva.folderWidth'))
    const legacyHeight = Number(localStorage.getItem('brevduva.listHeight'))
    if (legacyWidth > 0) layout.folderWidth = legacyWidth
    if (legacyHeight > 0) layout.listHeight = legacyHeight
    if (saved && typeof saved === 'object') {
      if (FOLDER_POSITIONS.includes(saved.folders)) layout.folders = saved.folders
      if (PREVIEW_POSITIONS.includes(saved.preview)) layout.preview = saved.preview
      for (const key of ['folderWidth', 'listHeight', 'listWidth']) {
        if (Number(saved[key]) > 0) layout[key] = Number(saved[key])
      }
    }
  } catch {
    // Lagring otillgänglig; standardlayouten används.
  }
  return layout
}

export const useMailStore = defineStore('mail', () => {
  const user = ref(null) // { id, username, isAdmin }
  const oauthProviders = ref({ google: { configured: false, redirectUri: '' } })
  const accounts = ref([])
  const folders = reactive({}) // accountId -> platt mapplista
  const folderErrors = reactive({})
  const cacheStatus = reactive({}) // accountId -> synkens läge för mejlcachen, se loadCacheStatus
  const expanded = ref(loadExpanded())

  // view är satt i stället för konto och mapp när listan visar en samlad vy: 'unread' = olästa i alla konton och mappar.
  const current = reactive({ accountId: null, folder: null, view: null })
  const messages = ref([])
  const total = ref(0)
  const page = ref(0)
  const loadingMessages = ref(false)
  const listError = ref('')
  const filter = reactive({ q: '', unread: false, flagged: false })
  const sort = reactive(loadSort())
  watch(
    sort,
    (value) => {
      try {
        localStorage.setItem(SORT_KEY, JSON.stringify(value))
      } catch {
        // Ignoreras; sorteringen gäller bara denna flik.
      }
    },
    { deep: true },
  )
  // Sorteringen för listan som visas just nu.
  const currentSort = computed(() => sort[current.view || 'folder'])
  const sortParams = (s) => ({ sort: s.key, order: s.desc ? 'desc' : 'asc' })

  const selection = ref(new Set())
  const anchorUid = ref(null)
  const openMessage = ref(null)
  const loadingMessage = ref(false)
  const messageError = ref('')

  const compose = ref(null)
  const accountDialog = ref(null)
  const identityDialog = ref(null) // { account, identity } när en avsändare redigeras
  const usersDialog = ref(false)
  const passwordDialog = ref(false)
  const thunderbirdDialog = ref(false)
  const layoutDialog = ref(false)
  const backupDialog = ref(false) // true eller { accountId } för att förvälja ett konto
  const draggedFolder = ref(null) // { accountId, folder } medan en mapp dras i mapplistan
  const folderMoveDialog = ref(null) // { accountId, folder } när en mapp ska flyttas
  const aiDialog = ref(false) // true eller { accountId, folder, uids } för att förvälja vad som ska sorteras
  const remoteSenders = ref(new Set()) // Avsändare vars fjärrinnehåll alltid visas
  const searchDialog = ref(false) // Sökning i alla mappar
  // Sökningen ligger här och inte i dialogen, så att träffarna finns kvar när man har öppnat en av dem och kommer tillbaka.
  const emptySearchForm = () => ({
    q: '', from: '', to: '', after: '', before: '', accountId: null, folder: '',
    unread: false, flagged: false, attachments: false, includeTrash: false,
  })
  const search = reactive({ question: '', explanation: '', form: emptySearchForm(), results: [], total: 0, page: 0, searched: false })
  const readerOpen = ref(false) // Meddelandet i ett eget fönster när läsrutan är dold
  const contextMenu = ref(null)

  const layout = reactive(loadLayout())
  watch(
    layout,
    (value) => {
      try {
        localStorage.setItem(LAYOUT_KEY, JSON.stringify(value))
      } catch {
        // Ignoreras; layouten gäller bara denna flik.
      }
    },
    { deep: true },
  )
  const toasts = ref([])

  let listRequest = 0
  let messageRequest = 0

  // Nollställer allt användarspecifikt, t.ex. vid utloggning eller när en annan användare loggar in.
  function reset() {
    listRequest++
    messageRequest++
    user.value = null
    accounts.value = []
    for (const key of Object.keys(folders)) delete folders[key]
    for (const key of Object.keys(folderErrors)) delete folderErrors[key]
    for (const key of Object.keys(cacheStatus)) delete cacheStatus[key]
    current.accountId = null
    current.folder = null
    messages.value = []
    total.value = 0
    page.value = 0
    loadingMessages.value = false
    listError.value = ''
    Object.assign(filter, { q: '', unread: false, flagged: false })
    selection.value = new Set()
    anchorUid.value = null
    openMessage.value = null
    loadingMessage.value = false
    messageError.value = ''
    compose.value = null
    accountDialog.value = null
    identityDialog.value = null
    usersDialog.value = false
    passwordDialog.value = false
    thunderbirdDialog.value = false
    layoutDialog.value = false
    backupDialog.value = false
    aiDialog.value = false
    searchDialog.value = false
    remoteSenders.value = new Set()
    Object.assign(search, { question: '', explanation: '', form: emptySearchForm(), results: [], total: 0, page: 0, searched: false })
    folderMoveDialog.value = null
    readerOpen.value = false
    contextMenu.value = null
  }

  // --- Hjälpfunktioner --------------------------------------------------

  function notify(text, type = 'info') {
    const id = Date.now() + Math.random()
    toasts.value.push({ id, text, type })
    setTimeout(() => {
      toasts.value = toasts.value.filter((t) => t.id !== id)
    }, type === 'error' ? 8000 : 4000)
  }

  function account(id) {
    return accounts.value.find((a) => a.id === id)
  }

  function folderInfo(accountId, path) {
    return (folders[accountId] || []).find((f) => f.path === path)
  }

  function specialFolder(accountId, use) {
    return (folders[accountId] || []).find((f) => f.specialUse === use)
  }

  const currentFolderInfo = computed(() =>
    current.accountId ? folderInfo(current.accountId, current.folder) : null,
  )

  const selectedMessages = computed(() => messages.value.filter((m) => selection.value.has(m.key)))

  function toggleExpanded(key) {
    const next = new Set(expanded.value)
    if (next.has(key)) next.delete(key)
    else next.add(key)
    expanded.value = next
    try {
      localStorage.setItem('brevduva.expanded', JSON.stringify([...next]))
    } catch {
      // Lagring otillgänglig; expanderingen gäller bara denna flik.
    }
  }

  function adjustUnread(accountId, path, delta) {
    const f = folderInfo(accountId, path)
    if (f) f.unread = Math.max(0, f.unread + delta)
  }

  // --- Konton och mappar ------------------------------------------------

  async function init() {
    api.oauthProviders().then((p) => (oauthProviders.value = p)).catch(() => {})
    api.remoteSenders().then((r) => (remoteSenders.value = new Set(r.senders))).catch(() => {})
    loadCacheStatus()
    await loadAccounts()
    await Promise.all(accounts.value.map((a) => loadFolders(a.id)))
    const first = accounts.value[0]
    if (first) {
      expanded.value.add(`acc:${first.id}`)
      await selectFolder(first.id, 'INBOX')
    }
  }

  async function loadAccounts() {
    accounts.value = await api.accounts()
  }

  async function loadFolders(accountId) {
    try {
      folders[accountId] = await api.folders(accountId)
      delete folderErrors[accountId]
    } catch (e) {
      folderErrors[accountId] = e.message
    }
  }

  // Mejlcachens läge per konto: om mejlservern går att nå och hur långt den första synken har kommit.
  async function loadCacheStatus() {
    try {
      const result = await api.cacheStatus()
      for (const key of Object.keys(cacheStatus)) delete cacheStatus[key]
      for (const s of result.accounts) cacheStatus[s.accountId] = s
    } catch {
      // Statusen är bara upplysning; listor och läsning fungerar utan den.
    }
  }

  async function refreshAll() {
    // Hämta = be servern synka kontona mot mejlservern nu, i stället för att vänta på nästa kontroll.
    await Promise.all(accounts.value.map((a) => api.syncAccount(a.id).catch(() => {})))
    loadCacheStatus()
    await Promise.all(accounts.value.map((a) => loadFolders(a.id)))
    if (current.accountId || current.view) await loadMessages({ keepSelection: true })
  }

  async function createFolder(accountId, parentPath, delimiter) {
    const name = window.prompt('Namn på den nya mappen:')
    if (!name) return
    const path = parentPath ? `${parentPath}${delimiter || '/'}${name}` : name
    try {
      await api.createFolder(accountId, path)
      if (parentPath) expanded.value.add(`${accountId}:${parentPath}`)
      await loadFolders(accountId)
    } catch (e) {
      notify(e.message, 'error')
    }
  }

  async function renameFolder(accountId, folder) {
    const delimiter = folder.delimiter
    const parts = delimiter ? folder.path.split(delimiter) : [folder.path]
    const name = window.prompt('Nytt namn på mappen:', parts[parts.length - 1])
    if (!name || name === parts[parts.length - 1]) return
    parts[parts.length - 1] = name
    const to = parts.join(delimiter)
    try {
      await api.renameFolder(accountId, folder.path, to)
      await loadFolders(accountId)
      if (current.accountId === accountId && current.folder === folder.path) await selectFolder(accountId, to)
    } catch (e) {
      notify(e.message, 'error')
    }
  }

  function parentPath(folder) {
    const index = folder.delimiter ? folder.path.lastIndexOf(folder.delimiter) : -1
    return index === -1 ? '' : folder.path.slice(0, index)
  }

  // Inkorgen och specialmappar (Skickat, Papperskorg …) ligger där servern vill ha dem.
  function canMoveFolder(folder) {
    return !!folder && !folder.specialUse && folder.path !== 'INBOX'
  }

  // Flyttar en mapp med undermappar till en annan mapp; tom parent = översta nivån. I IMAP är det ett namnbyte.
  async function moveFolder(accountId, folder, parent, { confirm = true } = {}) {
    if (!canMoveFolder(folder) || parent === parentPath(folder)) return false
    if (parent && isInsideFolder(parent, folder)) {
      notify('En mapp kan inte flyttas in i sig själv', 'error')
      return false
    }
    const name = folder.delimiter ? folder.path.slice(folder.path.lastIndexOf(folder.delimiter) + 1) : folder.path
    const delimiter = folder.delimiter || folderInfo(accountId, parent)?.delimiter || '/'
    const to = parent ? `${parent}${delimiter}${name}` : name
    if (folderInfo(accountId, to)) {
      notify(`Det finns redan en mapp som heter "${name}" där`, 'error')
      return false
    }
    const target = parent ? folderInfo(accountId, parent) : null
    const targetLabel = parent ? `"${target ? target.path : parent}"` : 'översta nivån'
    if (confirm && !window.confirm(`Flytta mappen "${name}" till ${targetLabel}?`)) return false
    try {
      await api.renameFolder(accountId, folder.path, to)
      // Behåll utfällda undermappar och den öppna mappen under den nya sökvägen.
      const next = new Set()
      for (const key of expanded.value) {
        const prefix = `${accountId}:`
        const path = key.startsWith(prefix) ? key.slice(prefix.length) : null
        next.add(path !== null && isInsideFolder(path, folder) ? prefix + to + path.slice(folder.path.length) : key)
      }
      if (parent) next.add(`${accountId}:${parent}`)
      expanded.value = next
      try {
        localStorage.setItem('brevduva.expanded', JSON.stringify([...next]))
      } catch {
        // Lagring otillgänglig; expanderingen gäller bara denna flik.
      }
      await loadFolders(accountId)
      // Vissa servrar svarar OK utan att flytta mappen.
      if (folderInfo(accountId, folder.path) && !folderInfo(accountId, to)) {
        notify('E-postservern flyttade inte mappen', 'error')
        return false
      }
      if (current.accountId === accountId && current.folder && isInsideFolder(current.folder, folder)) {
        await selectFolder(accountId, to + current.folder.slice(folder.path.length))
      }
      notify(`Mappen "${name}" flyttades`)
      return true
    } catch (e) {
      notify(e.message, 'error')
      return false
    }
  }

  async function deleteFolder(accountId, folder) {
    if (!window.confirm(`Ta bort mappen "${folder.name}" och allt innehåll? Det går inte att ångra.`)) return
    try {
      await api.deleteFolder(accountId, folder.path)
      if (current.accountId === accountId && current.folder === folder.path) await selectFolder(accountId, 'INBOX')
      await loadFolders(accountId)
    } catch (e) {
      notify(e.message, 'error')
    }
  }

  async function saveAccount(data) {
    const saved = data.id ? await api.updateAccount(data.id, data) : await api.createAccount(data)
    await loadAccounts()
    await loadFolders(saved.id)
    if (!data.id) {
      expanded.value.add(`acc:${saved.id}`)
      await selectFolder(saved.id, 'INBOX')
    }
    return saved
  }

  async function removeAccount(id) {
    await api.deleteAccount(id)
    delete folders[id]
    await loadAccounts()
    if (current.accountId === id) {
      current.accountId = null
      current.folder = null
      messages.value = []
      openMessage.value = null
      const first = accounts.value[0]
      if (first) await selectFolder(first.id, 'INBOX')
    }
  }

  // Litar på avsändaren (trusted = true) eller slutar lita på den. Fjärrinnehåll i avsändarens mejl visas då direkt.
  async function setRemoteSenderTrusted(email, trusted) {
    try {
      const result = await (trusted ? api.trustRemoteSender(email) : api.untrustRemoteSender(email))
      remoteSenders.value = new Set(result.senders)
    } catch (e) {
      notify(e.message, 'error')
    }
  }

  // --- Meddelandelista --------------------------------------------------

  async function selectFolder(accountId, path) {
    current.accountId = accountId
    current.folder = path
    current.view = null
    filter.q = ''
    filter.unread = false
    filter.flagged = false
    openMessage.value = null
    selection.value = new Set()
    messages.value = []
    await loadMessages()
  }

  // En samlad vy över alla konton och mappar, i stället för en mapp. Den läses ur mejlcachen.
  async function selectView(view) {
    current.accountId = null
    current.folder = null
    current.view = view
    filter.q = ''
    filter.unread = false
    filter.flagged = false
    openMessage.value = null
    selection.value = new Set()
    messages.value = []
    await loadMessages()
  }

  // Varje rad i listan har en nyckel som är unik i listan, och vet vilket konto och vilken mapp den ligger i. I en
  // mapp räcker UID som nyckel; i en samlad vy kan samma UID finnas i flera mappar.
  function withKeys(list) {
    return list.map((m) =>
      current.view
        ? { ...m, key: `${m.accountId}:${m.folder}:${m.uid}` }
        : { ...m, key: m.uid, accountId: current.accountId, folder: current.folder },
    )
  }

  async function loadMessages({ append = false, keepSelection = false } = {}) {
    if (!current.accountId && !current.view) return
    const token = ++listRequest
    const nextPage = append ? page.value + 1 : 0
    loadingMessages.value = true
    listError.value = ''
    try {
      const result = current.view
        ? await api.search({
            unread: true,
            skipSent: true,
            flagged: filter.flagged,
            q: filter.q,
            page: nextPage,
            pageSize: PAGE_SIZE,
            ...sortParams(currentSort.value),
          })
        : await api.messages(current.accountId, {
            folder: current.folder,
            page: nextPage,
            pageSize: PAGE_SIZE,
            q: filter.q,
            unread: filter.unread,
            flagged: filter.flagged,
            ...sortParams(currentSort.value),
          })
      if (token !== listRequest) return
      total.value = result.total
      page.value = nextPage
      let loaded = withKeys(result.messages)
      if (current.view && keepSelection && !append) {
        // Vid en uppdatering av vyn försvinner det som har lästs, utom mejlet som är öppet just nu: den raden ska inte
        // försvinna medan man läser. Har fler sidor lästs in behålls de, och nya olästa läggs till överst.
        const fresh = new Set(loaded.map((m) => m.key))
        const openKey = openMessage.value?.key
        const kept = messages.value.filter((m) => !fresh.has(m.key) && (m.key === openKey || (!m.seen && messages.value.length > PAGE_SIZE)))
        loaded = [...loaded, ...kept].sort(messageComparator(currentSort.value))
      }
      if (append) {
        const present = new Set(messages.value.map((m) => m.key))
        loaded = loaded.filter((m) => !present.has(m.key))
      }
      messages.value = append ? [...messages.value, ...loaded] : loaded
      if (!keepSelection && !append) selection.value = new Set()
      else {
        const present = new Set(messages.value.map((m) => m.key))
        selection.value = new Set([...selection.value].filter((key) => present.has(key)))
      }
    } catch (e) {
      if (token === listRequest) listError.value = e.message
    } finally {
      if (token === listRequest) loadingMessages.value = false
    }
  }

  const hasMore = computed(() => messages.value.length < total.value)

  // Byter sortering för listan som visas och läser om den från början. Markeringen behålls.
  async function setSort(key, desc) {
    if (!isSortKey(key)) return
    Object.assign(currentSort.value, { key, desc })
    await loadMessages({ keepSelection: true })
  }

  // Öppnar en träff från sökningen i alla mappar: går till träffens mapp och visar meddelandet där.
  async function openSearchResult(hit) {
    searchDialog.value = false
    if (current.accountId !== hit.accountId || current.folder !== hit.folder) await selectFolder(hit.accountId, hit.folder)
    selection.value = new Set([hit.uid])
    anchorUid.value = hit.uid
    await showMessage(hit.uid)
    if (layout.preview === 'hidden' && openMessage.value) readerOpen.value = true
  }

  function selectMessage(key, { ctrl = false, shift = false } = {}) {
    if (shift && anchorUid.value !== null) {
      const keys = messages.value.map((m) => m.key)
      const a = keys.indexOf(anchorUid.value)
      const b = keys.indexOf(key)
      if (a !== -1 && b !== -1) {
        const [from, to] = a < b ? [a, b] : [b, a]
        selection.value = new Set(keys.slice(from, to + 1))
        return
      }
    }
    if (ctrl) {
      const next = new Set(selection.value)
      if (next.has(key)) next.delete(key)
      else next.add(key)
      selection.value = next
      anchorUid.value = key
      return
    }
    selection.value = new Set([key])
    anchorUid.value = key
    showMessage(key)
  }

  function selectAll() {
    selection.value = new Set(messages.value.map((m) => m.key))
  }

  function moveSelection(offset) {
    if (!messages.value.length) return
    const keys = messages.value.map((m) => m.key)
    const currentIndex = anchorUid.value !== null ? keys.indexOf(anchorUid.value) : -1
    const next = Math.min(keys.length - 1, Math.max(0, currentIndex + offset))
    selectMessage(keys[next])
  }

  // Var meddelandet med nyckeln ligger: raden i listan, det öppna meddelandet, eller (för ett meddelande som öppnats
  // från sökningen och inte finns i den inlästa delen av listan) den aktuella mappen med nyckeln som UID.
  function locate(key) {
    const item = messages.value.find((m) => m.key === key)
    if (item) return { key, accountId: item.accountId, folder: item.folder, uid: item.uid }
    const open = openMessage.value
    if (open && open.key === key) return { key, accountId: open.accountId, folder: open.folder, uid: open.uid }
    if (current.accountId && typeof key === 'number') return { key, accountId: current.accountId, folder: current.folder, uid: key }
    return null
  }

  // Nycklarna grupperade per konto och mapp, eftersom mejlservern tar en mapp i taget.
  function groupByFolder(keys) {
    const groups = new Map()
    for (const key of keys) {
      const at = locate(key)
      if (!at) continue
      const id = `${at.accountId}\n${at.folder}`
      if (!groups.has(id)) groups.set(id, { accountId: at.accountId, folder: at.folder, uids: [], keys: [] })
      groups.get(id).uids.push(at.uid)
      groups.get(id).keys.push(key)
    }
    return [...groups.values()]
  }

  async function showMessage(key) {
    const token = ++messageRequest
    const at = locate(key)
    if (!at) return
    if (current.view === 'unread') {
      // I vyn över olästa försvinner ett läst mejl när man går vidare till nästa. Markerade rader får stå kvar.
      const before = messages.value.length
      messages.value = messages.value.filter((m) => !m.seen || m.key === key || selection.value.has(m.key))
      total.value = Math.max(0, total.value - (before - messages.value.length))
    }
    const { accountId, folder, uid } = at
    loadingMessage.value = true
    messageError.value = ''
    try {
      const msg = await api.message(accountId, folder, uid)
      if (token !== messageRequest) return
      openMessage.value = { ...msg, accountId, key }
      loadInlineImages(openMessage.value, token)
      const item = messages.value.find((m) => m.key === key)
      if (item && !item.seen) {
        item.seen = true
        adjustUnread(accountId, folder, -1)
      }
    } catch (e) {
      if (token === messageRequest) {
        openMessage.value = null
        messageError.value = e.message
      }
    } finally {
      if (token === messageRequest) loadingMessage.value = false
    }
  }

  // Inbäddade bilder (cid:) skickas inte med i meddelandets HTML. De hämtas som bilagor när texten redan visas
  // och sätts in som data-URI:er, eftersom den sandlådade läsrutan inte får hämta något själv.
  async function loadInlineImages(msg, token) {
    const images = (msg.attachments || []).filter(
      (a) => a.inline && a.contentId && a.contentType.startsWith('image/') && a.size <= 5 * 1024 * 1024,
    )
    if (!images.length || !msg.html) return
    let html = msg.html
    for (const a of images) {
      try {
        const res = await fetch(attachmentUrl(msg.accountId, msg.folder, msg.uid, a.index, false), { credentials: 'same-origin' })
        if (!res.ok) continue
        const blob = await res.blob()
        const dataUrl = await new Promise((resolve, reject) => {
          const reader = new FileReader()
          reader.onload = () => resolve(reader.result)
          reader.onerror = () => reject(reader.error)
          reader.readAsDataURL(blob)
        })
        html = html.split(`cid:${a.contentId}`).join(dataUrl)
      } catch {
        // Bilden går inte att hämta, t.ex. när mejlservern inte går att nå. Texten visas ändå.
      }
      if (token !== messageRequest) return
    }
    if (token === messageRequest && openMessage.value?.key === msg.key && html !== msg.html) {
      openMessage.value = { ...openMessage.value, html }
    }
  }

  // --- Åtgärder på meddelanden --------------------------------------------

  function targetKeys(keys) {
    if (keys && keys.length) return keys
    return [...selection.value]
  }

  async function setFlag(flag, value, keys) {
    const list = targetKeys(keys)
    if (!list.length) return
    try {
      for (const group of groupByFolder(list)) {
        await api.setFlags(group.accountId, group.folder, group.uids, value ? [flag] : [], value ? [] : [flag])
        for (const m of messages.value) {
          if (!group.keys.includes(m.key) || m[flag] === value) continue
          if (flag === 'seen') adjustUnread(group.accountId, group.folder, value ? -1 : 1)
          m[flag] = value
        }
        if (openMessage.value && group.keys.includes(openMessage.value.key)) openMessage.value[flag] = value
      }
    } catch (e) {
      notify(e.message, 'error')
    }
  }

  function removeFromList(keys) {
    const removed = new Set(keys)
    const index = messages.value.findIndex((m) => removed.has(m.key))
    for (const m of messages.value) {
      if (removed.has(m.key) && !m.seen) adjustUnread(m.accountId, m.folder, -1)
    }
    messages.value = messages.value.filter((m) => !removed.has(m.key))
    total.value = Math.max(0, total.value - removed.size)

    const wasOpen = openMessage.value && removed.has(openMessage.value.key)
    selection.value = new Set()
    openMessage.value = null
    // Som i Thunderbird: markera nästa meddelande efter borttagning. Ett meddelande som öppnats från sökningen
    // behöver inte finnas i den inlästa delen av listan; då markeras inget.
    if (wasOpen && index !== -1 && messages.value.length) {
      const next = messages.value[Math.min(index, messages.value.length - 1)]
      selectMessage(next.key)
    }
  }

  async function deleteMessages(keys) {
    const list = targetKeys(keys)
    if (!list.length) return
    const groups = groupByFolder(list)
    const fromTrash = groups.filter((g) => folderInfo(g.accountId, g.folder)?.specialUse === 'trash')
    const permanent = fromTrash.reduce((n, g) => n + g.uids.length, 0)
    if (permanent && !window.confirm(`Radera ${permanent} meddelande(n) permanent?`)) return
    const done = []
    try {
      for (const group of groups) {
        await api.remove(group.accountId, group.folder, group.uids)
        done.push(...group.keys)
      }
    } catch (e) {
      notify(e.message, 'error')
    }
    if (done.length) removeFromList(done)
    for (const accountId of new Set(groups.map((g) => g.accountId))) loadFolders(accountId)
  }

  // Flyttar till en mapp i targetAccountId (det aktuella kontot om inget anges). I en samlad vy kan markeringen
  // innehålla mejl från flera konton; flytt mellan konton stöds inte, så de mejlen hoppas över.
  async function moveMessages(target, keys, targetAccount) {
    const list = targetKeys(keys)
    if (!list.length) return
    const groups = groupByFolder(list)
    const targetAccountId = targetAccount ?? current.accountId ?? groups[0]?.accountId
    const movable = groups.filter((g) => g.accountId === targetAccountId && g.folder !== target)
    const skipped = groups.filter((g) => g.accountId !== targetAccountId).reduce((n, g) => n + g.uids.length, 0)
    const done = []
    let moved = 0
    try {
      for (const group of movable) {
        await api.move(group.accountId, group.folder, group.uids, target)
        moved += group.uids.length
        done.push(...group.keys)
      }
    } catch (e) {
      notify(e.message, 'error')
    }
    if (done.length) removeFromList(done)
    if (moved) {
      loadFolders(targetAccountId)
      notify(`${moved} meddelande(n) flyttades`)
    }
    if (skipped) notify(`${skipped} meddelande(n) ligger i ett annat konto och flyttades inte. Flytt mellan konton stöds inte ännu.`, 'error')
  }

  // Skräppost och arkiv finns per konto, så markeringen delas upp per konto.
  async function moveToSpecial(use, missing, keys) {
    const list = targetKeys(keys)
    if (!list.length) return
    const byAccount = new Map()
    for (const group of groupByFolder(list)) {
      if (!byAccount.has(group.accountId)) byAccount.set(group.accountId, [])
      byAccount.get(group.accountId).push(...group.keys)
    }
    for (const [accountId, accountKeys] of byAccount) {
      const folder = specialFolder(accountId, use)
      if (!folder) {
        notify(missing, 'error')
        continue
      }
      await moveMessages(folder.path, accountKeys, accountId)
    }
  }

  const markJunk = (keys) => moveToSpecial('junk', 'Kontot har ingen skräppostmapp', keys)
  const archive = (keys) => moveToSpecial('archive', 'Kontot har ingen arkivmapp', keys)

  // --- Skriva meddelanden -----------------------------------------------

  // Alla adresser användaren kan skicka från: varje kontos egen adress plus dess extra avsändare.
  const senders = computed(() =>
    accounts.value.flatMap((a) => [
      { key: `a${a.id}`, accountId: a.id, identityId: null, name: a.name, email: a.email, signature: a.signature, signatureHtml: a.signatureHtml, via: '' },
      ...(a.identities || []).map((i) => ({
        key: `a${a.id}-i${i.id}`,
        accountId: a.id,
        identityId: i.id,
        name: i.name,
        email: i.email,
        signature: i.signature,
        signatureHtml: i.signatureHtml,
        via: i.useAccountSmtp ? '' : i.smtpHost, // Egen SMTP-server visas i avsändarlistan.
      })),
    ]),
  )

  function senderByKey(key) {
    return senders.value.find((s) => s.key === key)
  }

  function signatureFor(sender) {
    return sender?.signature ? `\n\n-- \n${sender.signature}` : ''
  }

  // Välj avsändare som i Thunderbird: den första mottagaradressen (Till före Kopia) som är en egen
  // avsändare, helst i samma konto. Annars kontots egen adress.
  function pickSender(accountId, addresses) {
    for (const address of addresses) {
      const email = address.email.toLowerCase()
      const matches = senders.value.filter((s) => s.email.toLowerCase() === email)
      const match = matches.find((s) => s.accountId === accountId) || matches[0]
      if (match) return match
    }
    return senders.value.find((s) => s.accountId === accountId && s.identityId === null)
  }

  function startCompose(mode = 'new', message = openMessage.value) {
    const accountId = message?.accountId ?? current.accountId ?? accounts.value[0]?.id
    if (!accountId) {
      notify('Lägg till ett konto först', 'error')
      return
    }
    let sender = senders.value.find((s) => s.accountId === accountId && s.identityId === null)
    if (message && (mode === 'reply' || mode === 'replyAll' || mode === 'forward')) {
      sender = pickSender(accountId, [...message.to, ...message.cc])
    } else if (message && mode === 'editDraft') {
      sender = pickSender(accountId, message.from)
    }

    const draft = {
      key: Date.now(),
      accountId,
      senderKey: sender.key,
      to: '',
      cc: '',
      bcc: '',
      subject: '',
      text: signatureFor(sender),
      // HTML-versionen av samma innehåll, för skrivfönstrets formaterade läge.
      html: `<div><br></div>${signatureHtml(sender)}`,
      preferText: false, // Ett utkast i ren text öppnas i ren text, oavsett användarens förval.
      inReplyTo: '',
      references: '',
      replyTo: null,
      forwardOf: null,
      draft: null,
      forwardedAttachments: [],
    }

    if (message && mode !== 'new') {
      const ownAddresses = new Set(senders.value.map((s) => s.email.toLowerCase()))
      const notSelf = (a) => !ownAddresses.has(a.email.toLowerCase())
      const ref = { folder: message.folder, uid: message.uid }
      const body = messagePlainText(message)
      const msgId = message.messageId ? `<${message.messageId}>` : ''

      if (mode === 'reply' || mode === 'replyAll') {
        const replyAddresses = message.replyTo?.length ? message.replyTo : message.from
        draft.to = formatAddressList(replyAddresses)
        if (mode === 'replyAll') {
          const others = [...replyAddresses, ...message.to].filter(notSelf)
          const seen = new Set()
          draft.to = formatAddressList(
            others.filter((a) => !seen.has(a.email.toLowerCase()) && seen.add(a.email.toLowerCase())),
          )
          draft.cc = formatAddressList(message.cc.filter(notSelf))
        }
        draft.subject = prefixSubject('Re:', message.subject)
        const header = `Den ${formatFullDate(message.date)} skrev ${displayName(message.from[0])}:`
        draft.text = `\n\n${header}\n${quoteText(body)}${signatureFor(sender)}`
        draft.html =
          `<div><br></div><div>${escapeHtml(header)}</div>` +
          `<blockquote type="cite" style="${QUOTE_STYLE}">${messageHtml(message)}</blockquote>` +
          `<div><br></div>${signatureHtml(sender)}`
        draft.inReplyTo = msgId
        draft.references = [message.references, msgId].filter(Boolean).join(' ')
        draft.replyTo = ref
      } else if (mode === 'forward') {
        draft.subject = prefixSubject('Fwd:', message.subject)
        const lines = [
          '-------- Vidarebefordrat meddelande --------',
          `Ämne: ${message.subject}`,
          `Datum: ${formatFullDate(message.date)}`,
          `Från: ${formatAddressList(message.from)}`,
          `Till: ${formatAddressList(message.to)}`,
        ]
        if (message.cc.length) lines.push(`Kopia: ${formatAddressList(message.cc)}`)
        draft.text = `${signatureFor(sender)}\n\n${lines.join('\n')}\n\n${body}`
        draft.html =
          `<div><br></div>${signatureHtml(sender)}<div><br></div>` +
          `<div>${lines.map(escapeHtml).join('<br>')}</div><div><br></div>${messageHtml(message)}`
        draft.forwardOf = ref
        draft.forwardedAttachments = message.attachments.filter((a) => !a.inline)
      } else if (mode === 'editDraft') {
        draft.to = formatAddressList(message.to)
        draft.cc = formatAddressList(message.cc)
        draft.subject = message.subject
        draft.text = body
        draft.html = message.html ? sanitizeHtml(message.html) : textToHtml(body)
        draft.preferText = !message.html
        draft.draft = ref
      }
    }
    compose.value = draft
  }

  function composeTo(address) {
    startCompose('new', null)
    if (compose.value) compose.value.to = formatAddress(address)
  }

  async function onMessageSent(accountId) {
    await loadFolders(accountId)
    const f = currentFolderInfo.value
    if (current.accountId === accountId && f && ['sent', 'drafts'].includes(f.specialUse)) {
      await loadMessages({ keepSelection: true })
    }
  }

  return {
    oauthProviders,
    senders,
    senderByKey,
    signatureFor,
    identityDialog,
    thunderbirdDialog,
    layoutDialog,
    backupDialog,
    aiDialog,
    remoteSenders,
    setRemoteSenderTrusted,
    selectView,
    searchDialog,
    search,
    emptySearchForm,
    openSearchResult,
    folderMoveDialog,
    draggedFolder,
    parentPath,
    canMoveFolder,
    moveFolder,
    readerOpen,
    layout,
    user,
    usersDialog,
    passwordDialog,
    reset,
    accounts,
    folders,
    folderErrors,
    cacheStatus,
    loadCacheStatus,
    expanded,
    current,
    messages,
    total,
    loadingMessages,
    listError,
    filter,
    sort,
    currentSort,
    sortParams,
    setSort,
    selection,
    openMessage,
    loadingMessage,
    messageError,
    compose,
    accountDialog,
    contextMenu,
    toasts,
    currentFolderInfo,
    selectedMessages,
    hasMore,
    notify,
    account,
    folderInfo,
    specialFolder,
    toggleExpanded,
    init,
    loadAccounts,
    loadFolders,
    refreshAll,
    createFolder,
    renameFolder,
    deleteFolder,
    saveAccount,
    removeAccount,
    selectFolder,
    loadMessages,
    selectMessage,
    selectAll,
    moveSelection,
    showMessage,
    setFlag,
    deleteMessages,
    moveMessages,
    markJunk,
    archive,
    startCompose,
    composeTo,
    onMessageSent,
  }
})
