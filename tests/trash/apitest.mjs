// End-to-end-test av inställningen "töm papperskorgen automatiskt" (GET/PUT /api/settings) och rensaren som tar
// bort meddelanden som legat längre än trashDays dagar i papperskorgen, mot GreenMail.
// Körs i testmiljön från tests/ai-mock/README.md. Appcontainern måste startas med TRASH_DAY_SECONDS=2 och
// TRASH_PURGE_INTERVAL=2, så att en "dag" tar två sekunder och rensaren går varannan sekund.
import net from 'node:net'

const APP = process.env.APP || 'http://localhost:8082'
const SMTP_PORT = Number(process.env.SMTP_PORT || 13025)
const IMAP_PORT = Number(process.env.IMAP_PORT || 13143)

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
  return { status: res.status, data }
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

// Antal meddelanden i en mapp enligt mejlservern själv ("* N EXISTS").
async function existsOnServer(user, folder) {
  const out = await imap(user, [`SELECT ${folder}`])
  const m = out.match(/\* (\d+) EXISTS/)
  return m ? Number(m[1]) : -1
}

async function waitFor(check, timeoutMs = 30000, every = 500) {
  const until = Date.now() + timeoutMs
  while (Date.now() < until) {
    const result = await check()
    if (result) return result
    await sleep(every)
  }
  return null
}

const USER = `trash-${Date.now().toString(36)}@localhost`
await api('POST', '/login', { username: 'tester', password: 'testerpass1' })

// 1. Standardvärde och validering
let r = await api('GET', '/session')
log(r.data.user && r.data.user.trashDays === 30, 'användaren har 30 dagar som standard', JSON.stringify(r.data.user))
r = await api('GET', '/settings')
log(r.status === 200 && r.data.trashDays === 30, 'GET /settings ger inställningen')
for (const bad of [{ trashDays: -1 }, { trashDays: 'x' }, { trashDays: 3651 }, { trashDays: 1.5 }, {}]) {
  r = await api('PUT', '/settings', bad)
  log(r.status === 400, `PUT /settings avvisar ${JSON.stringify(bad)}`, JSON.stringify(r.data))
}
r = await api('GET', '/settings', undefined, '')
log(r.status === 401, 'inställningarna kräver inloggning')

// 2. Ett konto med fyra mejl, varav två tas bort (hamnar i papperskorgen)
const account = await api('POST', '/accounts', {
  name: 'Papperskorg', email: USER, username: USER, password: 'test',
  imapHost: 'brevduva-aitest-mail', imapPort: 3143, imapSecurity: 'none',
  smtpHost: 'brevduva-aitest-mail', smtpPort: 3025, smtpSecurity: 'none', tlsVerify: false, skipTest: true,
})
const id = account.data.id
log(account.status === 201, 'testkonto skapat')
// GreenMail har ingen papperskorg från början; en mapp som heter Trash känns igen på namnet.
await api('POST', `/accounts/${id}/folders`, { path: 'Trash' })
for (let i = 1; i <= 4; i++) await smtpDeliver(USER, `Mejl ${i}`)
await sleep(500)
await api('POST', `/accounts/${id}/sync`)
const synced = await waitFor(async () => {
  const c = await api('GET', '/cache')
  const a = c.data.accounts.find((x) => x.accountId === id)
  return a && a.complete ? a : null
}, 60000, 1000)
log(!!synced, 'cachen har synkat kontot')
const list = (folder) => api('GET', `/accounts/${id}/messages?folder=${encodeURIComponent(folder)}&pageSize=50`)
const inbox = (await list('INBOX')).data.messages
const uidOf = (subject) => inbox.find((m) => m.subject === subject).uid
let cursor = (await api('GET', `/accounts/${id}/changes`)).data.modseq

r = await api('POST', `/accounts/${id}/messages/delete`, { folder: 'INBOX', uids: [uidOf('Mejl 1'), uidOf('Mejl 2')] })
log(r.status === 200, 'två meddelanden togs bort via API:t')
log((await existsOnServer(USER, 'Trash')) === 2 && (await existsOnServer(USER, 'INBOX')) === 2, 'de hamnade i papperskorgen i stället för att raderas')
const trashListed = await waitFor(async () => ((await list('Trash')).data.total === 2 ? true : null), 30000, 1000)
log(!!trashListed, 'papperskorgen i cachen visar de två meddelandena')

// 3. Tiden börjar räknas när rensaren först ser meddelandet, inte från mejlets datum
r = await api('PUT', '/settings', { trashDays: 1 })
log(r.status === 200 && r.data.trashDays === 1, 'PUT /settings sparar 1 dag')
r = await api('GET', '/session')
log(r.data.user.trashDays === 1, 'sessionen visar det nya värdet')
await sleep(1000)
log((await existsOnServer(USER, 'Trash')) === 2, 'första passet tar inte bort något, även om mejlen är "gamla"')
const purged = await waitFor(async () => ((await existsOnServer(USER, 'Trash')) === 0 ? true : null), 20000, 500)
log(!!purged, 'efter en "dag" (2 s) är papperskorgen tömd')
log((await existsOnServer(USER, 'INBOX')) === 2, 'inkorgen är orörd')
const cacheEmpty = await waitFor(async () => ((await list('Trash')).data.total === 0 ? true : null), 20000, 500)
log(!!cacheEmpty, 'cachen uppdaterades av rensaren')
r = await api('GET', `/accounts/${id}/changes?since=${cursor}`)
const removedFromTrash = r.data.removed.filter((x) => x.folder === 'Trash')
log(removedFromTrash.length === 2, 'appen får gravstenar för det som rensades', JSON.stringify(r.data.removed))

// 4. Det som ett annat e-postprogram lägger i papperskorgen tas också bort efter tiden
await imap(USER, ['SELECT INBOX', `UID COPY ${uidOf('Mejl 3')} Trash`])
log((await existsOnServer(USER, 'Trash')) === 1, 'ett annat program la ett meddelande i papperskorgen')
const purged2 = await waitFor(async () => ((await existsOnServer(USER, 'Trash')) === 0 ? true : null), 20000, 500)
log(!!purged2, 'även det tas bort efter en "dag"')

// 5. 0 = aldrig
r = await api('PUT', '/settings', { trashDays: 0 })
log(r.status === 200 && r.data.trashDays === 0, 'PUT /settings sparar 0 (aldrig)')
r = await api('POST', `/accounts/${id}/messages/delete`, { folder: 'INBOX', uids: [uidOf('Mejl 4')] })
await sleep(7000)
log((await existsOnServer(USER, 'Trash')) === 1, 'med 0 dagar ligger meddelandet kvar i papperskorgen')

// 6. Inställningen är per användare
r = await api('POST', '/users', { username: 'papper', password: 'papperpass1' })
log(r.status === 201, 'en andra användare skapades', JSON.stringify(r.data))
const otherUserId = r.data.id
const other = await fetch(APP + '/api/login', { method: 'POST', headers: { 'X-Brevduva': '1', 'Content-Type': 'application/json' }, body: JSON.stringify({ username: 'papper', password: 'papperpass1' }) })
const otherCookie = other.headers.get('set-cookie').split(';')[0]
r = await api('GET', '/settings', undefined, otherCookie)
log(r.data.trashDays === 30, 'den andra användaren har sitt eget standardvärde')
r = await api('PUT', '/settings', { trashDays: 7 }, otherCookie)
log(r.data.trashDays === 7 && (await api('GET', '/settings')).data.trashDays === 0, 'den enas ändring påverkar inte den andra')

// Städa
await api('DELETE', `/accounts/${id}`)
await api('DELETE', `/users/${otherUserId}`)
await api('PUT', '/settings', { trashDays: 30 })

console.log(failures ? `\n${failures} test misslyckades` : '\nAlla test gick igenom')
process.exit(failures ? 1 : 0)
