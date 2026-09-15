import { defineStore } from 'pinia'
import { computed, reactive, ref } from 'vue'
import { api } from './api'
import {
  formatAddress,
  formatAddressList,
  formatFullDate,
  displayName,
  messagePlainText,
  prefixSubject,
  quoteText,
} from './utils'

const PAGE_SIZE = 100

function loadExpanded() {
  try {
    return new Set(JSON.parse(localStorage.getItem('tbk.expanded') || '[]'))
  } catch {
    return new Set()
  }
}

export const useMailStore = defineStore('mail', () => {
  const user = ref(null) // { id, username, isAdmin }
  const oauthProviders = ref({ google: { configured: false, redirectUri: '' } })
  const accounts = ref([])
  const folders = reactive({}) // accountId -> platt mapplista
  const folderErrors = reactive({})
  const expanded = ref(loadExpanded())

  const current = reactive({ accountId: null, folder: null })
  const messages = ref([])
  const total = ref(0)
  const page = ref(0)
  const loadingMessages = ref(false)
  const listError = ref('')
  const filter = reactive({ q: '', unread: false, flagged: false })

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
  const contextMenu = ref(null)
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

  const selectedMessages = computed(() => messages.value.filter((m) => selection.value.has(m.uid)))

  function toggleExpanded(key) {
    const next = new Set(expanded.value)
    if (next.has(key)) next.delete(key)
    else next.add(key)
    expanded.value = next
    try {
      localStorage.setItem('tbk.expanded', JSON.stringify([...next]))
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

  async function refreshAll() {
    await Promise.all(accounts.value.map((a) => loadFolders(a.id)))
    if (current.accountId) await loadMessages({ keepSelection: true })
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

  // --- Meddelandelista --------------------------------------------------

  async function selectFolder(accountId, path) {
    current.accountId = accountId
    current.folder = path
    filter.q = ''
    filter.unread = false
    filter.flagged = false
    openMessage.value = null
    selection.value = new Set()
    messages.value = []
    await loadMessages()
  }

  async function loadMessages({ append = false, keepSelection = false } = {}) {
    if (!current.accountId) return
    const token = ++listRequest
    const nextPage = append ? page.value + 1 : 0
    loadingMessages.value = true
    listError.value = ''
    try {
      const result = await api.messages(current.accountId, {
        folder: current.folder,
        page: nextPage,
        pageSize: PAGE_SIZE,
        q: filter.q,
        unread: filter.unread,
        flagged: filter.flagged,
      })
      if (token !== listRequest) return
      total.value = result.total
      page.value = nextPage
      messages.value = append ? [...messages.value, ...result.messages] : result.messages
      if (!keepSelection && !append) selection.value = new Set()
      else {
        const present = new Set(messages.value.map((m) => m.uid))
        selection.value = new Set([...selection.value].filter((uid) => present.has(uid)))
      }
    } catch (e) {
      if (token === listRequest) listError.value = e.message
    } finally {
      if (token === listRequest) loadingMessages.value = false
    }
  }

  const hasMore = computed(() => messages.value.length < total.value)

  function selectMessage(uid, { ctrl = false, shift = false } = {}) {
    if (shift && anchorUid.value !== null) {
      const uids = messages.value.map((m) => m.uid)
      const a = uids.indexOf(anchorUid.value)
      const b = uids.indexOf(uid)
      if (a !== -1 && b !== -1) {
        const [from, to] = a < b ? [a, b] : [b, a]
        selection.value = new Set(uids.slice(from, to + 1))
        return
      }
    }
    if (ctrl) {
      const next = new Set(selection.value)
      if (next.has(uid)) next.delete(uid)
      else next.add(uid)
      selection.value = next
      anchorUid.value = uid
      return
    }
    selection.value = new Set([uid])
    anchorUid.value = uid
    showMessage(uid)
  }

  function selectAll() {
    selection.value = new Set(messages.value.map((m) => m.uid))
  }

  function moveSelection(offset) {
    if (!messages.value.length) return
    const uids = messages.value.map((m) => m.uid)
    const currentIndex = anchorUid.value !== null ? uids.indexOf(anchorUid.value) : -1
    const next = Math.min(uids.length - 1, Math.max(0, currentIndex + offset))
    selectMessage(uids[next])
  }

  async function showMessage(uid) {
    const token = ++messageRequest
    const accountId = current.accountId
    const folder = current.folder
    loadingMessage.value = true
    messageError.value = ''
    try {
      const msg = await api.message(accountId, folder, uid)
      if (token !== messageRequest) return
      openMessage.value = { ...msg, accountId }
      const item = messages.value.find((m) => m.uid === uid)
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

  // --- Åtgärder på meddelanden --------------------------------------------

  function targetUids(uids) {
    if (uids && uids.length) return uids
    return [...selection.value]
  }

  async function setFlag(flag, value, uids) {
    const list = targetUids(uids)
    if (!list.length) return
    const accountId = current.accountId
    const folder = current.folder
    try {
      await api.setFlags(accountId, folder, list, value ? [flag] : [], value ? [] : [flag])
      for (const m of messages.value) {
        if (!list.includes(m.uid) || m[flag] === value) continue
        if (flag === 'seen') adjustUnread(accountId, folder, value ? -1 : 1)
        m[flag] = value
      }
      if (openMessage.value && list.includes(openMessage.value.uid)) openMessage.value[flag] = value
    } catch (e) {
      notify(e.message, 'error')
    }
  }

  function removeFromList(uids) {
    const removed = new Set(uids)
    const index = messages.value.findIndex((m) => removed.has(m.uid))
    const unreadRemoved = messages.value.filter((m) => removed.has(m.uid) && !m.seen).length
    messages.value = messages.value.filter((m) => !removed.has(m.uid))
    total.value = Math.max(0, total.value - removed.size)
    adjustUnread(current.accountId, current.folder, -unreadRemoved)

    const wasOpen = openMessage.value && removed.has(openMessage.value.uid)
    selection.value = new Set()
    openMessage.value = null
    // Som i Thunderbird: markera nästa meddelande efter borttagning.
    if (wasOpen && messages.value.length) {
      const next = messages.value[Math.min(index, messages.value.length - 1)]
      selectMessage(next.uid)
    }
  }

  async function deleteMessages(uids) {
    const list = targetUids(uids)
    if (!list.length) return
    const accountId = current.accountId
    const inTrash = currentFolderInfo.value?.specialUse === 'trash'
    if (inTrash && !window.confirm(`Radera ${list.length} meddelande(n) permanent?`)) return
    try {
      await api.remove(accountId, current.folder, list)
      removeFromList(list)
      loadFolders(accountId)
    } catch (e) {
      notify(e.message, 'error')
    }
  }

  async function moveMessages(target, uids, sourceFolder) {
    const list = targetUids(uids)
    const folder = sourceFolder || current.folder
    if (!list.length || target === folder) return
    const accountId = current.accountId
    try {
      await api.move(accountId, folder, list, target)
      if (folder === current.folder) removeFromList(list)
      loadFolders(accountId)
      notify(`${list.length} meddelande(n) flyttades`)
    } catch (e) {
      notify(e.message, 'error')
    }
  }

  async function markJunk(uids) {
    const junk = specialFolder(current.accountId, 'junk')
    if (!junk) {
      notify('Kontot har ingen skräppostmapp', 'error')
      return
    }
    await moveMessages(junk.path, uids)
  }

  async function archive(uids) {
    const target = specialFolder(current.accountId, 'archive')
    if (!target) {
      notify('Kontot har ingen arkivmapp', 'error')
      return
    }
    await moveMessages(target.path, uids)
  }

  // --- Skriva meddelanden -----------------------------------------------

  // Alla adresser användaren kan skicka från: varje kontos egen adress plus dess extra avsändare.
  const senders = computed(() =>
    accounts.value.flatMap((a) => [
      { key: `a${a.id}`, accountId: a.id, identityId: null, name: a.name, email: a.email, signature: a.signature, via: '' },
      ...(a.identities || []).map((i) => ({
        key: `a${a.id}-i${i.id}`,
        accountId: a.id,
        identityId: i.id,
        name: i.name,
        email: i.email,
        signature: i.signature,
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
        draft.forwardOf = ref
        draft.forwardedAttachments = message.attachments.filter((a) => !a.inline)
      } else if (mode === 'editDraft') {
        draft.to = formatAddressList(message.to)
        draft.cc = formatAddressList(message.cc)
        draft.subject = message.subject
        draft.text = body
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
    user,
    usersDialog,
    passwordDialog,
    reset,
    accounts,
    folders,
    folderErrors,
    expanded,
    current,
    messages,
    total,
    loadingMessages,
    listError,
    filter,
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
