const timeFmt = new Intl.DateTimeFormat('sv-SE', { hour: '2-digit', minute: '2-digit' })
const dayFmt = new Intl.DateTimeFormat('sv-SE', { day: 'numeric', month: 'short' })
const fullFmt = new Intl.DateTimeFormat('sv-SE', { dateStyle: 'short', timeStyle: 'short' })
const longFmt = new Intl.DateTimeFormat('sv-SE', { dateStyle: 'full', timeStyle: 'short' })

export function formatListDate(epochSeconds) {
  if (!epochSeconds) return ''
  const d = new Date(epochSeconds * 1000)
  const now = new Date()
  if (d.toDateString() === now.toDateString()) return timeFmt.format(d)
  if (d.getFullYear() === now.getFullYear()) return `${dayFmt.format(d)} ${timeFmt.format(d)}`
  return fullFmt.format(d)
}

export function formatFullDate(epochSeconds) {
  return epochSeconds ? longFmt.format(new Date(epochSeconds * 1000)) : ''
}

export function formatSize(bytes) {
  if (bytes < 1024) return `${bytes} B`
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(0)} kB`
  return `${(bytes / 1024 / 1024).toFixed(1)} MB`
}

export function displayName(address) {
  if (!address) return ''
  return address.name || address.email
}

// Formaterar en adress för ett adressfält, med citattecken när namnet innehåller specialtecken.
export function formatAddress(address) {
  if (!address) return ''
  if (!address.name) return address.email
  const name = /[",;<>@()[\]\\:]/.test(address.name) ? `"${address.name.replace(/["\\]/g, '\\$&')}"` : address.name
  return `${name} <${address.email}>`
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

export const specialFolderNames = {
  inbox: 'Inkorg',
  drafts: 'Utkast',
  sent: 'Skickat',
  archive: 'Arkiv',
  junk: 'Skräppost',
  trash: 'Papperskorg',
  all: 'Alla meddelanden',
  flagged: 'Stjärnmärkt',
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
      return a.name.localeCompare(b.name, 'sv')
    })
    nodes.forEach((n) => sortNodes(n.children))
  }
  sortNodes(root.children)
  return root.children
}

export function folderLabel(folder) {
  return specialFolderNames[folder.specialUse] || folder.name
}
