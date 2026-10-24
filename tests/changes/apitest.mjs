// End-to-end-test av ändringslistan för appen (GET /api/accounts/:id/changes) och sessionsförnyelse mot GreenMail:
// nya meddelanden, flaggor, flytt, borttagning, sidindelning, omnumrerad mapp, rensade gravstenar och behörighet.
// Körs i testmiljön från tests/ai-mock/README.md, efter tests/cache/apitest.mjs.
import { execSync } from 'node:child_process'
import net from 'node:net'

const APP = process.env.APP || 'http://localhost:8082'
const SMTP_PORT = Number(process.env.SMTP_PORT || 13025)
const IMAP_PORT = Number(process.env.IMAP_PORT || 13143)
const DB_CONTAINER = process.env.DB_CONTAINER || 'brevduva-aitest-db'
const DB_PASSWORD = process.env.DB_PASSWORD || 'testpw'

let cookie = ''
let failures = 0
const log = (ok, text, extra = '') => {
  if (!ok) failures++
  console.log(`${ok ? 'OK  ' : 'FEL '} ${text}${extra ? ' — ' + extra : ''}`)
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms))

async function api(method, path, body, useCookie = cookie) {
  const res = await fetch(APP + '/api' + path, {
    method,
    headers: { 'X-Brevduva': '1', 'Content-Type': 'application/json', Cookie: useCookie },
    body: body === undefined ? undefined : JSON.stringify(body),
  })
  const setCookie = res.headers.get('set-cookie')
  if (setCookie && useCookie === cookie) cookie = setCookie.split(';')[0]
  const text = await res.text()
  let data = text
  try { data = JSON.parse(text) } catch {}
  return { status: res.status, data, headers: res.headers }
}

function smtpDeliver(to, subject, body = 'Hej!') {
  const raw = `From: Avsändare <avsandare@exempel.se>\nTo: ${to}\nSubject: ${subject}\nMessage-ID: <${Math.random().toString(36).slice(2)}@exempel.se>\nDate: ${new Date().toUTCString()}\nContent-Type: text/plain; charset=utf-8\n\n${body}\n`
  return new Promise((resolve, reject) => {
    const socket = net.connect(SMTP_PORT, 'localhost')
    const steps = ['HELO test', 'MAIL FROM:<avsandare@exempel.se>', `RCPT TO:<${to}>`, 'DATA',
      Buffer.concat([Buffer.from(Buffer.from(raw, 'utf8').toString('latin1').replace(/\r?\n/g, '\r\n'), 'latin1'), Buffer.from('\r\n.')]), 'QUIT']
    let step = 0
    socket.on('data', () => {
      if (step >= steps.length) return socket.end()
      const next = steps[step++]
      socket.write(Buffer.isBuffer(next) ? Buffer.concat([next, Buffer.from('\r\n')]) : next + '\r\n')
    })
    socket.on('end', resolve)
    socket.on('error', reject)
  })
}

// En annan e-postklient: kör IMAP-kommandon direkt mot GreenMail.
function imap(user, commands) {
  return new Promise((resolve, reject) => {
    const socket = net.connect(IMAP_PORT, 'localhost')
    const all = [`LOGIN ${user} test`, ...commands, 'LOGOUT']
    let step = 0
    let output = ''
    socket.on('data', (d) => {
      output += d.toString()
      if (/^(\* OK|a\d+ (OK|NO|BAD))/m.test(d.toString()) && step < all.length) socket.write(`a${step} ${all[step++]}\r\n`)
    })
    socket.on('end', () => resolve(output))
    socket.on('error', reject)
  })
}

function sql(statement) {
  return execSync(`docker exec ${DB_CONTAINER} mariadb -ubrevduva -p${DB_PASSWORD} brevduva -N -e ${JSON.stringify(statement)}`).toString().trim()
}

async function waitFor(check, timeoutMs = 60000, every = 1000) {
  const until = Date.now() + timeoutMs
  while (Date.now() < until) {
    const result = await check()
    if (result) return result
    await sleep(every)
  }
  return null
}

const USER = `changes-${Date.now().toString(36)}@localhost`
await api('POST', '/login', { username: 'tester', password: 'testerpass1' })
const account = await api('POST', '/accounts', {
  name: 'Ändringar', email: USER, username: USER, password: 'test',
  imapHost: 'brevduva-aitest-mail', imapPort: 3143, imapSecurity: 'none',
  smtpHost: 'brevduva-aitest-mail', smtpPort: 3025, smtpSecurity: 'none', tlsVerify: false, skipTest: true,
})
const id = account.data.id
log(account.status === 201, 'testkonto skapat')
// GreenMail har ingen papperskorg från början; en mapp som heter Trash känns igen på namnet.
await api('POST', `/accounts/${id}/folders`, { path: 'Trash' })

// Lite post innan första synken, så att den inte räknas som ändring senare.
for (let i = 1; i <= 3; i++) await smtpDeliver(USER, `Gammal ${i}`)
await sleep(500)
await api('POST', `/accounts/${id}/sync`)
const synced = await waitFor(async () => {
  const c = await api('GET', '/cache')
  const a = c.data.accounts.find((x) => x.accountId === id)
  return a && a.complete ? a : null
})
log(!!synced, 'cachen har synkat kontot')

// 1. Utan since: bara mappar och aktuellt nummer
let r = await api('GET', `/accounts/${id}/changes`)
log(r.status === 200 && r.data.modseq > 0 && r.data.messages.length === 0 && r.data.removed.length === 0, 'since=0 ger bara mapplistan och numret', JSON.stringify(r.data).slice(0, 200))
const inbox = r.data.folders.find((f) => f.path === 'INBOX')
log(inbox && inbox.specialUse === 'inbox' && inbox.cached === true && inbox.headersComplete === true && inbox.resetModseq === 0, 'mapplistan har cached, headersComplete och resetModseq', JSON.stringify(inbox))
let cursor = r.data.modseq
log(r.data.since === 0 && r.data.more === false, 'since och more i svaret')

// 2. Tre nya meddelanden blir tre rader med folder och modseq över markören
for (let i = 1; i <= 3; i++) await smtpDeliver(USER, `Ny ${i}`)
await sleep(500)
await api('POST', `/accounts/${id}/sync`)
r = await waitFor(async () => {
  const x = await api('GET', `/accounts/${id}/changes?since=${cursor}`)
  return x.data.messages.length >= 3 ? x : null
}, 30000)
log(r && r.data.messages.length === 3, 'tre nya meddelanden kommer som ändringar', r && JSON.stringify(r.data.messages.map((m) => m.subject)))
log(r && r.data.messages.every((m) => m.folder === 'INBOX' && m.modseq > cursor && m.seen === false && typeof m.uid === 'number'), 'raderna har folder, modseq, flaggor och uid')
log(r && r.data.modseq > cursor && r.data.more === false, 'markören flyttas fram')
const newUids = r.data.messages.map((m) => m.uid)
cursor = r.data.modseq
r = await api('GET', `/accounts/${id}/changes?since=${cursor}`)
log(r.data.messages.length === 0 && r.data.removed.length === 0 && r.data.modseq === cursor, 'inget nytt efter markören')

// 3. Flaggor via API ger en rad med nya flaggor
await api('POST', `/accounts/${id}/messages/flags`, { folder: 'INBOX', uids: [newUids[0]], add: ['seen', 'flagged'], remove: [] })
r = await api('GET', `/accounts/${id}/changes?since=${cursor}`)
log(r.data.messages.length === 1 && r.data.messages[0].uid === newUids[0] && r.data.messages[0].seen && r.data.messages[0].flagged, 'flaggor via appen blir en ändring', JSON.stringify(r.data.messages))
cursor = r.data.modseq

// 4. Flaggor från en annan klient hittas vid avstämning
await imap(USER, ['SELECT INBOX', `UID STORE ${newUids[1]} +FLAGS (\\Seen)`])
await api('POST', `/accounts/${id}/sync`)
r = await waitFor(async () => {
  await api('POST', `/accounts/${id}/sync`)
  const x = await api('GET', `/accounts/${id}/changes?since=${cursor}`)
  return x.data.messages.some((m) => m.uid === newUids[1] && m.seen) ? x : null
}, 90000, 3000)
log(!!r, 'flagga satt från en annan klient blir en ändring efter avstämning')
if (r) cursor = r.data.modseq

// 5. Flytt: gravsten i källan och ny rad i målet
await api('POST', `/accounts/${id}/folders`, { path: 'Flyttmål' })
await api('POST', `/accounts/${id}/messages/move`, { folder: 'INBOX', uids: [newUids[2]], target: 'Flyttmål' })
r = await api('GET', `/accounts/${id}/changes?since=${cursor}`)
log(r.data.removed.some((t) => t.folder === 'INBOX' && t.uid === newUids[2] && t.modseq > cursor), 'flytt ger en gravsten i källmappen', JSON.stringify(r.data.removed))
r = await waitFor(async () => {
  const x = await api('GET', `/accounts/${id}/changes?since=${cursor}`)
  return x.data.messages.some((m) => m.folder === 'Flyttmål' && m.subject === 'Ny 3') ? x : null
}, 60000)
log(!!r, 'det flyttade meddelandet dyker upp i målmappen')
log(r && r.data.folders.some((f) => f.path === 'Flyttmål'), 'den nya mappen finns i mapplistan')
if (r) cursor = r.data.modseq

// 6. Borttagning: till papperskorgen, och permanent ur papperskorgen
const trash = r && r.data.folders.find((f) => f.specialUse === 'trash')
log(!!trash, 'kontot har en papperskorg', trash && trash.path)
await api('POST', `/accounts/${id}/messages/delete`, { folder: 'INBOX', uids: [newUids[0]] })
r = await api('GET', `/accounts/${id}/changes?since=${cursor}`)
log(r.data.removed.some((t) => t.folder === 'INBOX' && t.uid === newUids[0]), 'borttagning ger en gravsten')
r = await waitFor(async () => {
  const x = await api('GET', `/accounts/${id}/changes?since=${cursor}`)
  const hit = x.data.messages.find((m) => trash && m.folder === trash.path && m.subject === 'Ny 1')
  return hit ? { x, hit } : null
}, 60000)
log(!!r, 'meddelandet finns i papperskorgen i ändringslistan')
if (r) {
  cursor = r.x.data.modseq
  await api('POST', `/accounts/${id}/messages/delete`, { folder: trash.path, uids: [r.hit.uid] })
  const x = await api('GET', `/accounts/${id}/changes?since=${cursor}`)
  log(x.data.removed.some((t) => t.folder === trash.path && t.uid === r.hit.uid), 'permanent borttagning ur papperskorgen ger en gravsten')
  cursor = x.data.modseq
}

// 7. Sidindelning: tre synkomgångar med fyra meddelanden var, hämtade med limit=5, utan luckor och utan att
//    dela ett nummer.
for (let round = 1; round <= 3; round++) {
  for (let i = 1; i <= 4; i++) await smtpDeliver(USER, `Sida ${round}-${i}`)
  await sleep(500)
  await api('POST', `/accounts/${id}/sync`)
  await waitFor(async () => {
    const x = await api('GET', `/accounts/${id}/changes?since=${cursor}`)
    return x.data.messages.filter((m) => m.subject.startsWith('Sida ')).length >= round * 4 ? x : null
  }, 30000)
}
let pages = 0
const seen = new Set()
let pageCursor = cursor
let last
do {
  last = await api('GET', `/accounts/${id}/changes?since=${pageCursor}&limit=5`)
  pages++
  for (const m of last.data.messages) seen.add(`${m.folder}:${m.uid}`)
  const groups = new Set(last.data.messages.map((m) => m.modseq))
  log(last.data.messages.length <= 5 || groups.size === 1, `sida ${pages}: högst fem rader eller ett enda nummer`, `${last.data.messages.length} rader, ${groups.size} nummer`)
  pageCursor = last.data.modseq
} while (last.data.more && pages < 10)
log(pages >= 3 && seen.size === 12, 'sidindelningen ger alla tolv meddelanden utan dubbletter', `${pages} sidor, ${seen.size} rader`)
cursor = pageCursor

// 8. Omnumrerad mapp (UIDVALIDITY) simulerad i databasen: resetModseq flyttas fram och mappen fylls om
sql(`UPDATE cache_folders SET synced_uid_validity = 1 WHERE account_id = ${id} AND path = 'INBOX'`)
await api('POST', `/accounts/${id}/sync`)
r = await waitFor(async () => {
  const x = await api('GET', `/accounts/${id}/changes?since=${cursor}`)
  const f = x.data.folders.find((f) => f.path === 'INBOX')
  return f && f.resetModseq > cursor && f.headersComplete ? x : null
}, 60000)
log(!!r, 'omnumrerad mapp får resetModseq över markören')
log(r && r.data.messages.filter((m) => m.folder === 'INBOX').length >= 12, 'mappens meddelanden kommer på nytt efter omnumreringen', r && String(r.data.messages.length))
if (r) cursor = r.data.modseq

// 9. Mappbyte och borttagning av mapp syns i mapplistan
await api('POST', `/accounts/${id}/folders/rename`, { from: 'Flyttmål', to: 'Nyttnamn' })
r = await api('GET', `/accounts/${id}/changes?since=${cursor}`)
log(!r.data.folders.some((f) => f.path === 'Flyttmål') && r.data.folders.some((f) => f.path === 'Nyttnamn'), 'namnbyte: gamla sökvägen borta, nya finns')
await api('POST', `/accounts/${id}/folders/delete`, { path: 'Nyttnamn' })
r = await api('GET', `/accounts/${id}/changes?since=${cursor}`)
log(!r.data.folders.some((f) => f.path === 'Nyttnamn'), 'borttagen mapp försvinner ur mapplistan')
cursor = r.data.modseq

// 10. För gammalt eller okänt märke ger 410
const pruned = sql('SELECT pruned_modseq FROM cache_seq WHERE id = 1')
sql(`UPDATE cache_seq SET pruned_modseq = ${cursor} WHERE id = 1`)
r = await api('GET', `/accounts/${id}/changes?since=${cursor - 1}`)
log(r.status === 410, 'märke före rensade gravstenar ger 410', `${r.status} ${JSON.stringify(r.data)}`)
r = await api('GET', `/accounts/${id}/changes?since=${cursor}`)
log(r.status === 200, 'märke lika med rensningsgränsen går bra')
sql(`UPDATE cache_seq SET pruned_modseq = ${pruned} WHERE id = 1`)
r = await api('GET', `/accounts/${id}/changes?since=${cursor + 1000000}`)
log(r.status === 410, 'märke efter det senaste numret ger 410')
r = await api('GET', `/accounts/${id}/changes?since=abc`)
log(r.status === 400, 'ogiltigt since ger 400')

// 11. Behörighet: en annan användare ser inte kontot
const admin = cookie
await api('POST', '/users', { username: 'annan', password: 'annanpass1' })
const other = await fetch(APP + '/api/login', { method: 'POST', headers: { 'X-Brevduva': '1', 'Content-Type': 'application/json' }, body: JSON.stringify({ username: 'annan', password: 'annanpass1' }) })
const otherCookie = other.headers.get('set-cookie').split(';')[0]
r = await api('GET', `/accounts/${id}/changes`, undefined, otherCookie)
log(r.status === 404, 'annan användare får 404')

// 12. Sessionsförnyelse: ny cookie, gammal ogiltig, expiresAt i /session
cookie = admin
let session = await api('GET', '/session')
log(session.data.authenticated && session.data.expiresAt > Date.now() / 1000 + 13 * 86400, '/session visar expiresAt', String(session.data.expiresAt))
const oldCookie = cookie
const renewed = await api('POST', '/session/renew')
log(renewed.status === 200 && renewed.data.ok && renewed.data.expiresAt > 0, 'förnyelsen svarar med expiresAt')
log(cookie !== oldCookie && cookie.startsWith('brevduva_session='), 'förnyelsen sätter en ny cookie')
const withOld = await api('GET', '/session', undefined, oldCookie)
log(withOld.data.authenticated === false, 'den gamla sessionen gäller inte längre')
session = await api('GET', '/session')
log(session.data.authenticated === true, 'den nya sessionen gäller')
const unauth = await fetch(APP + '/api/session/renew', { method: 'POST', headers: { 'X-Brevduva': '1' } })
log(unauth.status === 401, 'förnyelse utan inloggning ger 401')

// 13. Gzip: svaret komprimeras när klienten ber om det
const gz = await fetch(APP + `/api/accounts/${id}/changes?since=0`, { headers: { Cookie: cookie, 'Accept-Encoding': 'gzip' } })
log(gz.ok && gz.headers.get('content-encoding') === 'gzip', 'svaret gzippas på begäran', String(gz.headers.get('content-encoding')))

// Texter i bunt för appens förhämtning: allt som finns i cachen i ett svar, resten i missing.
{
  const inbox = (await api('GET', `/accounts/${id}/messages?folder=INBOX&pageSize=100`)).data.messages || []
  const uids = inbox.map((m) => m.uid)
  const bulk = await api('POST', `/accounts/${id}/messages/bodies`, { folder: 'INBOX', uids: [...uids, 987654321] })
  const got = bulk.data.messages || []
  log(bulk.status === 200 && got.length + (bulk.data.missing || []).length === uids.length + 1 && bulk.data.missing.includes(987654321),
      'POST /messages/bodies ger texterna i bunt och listar det som saknas', `${got.length} texter, ${(bulk.data.missing || []).length} saknas av ${uids.length + 1}`)
  log(got.every((m) => m.uid && m.folder === 'INBOX' && typeof m.text === 'string' && typeof m.html === 'string' && Array.isArray(m.attachments) && m.cached === true),
      'varje text i bunten har samma form som GET /message')
  const seenBefore = inbox.map((m) => m.seen)
  const after = (await api('GET', `/accounts/${id}/messages?folder=INBOX&pageSize=100`)).data.messages.map((m) => m.seen)
  log(JSON.stringify(seenBefore) === JSON.stringify(after), 'bunthämtningen märker inget som läst')
  const tooMany = await api('POST', `/accounts/${id}/messages/bodies`, { folder: 'INBOX', uids: Array.from({ length: 101 }, (_, i) => i + 1) })
  log(tooMany.status === 400, 'högst 100 meddelanden per anrop', String(tooMany.status))
}

// Städa
const users = await api('GET', '/users')
const annan = users.data.find((u) => u.username === 'annan')
if (annan) await api('DELETE', `/users/${annan.id}`)
await api('DELETE', `/accounts/${id}`)

console.log(failures ? `\n${failures} test misslyckades` : '\nAlla test gick igenom')
process.exit(failures ? 1 : 0)
