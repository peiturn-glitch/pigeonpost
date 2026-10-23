import { locale, t } from './i18n'

const DATE_FORMATS = {
  time: { hour: '2-digit', minute: '2-digit' },
  day: { day: 'numeric', month: 'short' },
  full: { dateStyle: 'short', timeStyle: 'short' },
  long: { dateStyle: 'full', timeStyle: 'short' },
}
const formatters = new Map()

// Datumformat för det språk som visas just nu. Formaterarna skapas en gång per språk.
function dateFormat(name) {
  const key = `${locale()}:${name}`
  let fmt = formatters.get(key)
  if (!fmt) {
    fmt = new Intl.DateTimeFormat(locale(), DATE_FORMATS[name])
    formatters.set(key, fmt)
  }
  return fmt
}

export function formatListDate(epochSeconds) {
  if (!epochSeconds) return ''
  const d = new Date(epochSeconds * 1000)
  const now = new Date()
  if (d.toDateString() === now.toDateString()) return dateFormat('time').format(d)
  if (d.getFullYear() === now.getFullYear()) return `${dateFormat('day').format(d)} ${dateFormat('time').format(d)}`
  return dateFormat('full').format(d)
}

export function formatFullDate(epochSeconds) {
  return epochSeconds ? dateFormat('long').format(new Date(epochSeconds * 1000)) : ''
}

export function formatSize(bytes) {
  if (bytes < 1024) return t('{n} B', { n: bytes })
  if (bytes < 1024 * 1024) return t('{n} kB', { n: (bytes / 1024).toFixed(0) })
  return t('{n} MB', { n: (bytes / 1024 / 1024).toFixed(1) })
}

export function displayName(address) {
  if (!address) return ''
  return address.name || address.email
}

// Sorteringar som listorna och sökningen kan begära av servern. Datum är standard, nyast först.
// label är engelska och översätts med t(label) där den visas.
export const SORT_KEYS = [
  { key: 'date', label: 'Date' },
  { key: 'from', label: 'Sender' },
  { key: 'to', label: 'Recipient' },
  { key: 'subject', label: 'Subject' },
  { key: 'size', label: 'Size' },
]
export const DEFAULT_SORT = { key: 'date', desc: true }

export function isSortKey(key) {
  return SORT_KEYS.some((s) => s.key === key)
}

// Ämnet utan svars- och vidarebefordringsprefix, som i serverns sortering.
function sortableSubject(subject) {
  return (subject || '').replace(/^(\s*(re|sv|fwd|fw|vb|aw|wg|tr)\s*(\[\d+\])?\s*:)+\s*/i, '').toLocaleLowerCase(locale())
}

// Samma ordning som servern ger, för listor som sätts ihop i webbläsaren (t.ex. när vyn över olästa uppdateras).
export function messageComparator({ key, desc }) {
  const collator = new Intl.Collator(locale())
  const value = (m) => {
    if (key === 'subject') return sortableSubject(m.subject)
    if (key === 'from') return displayName(m.from?.[0]).toLocaleLowerCase(locale())
    if (key === 'to') return displayName(m.to?.[0]).toLocaleLowerCase(locale())
    if (key === 'size') return m.size || 0
    return m.date || 0
  }
  const newestFirst = (a, b) => (b.date || 0) - (a.date || 0) || (b.uid || 0) - (a.uid || 0)
  return (a, b) => {
    const x = value(a)
    const y = value(b)
    let order = typeof x === 'number' ? x - y : collator.compare(x, y)
    if (desc) order = -order
    return order || (key === 'date' && !desc ? -newestFirst(a, b) : newestFirst(a, b))
  }
}

// Formaterar en adress för ett adressfält, med citattecken när namnet innehåller specialtecken.
export function formatAddress(address) {
  if (!address) return ''
  if (!address.name) return address.email
  const name = /[",;<>@()[\]\\:]/.test(address.name) ? `"${address.name.replace(/["\\]/g, '\\$&')}"` : address.name
  return `${name} <${address.email}>`
}

// Tolkar en lista med adresser: en per rad, eller åtskilda med komma eller semikolon.
// Varje post kan vara "adress@exempel.se" eller "Namn <adress@exempel.se>".
const plainAddress = /^[^\s<>@]+@[^\s<>@]+\.[^\s<>@]+$/
const namedAddress = /^"?([^"<]*?)"?\s*<\s*([^\s<>@]+@[^\s<>@]+\.[^\s<>@]+)\s*>$/

export function parseAddresses(text) {
  const addresses = []
  const invalid = []
  const seen = new Set()
  for (const part of String(text || '').split(/[\n,;]+/)) {
    const line = part.trim()
    if (!line) continue
    const named = namedAddress.exec(line)
    const email = named ? named[2] : plainAddress.test(line) ? line : ''
    if (!email) {
      invalid.push(line)
      continue
    }
    if (seen.has(email.toLowerCase())) continue
    seen.add(email.toLowerCase())
    addresses.push({ name: named ? named[1].trim() : '', email })
  }
  return { addresses, invalid }
}

export function formatAddressList(list) {
  return (list || []).map(formatAddress).join(', ')
}

// Omvandlar HTML till läsbar text för citat. DOMParser kör inga skript.
export function htmlToText(html) {
  const doc = new DOMParser().parseFromString(html, 'text/html')
  doc.querySelectorAll('script, style, head').forEach((el) => el.remove())
  doc.querySelectorAll('br').forEach((el) => el.replaceWith('\n'))
  doc.querySelectorAll('p, div, tr, li, h1, h2, h3, h4, h5, h6, blockquote, table').forEach((el) => {
    el.append('\n')
  })
  return (doc.body.textContent || '').replace(/\u00a0/g, ' ').replace(/[ \t]+\n/g, '\n').replace(/\n{3,}/g, '\n\n').trim()
}

export function messagePlainText(message) {
  if (message.text && message.text.trim()) return message.text
  if (message.html) return htmlToText(message.html)
  return ''
}

export function quoteText(text) {
  return text
    .split(/\r?\n/)
    .map((line) => (line.startsWith('>') ? `>${line}` : `> ${line}`))
    .join('\n')
}

export function prefixSubject(prefix, subject) {
  const s = subject || ''
  const pattern = prefix === 'Re:' ? /^(re|sv|aw):/i : /^(fwd?|vb):/i
  return pattern.test(s.trim()) ? s : `${prefix} ${s}`
}

const urlPattern = /(https?:\/\/[^\s<>"')\]]+|mailto:[^\s<>"')\]]+)/g

// Delar upp text i text- och länksegment så att länkar kan visas utan v-html.
export function linkify(text) {
  const segments = []
  let last = 0
  for (const match of text.matchAll(urlPattern)) {
    if (match.index > last) segments.push({ type: 'text', value: text.slice(last, match.index) })
    segments.push({ type: 'link', value: match[0] })
    last = match.index + match[0].length
  }
  if (last < text.length) segments.push({ type: 'text', value: text.slice(last) })
  return segments
}

export const mailPresets = {
  gmail: { imapHost: 'imap.gmail.com', imapPort: 993, imapSecurity: 'tls', smtpHost: 'smtp.gmail.com', smtpPort: 465, smtpSecurity: 'tls' },
  outlook: { imapHost: 'outlook.office365.com', imapPort: 993, imapSecurity: 'tls', smtpHost: 'smtp.office365.com', smtpPort: 587, smtpSecurity: 'starttls' },
  icloud: { imapHost: 'imap.mail.me.com', imapPort: 993, imapSecurity: 'tls', smtpHost: 'smtp.mail.me.com', smtpPort: 587, smtpSecurity: 'starttls' },
  yahoo: { imapHost: 'imap.mail.yahoo.com', imapPort: 993, imapSecurity: 'tls', smtpHost: 'smtp.mail.yahoo.com', smtpPort: 465, smtpSecurity: 'tls' },
  greenmail: { imapHost: 'mailserver', imapPort: 3143, imapSecurity: 'none', smtpHost: 'mailserver', smtpPort: 3025, smtpSecurity: 'none' },
}

export const defaultPorts = { imap: { tls: 993, starttls: 143, none: 143 }, smtp: { tls: 465, starttls: 587, none: 25 } }

// Gissar serverinställningar från e-postadressens domän, som Thunderbirds kontoguide.
export function presetForEmail(email) {
  const domain = email.split('@')[1]?.toLowerCase()
  if (!domain) return null
  if (['gmail.com', 'googlemail.com'].includes(domain)) return mailPresets.gmail
  if (['outlook.com', 'hotmail.com', 'live.com', 'live.se', 'hotmail.se', 'outlook.se'].includes(domain)) return mailPresets.outlook
  if (['icloud.com', 'me.com', 'mac.com'].includes(domain)) return mailPresets.icloud
  if (domain.startsWith('yahoo.')) return mailPresets.yahoo
  return { imapHost: `imap.${domain}`, smtpHost: `smtp.${domain}` }
}

// Visningsnamn för specialmappar, på engelska; översätts med t() där de visas (se folderLabel).
export const specialFolderNames = {
  inbox: 'Inbox',
  drafts: 'Drafts',
  sent: 'Sent',
  archive: 'Archive',
  junk: 'Junk',
  trash: 'Trash',
  all: 'All messages',
  flagged: 'Starred',
  important: 'Important',
}

const specialOrder = ['inbox', 'drafts', 'sent', 'archive', 'junk', 'trash']

// Bygger ett mappträd från serverns platta lista.
export function buildFolderTree(folders) {
  const root = { children: [] }
  const byPath = new Map()

  const ensureNode = (path, name, delimiter) => {
    if (byPath.has(path)) return byPath.get(path)
    const node = { path, name, delimiter, selectable: false, specialUse: '', total: 0, unread: 0, children: [] }
    byPath.set(path, node)
    return node
  }

  for (const folder of folders) {
    const delimiter = folder.delimiter
    const parts = delimiter ? folder.path.split(delimiter) : [folder.path]
    let parent = root
    for (let i = 0; i < parts.length; i++) {
      const path = parts.slice(0, i + 1).join(delimiter)
      const existed = byPath.has(path)
      const node = ensureNode(path, parts[i], delimiter)
      if (!existed) parent.children.push(node)
      if (i === parts.length - 1) Object.assign(node, folder, { name: parts[i], children: node.children })
      parent = node
    }
  }

  const sortNodes = (nodes) => {
    nodes.sort((a, b) => {
      const ai = specialOrder.indexOf(a.specialUse)
      const bi = specialOrder.indexOf(b.specialUse)
      if (ai !== bi) return (ai === -1 ? 99 : ai) - (bi === -1 ? 99 : bi)
      return a.name.localeCompare(b.name, locale())
    })
    nodes.forEach((n) => sortNodes(n.children))
  }
  sortNodes(root.children)
  return root.children
}

// Sant om path är mappen själv eller ligger under den.
export function isInsideFolder(path, folder) {
  return path === folder.path || (!!folder.delimiter && path.startsWith(folder.path + folder.delimiter))
}

export function folderLabel(folder) {
  const special = specialFolderNames[folder.specialUse]
  return special ? t(special) : folder.name
}
