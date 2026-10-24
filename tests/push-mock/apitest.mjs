// End-to-end-test av push-notiser till Android-appen mot GreenMail och låtsas-FCM (mock.mjs): enhetsregistrering,
// notis vid ny post, sammanslagning av en skur, inga notiser för lästa mejl, Skickat, första synken eller obevakade
// mappar, val av mappar per enhet, ingen dubbelnotis vid flytt, borttagning av avinstallerade enheter, omförsök,
// behörighet mellan användare, förnyad access-token och avregistrering.
// Körs i testmiljön från tests/ai-mock/README.md med appen startad mot låtsas-FCM (se README).
import net from 'node:net'

const APP = process.env.APP || 'http://localhost:8082'
const MOCK = process.env.MOCK || 'http://localhost:9997'
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

const mock = async (path, method = 'GET') => (await fetch(MOCK + path, { method })).json()
const requests = () => mock('/control/requests')
const resetMock = () => mock('/control/reset', 'POST')
// Utskick till en viss token, i ordning.
const sentTo = async (token) => (await requests()).filter((r) => r.token === token)

function rawMessage(subject, body = 'Hej!') {
  return `From: Avsändare <avsandare@exempel.se>\nTo: mottagare@localhost\nSubject: ${subject}\nMessage-ID: <${subject.replace(/\W+/g, '-')}-${Date.now().toString(36)}@exempel.se>\nDate: ${new Date().toUTCString()}\nContent-Type: text/plain; charset=utf-8\n\n${body}\n`
}

function smtpDeliver(to, subject) {
  const raw = rawMessage(subject)
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

// En annan e-postklient mot GreenMail. Ett kommando kan vara en sträng eller { cmd, literal } (t.ex. APPEND).
function imap(user, commands) {
  return new Promise((resolve, reject) => {
    const socket = net.connect(IMAP_PORT, 'localhost')
    const all = [`LOGIN ${user} test`, ...commands, 'LOGOUT']
    let step = 0
    let pendingLiteral = null
    let output = ''
    socket.on('data', (d) => {
      const text = d.toString()
      output += text
      if (pendingLiteral !== null && /^\+/m.test(text)) {
        socket.write(pendingLiteral + '\r\n')
        pendingLiteral = null
        return
      }
      if (/^(\* OK|a\d+ (OK|NO|BAD))/m.test(text) && step < all.length) {
        const next = all[step++]
        if (typeof next === 'string') {
          socket.write(`a${step} ${next}\r\n`)
        } else {
          pendingLiteral = next.literal
          socket.write(`a${step} ${next.cmd} {${Buffer.byteLength(next.literal)}}\r\n`)
        }
      }
    })
    socket.on('end', () => resolve(output))
    socket.on('error', reject)
  })
}

// Lägger in ett meddelande direkt i en mapp, med valfria flaggor, som om det kommit utifrån.
function append(user, folder, subject, flags = '') {
  const literal = Buffer.from(rawMessage(subject), 'utf8').toString('latin1').replace(/\r?\n/g, '\r\n')
  return imap(user, [{ cmd: `APPEND "${folder}" (${flags})`, literal }])
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
const waitForRequests = (token, count, timeoutMs) => waitFor(async () => {
  const list = await sentTo(token)
  return list.length >= count ? list : null
}, timeoutMs)

async function syncAndWait(id, token, count, timeoutMs = 30000) {
  await api('POST', `/accounts/${id}/sync`)
  return waitForRequests(token, count, timeoutMs)
}

const USER = `push-${Date.now().toString(36)}@localhost`
await api('POST', '/login', { username: 'tester', password: 'testerpass1' })

// 1. Enheten registreras före kontot, och den första synken ger ingen notis för det som redan låg i brevlådan
let r = await api('GET', '/push/config')
log(r.status === 200 && r.data.enabled === true && r.data.firebase && r.data.firebase.projectId === 'brevduva-test' && r.data.firebase.senderId === '123456789012' &&
    r.data.firebase.appId === '1:123456789012:android:0123456789abcdef' && r.data.firebase.apiKey === 'AIzaTestKeyNotReal' && r.data.firebase.packageName === 'cc.pigeonpost.app',
    'GET /push/config ger Firebase-klientvärdena ur google-services.json', JSON.stringify(r.data))
log((await api('GET', '/push/config', undefined, '')).status === 401, 'push-inställningarna kräver inloggning')
r = await api('GET', '/devices')
log(r.status === 200 && r.data.pushEnabled === true && r.data.devices.length === 0, 'push är påslagen och inga enheter finns')
r = await api('PUT', '/devices', { token: 'dev-a', name: 'Telefon A', platform: 'android', appVersion: '1.0', notify: {} })
log(r.status === 200 && r.data.ok, 'enheten registreras')
r = await api('PUT', '/devices', {})
log(r.status === 400, 'registrering utan token ger 400')
r = await api('PUT', '/devices', { token: 'dev-a', notify: [] })
log(r.status === 400, 'notify som inte är ett objekt ger 400')

await smtpDeliver(USER, 'Gammal 1')
await smtpDeliver(USER, 'Gammal 2')
await sleep(500)
await resetMock()
const account = await api('POST', '/accounts', {
  name: 'Pushkonto', email: USER, username: USER, password: 'test',
  imapHost: 'brevduva-aitest-mail', imapPort: 3143, imapSecurity: 'none',
  smtpHost: 'brevduva-aitest-mail', smtpPort: 3025, smtpSecurity: 'none', tlsVerify: false, skipTest: true,
})
const id = account.data.id
log(account.status === 201, 'testkonto skapat')
await api('POST', `/accounts/${id}/folders`, { path: 'Sent' })
await api('POST', `/accounts/${id}/folders`, { path: 'Projekt' })
await api('POST', `/accounts/${id}/sync`)
const synced = await waitFor(async () => {
  const c = await api('GET', '/cache')
  const a = c.data.accounts.find((x) => x.accountId === id)
  return a && a.complete ? a : null
}, 60000, 1000)
log(!!synced, 'cachen har synkat kontot')
await sleep(4000)
log((await requests()).length === 0, 'första synken ger ingen notis för gammal post', JSON.stringify(await requests()))
r = await api('PUT', '/devices', { token: 'dev-a', notify: { [id + 1000]: ['INBOX'] } })
log(r.status === 404, 'notify för någon annans konto ger 404')

// 2. Ett nytt mejl ger en notis med rätt innehåll
await smtpDeliver(USER, 'Faktura mars')
let list = await syncAndWait(id, 'dev-a', 1)
log(!!list, 'ett nytt mejl ger en notis inom 30 s')
if (list) {
  const d = list[0].data
  log(d.type === 'new_mail' && d.accountId === String(id) && d.accountName === 'Pushkonto' && d.folder === 'INBOX', 'notisen har typ, konto och mapp', JSON.stringify(d))
  log(d.subject === 'Faktura mars' && d.sender === 'Avsändare' && d.senderEmail === 'avsandare@exempel.se' && /^\d+$/.test(d.uid) && /^\d+$/.test(d.date), 'notisen har ämne, avsändare, uid och datum')
  log(d.count === '1' && JSON.parse(d.items).length === 1 && JSON.parse(d.items)[0].subject === 'Faktura mars', 'count och items stämmer')
  log(list[0].android.priority === 'HIGH' && list[0].android.collapse_key === `acct-${id}` && list[0].android.ttl === '3600s', 'android-delen har hög prioritet, collapse_key och ttl', JSON.stringify(list[0].android))
}

// 3. En skur av tre mejl blir en notis med count 3
await resetMock()
for (const s of ['Skur 1', 'Skur 2', 'Skur 3']) await smtpDeliver(USER, s)
await sleep(300)
list = await syncAndWait(id, 'dev-a', 1)
await sleep(5000)
list = await sentTo('dev-a')
log(list && list.length === 1 && list[0].data.count === '3' && JSON.parse(list[0].data.items).length === 3, 'tre mejl på en gång ger en notis med count 3', list && JSON.stringify(list.map((x) => x.data.count)))
log(list && list.length === 1 && ['Skur 1', 'Skur 2', 'Skur 3'].includes(list[0].data.subject), 'notisen gäller ett av mejlen i skuren')

// 4. Ingen notis för redan lästa mejl, mejl i Skickat eller i en obevakad mapp
await resetMock()
await append(USER, 'INBOX', 'Redan läst', '\\Seen')
await append(USER, 'Sent', 'Skickat av mig')
await append(USER, 'Projekt', 'I projektmappen')
await api('POST', `/accounts/${id}/sync`)
await sleep(10000)
log((await sentTo('dev-a')).length === 0, 'läst mejl, Skickat och obevakad mapp ger ingen notis', JSON.stringify((await sentTo('dev-a')).map((x) => x.data.subject)))

// 5. Mappval per enhet: Projekt bevakas, inkorgen inte
r = await api('PUT', '/devices', { token: 'dev-a', notify: { [id]: ['Projekt'] } })
log(r.status === 200, 'mappvalet sparas')
r = await api('GET', '/devices')
log(r.data.devices.length === 1 && JSON.stringify(r.data.devices[0].notify) === JSON.stringify({ [id]: ['Projekt'] }) && r.data.devices[0].name === 'Telefon A', 'enhetslistan visar mappvalet', JSON.stringify(r.data.devices))
await resetMock()
await append(USER, 'Projekt', 'Projektpost')
list = await syncAndWait(id, 'dev-a', 1)
log(list && list[0].data.folder === 'Projekt' && list[0].data.subject === 'Projektpost', 'ny post i en vald mapp ger notis med mappens namn')
await resetMock()
await smtpDeliver(USER, 'Till inkorgen utan val')
await api('POST', `/accounts/${id}/sync`)
await sleep(8000)
log((await sentTo('dev-a')).length === 0, 'inkorgen ger ingen notis när den inte är vald')
await api('PUT', '/devices', { token: 'dev-a', notify: { [id]: ['INBOX', 'Projekt'] } })

// 6. Ingen andra notis när ett mejl flyttas till en annan bevakad mapp (samma Message-ID)
await resetMock()
await smtpDeliver(USER, 'Flyttas snart')
list = await syncAndWait(id, 'dev-a', 1)
log(!!list, 'mejlet som ska flyttas ger en notis')
if (list) {
  await api('POST', `/accounts/${id}/messages/move`, { folder: 'INBOX', uids: [Number(list[0].data.uid)], target: 'Projekt' })
  await api('POST', `/accounts/${id}/sync`)
  await sleep(10000)
  log((await sentTo('dev-a')).length === 1, 'flytten ger ingen andra notis', JSON.stringify((await sentTo('dev-a')).map((x) => x.data.folder)))
}

// 7. En avinstallerad app (404 UNREGISTERED) tas bort ur enhetslistan
await api('PUT', '/devices', { token: 'bad-1', name: 'Avinstallerad' })
await resetMock()
await smtpDeliver(USER, 'Till den avinstallerade')
list = await syncAndWait(id, 'bad-1', 1)
log(list && list[0].status === 404, 'utskicket till den avinstallerade enheten fick 404')
r = await waitFor(async () => {
  const x = await api('GET', '/devices')
  return x.data.devices.some((d) => d.name === 'Avinstallerad') ? null : x
}, 10000)
log(!!r && r.data.devices.some((d) => d.name === 'Telefon A'), 'enheten togs bort, den andra finns kvar')

// 8. Tillfälligt fel (503) ger ett nytt försök
await api('PUT', '/devices', { token: 'flaky-1', name: 'Ostadig' })
await resetMock()
await smtpDeliver(USER, 'Till den ostadiga')
list = await syncAndWait(id, 'flaky-1', 2, 40000)
log(list && list[0].status === 503 && list[1].status === 200, 'efter 503 görs ett nytt försök som lyckas', list && JSON.stringify(list.map((x) => x.status)))
await api('POST', '/devices/delete', { token: 'flaky-1' })

// 9. En annan användares enhet får inget
await api('POST', '/users', { username: 'pushannan', password: 'annanpass1' })
const other = await fetch(APP + '/api/login', { method: 'POST', headers: { 'X-Brevduva': '1', 'Content-Type': 'application/json' }, body: JSON.stringify({ username: 'pushannan', password: 'annanpass1' }) })
const otherCookie = other.headers.get('set-cookie').split(';')[0]
r = await api('PUT', '/devices', { token: 'other-1', name: 'Annans telefon' }, otherCookie)
log(r.status === 200, 'den andra användaren registrerar en enhet')
await resetMock()
await smtpDeliver(USER, 'Bara till ägaren')
list = await syncAndWait(id, 'dev-a', 1)
log(!!list && (await sentTo('other-1')).length === 0, 'bara kontots ägare får notisen')
r = await api('PUT', '/devices', { token: 'dev-a', name: 'Kapad' }, otherCookie)
log(r.status === 200 && (await api('GET', '/devices')).data.devices.every((d) => d.name !== 'Telefon A'), 'en token som loggar in som någon annan flyttas till den användaren')
await api('PUT', '/devices', { token: 'dev-a', name: 'Telefon A', notify: { [id]: ['INBOX', 'Projekt'] } })

// 10. Access-token som gått ut eller återkallats förnyas
const before = await mock('/control/stats')
await mock('/control/expire-tokens', 'POST')
await resetMock()
await smtpDeliver(USER, 'Efter återkallad token')
list = await syncAndWait(id, 'dev-a', 1)
const after = await mock('/control/stats')
log(!!list && after.unauthenticated > before.unauthenticated && after.tokenRequests > before.tokenRequests, 'efter 401 hämtas en ny access-token och utskicket görs om', JSON.stringify({ before, after }))
log(after.badJwt === 0, 'alla JWT:er godkändes av tokenendpointen')

// 11. Avregistrering
r = await api('POST', '/devices/delete', { token: 'dev-a' })
log(r.status === 200 && (await api('GET', '/devices')).data.devices.length === 0, 'enheten avregistreras')
await resetMock()
await smtpDeliver(USER, 'Efter avregistrering')
await api('POST', `/accounts/${id}/sync`)
await sleep(8000)
log((await requests()).length === 0, 'inga utskick efter avregistrering')

// Städa
const users = await api('GET', '/users')
const annan = users.data.find((u) => u.username === 'pushannan')
if (annan) await api('DELETE', `/users/${annan.id}`)
await api('DELETE', `/accounts/${id}`)

console.log(failures ? `\n${failures} test misslyckades` : '\nAlla test gick igenom')
process.exit(failures ? 1 : 0)
