// End-to-end-test av mejlcachen mot GreenMail: synk, läsning ur cachen, sökning i text, sortering, ändringar via
// appen och från en annan klient, samt läsning när mejlservern inte går att nå.
// Körs i testmiljön från tests/ai-mock/README.md, med GreenMails IMAP-port öppnad: -p 127.0.0.1:13143:3143
import { execSync } from 'node:child_process'
import net from 'node:net'

const APP = process.env.APP || 'http://localhost:8082'
const SMTP_PORT = Number(process.env.SMTP_PORT || 13025)
const IMAP_PORT = Number(process.env.IMAP_PORT || 13143)
const MAIL_CONTAINER = 'brevduva-aitest-mail'
const NETWORK = 'brevduva-aitest'

let cookie = ''
let failures = 0
const log = (ok, text, extra = '') => {
  if (!ok) failures++
  console.log(`${ok ? 'OK  ' : 'FEL '} ${text}${extra ? ' — ' + extra : ''}`)
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms))

async function api(method, path, body) {
  const started = Date.now()
  const res = await fetch(APP + '/api' + path, {
    method,
    headers: { 'X-Brevduva': '1', 'Content-Type': 'application/json', Cookie: cookie },
    body: body === undefined ? undefined : JSON.stringify(body),
  })
  const setCookie = res.headers.get('set-cookie')
  if (setCookie) cookie = setCookie.split(';')[0]
  const text = await res.text()
  let data = text
  try { data = JSON.parse(text) } catch {}
  return { status: res.status, data, ms: Date.now() - started }
}

function smtpDeliver(to, raw) {
  return new Promise((resolve, reject) => {
    const socket = net.connect(SMTP_PORT, 'localhost')
    const steps = ['HELO test', 'MAIL FROM:<avsandare@exempel.se>', `RCPT TO:<${to}>`, 'DATA', Buffer.concat([Buffer.from((Buffer.isBuffer(raw) ? raw.toString('latin1') : Buffer.from(raw, 'utf8').toString('latin1')).replace(/\r?\n/g, '\r\n'), 'latin1'), Buffer.from('\r\n.')]), 'QUIT']
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

const USER = 'cache@localhost'
const plain = (subject, text, extra = '') => `From: Avsändare <avsandare@exempel.se>\nTo: ${USER}\nSubject: ${subject}\n${extra}MIME-Version: 1.0\nContent-Type: text/plain; charset=utf-8\n\n${text}\n`

await api('POST', '/login', { username: 'tester', password: 'testerpass1' })
const enabled = await api('GET', '/cache')
log(enabled.data.enabled === true, 'mejlcachen är påslagen')

// Post som finns innan kontot läggs till
for (let i = 1; i <= 12; i++) await smtpDeliver(USER, plain(`Meddelande ${i}`, `Innehåll nummer ${i}.`))
await smtpDeliver(USER, plain('Recept', 'Här kommer receptet på kantarellsoppa med timjan.'))
await smtpDeliver(USER, [
  'From: Butik <butik@exempel.se>', `To: ${USER}`, 'Subject: Nyhetsbrev med bild', 'MIME-Version: 1.0',
  'Content-Type: multipart/related; boundary="rel"', '',
  '--rel', 'Content-Type: text/html; charset=utf-8', '', '<html><body><p>Veckans <b>erbjudande</b> på lingonsylt</p><img src="cid:logga@butik"></body></html>',
  '--rel', 'Content-Type: image/png', 'Content-Transfer-Encoding: base64', 'Content-ID: <logga@butik>', 'Content-Disposition: inline; filename="logga.png"', '',
  'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==',
  '--rel--', '',
].join('\n'))
// Stort meddelande: texten först, sedan en bilaga på ungefär 900 kB.
await smtpDeliver(USER, [
  'From: Kollega <kollega@exempel.se>', `To: ${USER}`, 'Subject: Stor bilaga', 'MIME-Version: 1.0',
  'Content-Type: multipart/mixed; boundary="mix"', '',
  '--mix', 'Content-Type: text/plain; charset=utf-8', '', 'Rapporten om blåbärsskörden ligger bifogad.',
  '--mix', 'Content-Type: application/pdf; name="rapport.pdf"', 'Content-Transfer-Encoding: base64', 'Content-Disposition: attachment; filename="rapport.pdf"', '',
  ...Array(12000).fill('JVBERi0xLjQKJcfsj6IKNSAwIG9iago8PC9MZW5ndGggNiAwIFIvRmlsdGVyIC9GbGF0ZURlY29kZT4+'),
  '--mix--', '',
].join('\n'))
// Ogiltig UTF-8 (latin1-byte utan angiven teckenkodning) får inte stoppa synken.
await smtpDeliver(USER, Buffer.concat([Buffer.from(`From: Gammal <gammal@exempel.se>\nTo: ${USER}\nSubject: Trasig `, 'latin1'), Buffer.from([0xe5, 0xe4, 0xf6]), Buffer.from('\nMIME-Version: 1.0\nContent-Type: text/plain\n\nText med ', 'latin1'), Buffer.from([0xe5, 0xe4, 0xf6, 0xff]), Buffer.from(' i.\n', 'latin1')]))
await sleep(500)

const account = await api('POST', '/accounts', {
  name: 'Cache', email: USER, username: USER, password: 'test',
  imapHost: MAIL_CONTAINER, imapPort: 3143, imapSecurity: 'none',
  smtpHost: MAIL_CONTAINER, smtpPort: 3025, smtpSecurity: 'none', tlsVerify: false, skipTest: true,
})
const id = account.data.id
for (const path of ['Sent', 'Trash', 'Projekt']) await api('POST', `/accounts/${id}/folders`, { path })

async function waitFor(what, check, seconds = 90) {
  for (let i = 0; i < seconds * 2; i++) {
    const status = (await api('GET', '/cache')).data.accounts.find((a) => a.accountId === id)
    if (status && check(status)) return status
    await sleep(500)
  }
  throw new Error('väntade förgäves på: ' + what)
}
const sync = async () => (await api('POST', `/accounts/${id}/sync`)).data.account
const list = (folder = 'INBOX', query = '') => api('GET', `/accounts/${id}/messages?folder=${encodeURIComponent(folder)}&pageSize=100${query}`)

// 1. Första synken
const synced = await waitFor('första synken', (s) => s.complete && s.state === 'idle')
log(synced.messages === 16 && synced.bodies === 16, 'första synken hämtar alla rubriker och all text', JSON.stringify(synced))

const inbox = await list()
log(inbox.data.cached === true && inbox.data.total === 16 && inbox.data.messages[0].subject.startsWith('Trasig'), 'listan kommer ur cachen, nyast först', `${inbox.data.total} st, ${inbox.ms} ms`)
const find = (messages, subject) => messages.find((m) => m.subject === subject)
const recipe = find(inbox.data.messages, 'Recept')
log(recipe && recipe.from[0].email === 'avsandare@exempel.se' && recipe.seen === false && recipe.date > 0, 'rubriker, avsändare och flaggor finns med')
log(inbox.data.messages[0].subject.includes('�'), 'ogiltig UTF-8 i ämnet ersätts i stället för att stoppa synken', inbox.data.messages[0].subject)

const folders = await api('GET', `/accounts/${id}/folders`)
const inboxFolder = folders.data.find((f) => f.path === 'INBOX')
log(inboxFolder.total === 16 && inboxFolder.unread === 16 && folders.data.find((f) => f.path === 'Trash')?.specialUse === 'trash', 'mapplistan med antal kommer ur cachen', JSON.stringify(inboxFolder))

// 2. Sökning
const bodySearch = await list('INBOX', '&q=kantarellsoppa')
log(bodySearch.data.total === 1 && bodySearch.data.messages[0].subject === 'Recept', 'sökning hittar ord som bara finns i texten')
const prefixSearch = await list('INBOX', '&q=kantarell')
log(prefixSearch.data.total === 1, 'sökning matchar början av ord')
const htmlSearch = await list('INBOX', '&q=lingonsylt')
log(htmlSearch.data.total === 1 && htmlSearch.data.messages[0].subject === 'Nyhetsbrev med bild', 'sökning hittar text i HTML-meddelanden')
const subjectSearch = await list('INBOX', '&q=' + encodeURIComponent('Meddelande 1'))
log(subjectSearch.data.total === 4, 'sökning i ämnet fungerar som förut (Meddelande 1, 10, 11, 12)', String(subjectSearch.data.total))
const senderSearch = await list('INBOX', '&q=kollega')
log(senderSearch.data.total === 1, 'sökning i avsändare')
const injection = await list('INBOX', '&q=' + encodeURIComponent('+"\'%_ ) OR 1=1 -- ~*'))
log(injection.status === 200 && injection.data.total === 0, 'specialtecken i sökningen är ofarliga', JSON.stringify(injection.data).slice(0, 100))
const unreadOnly = await list('INBOX', '&unread=1')
log(unreadOnly.data.total === 16, 'filtret Olästa räknas ur cachen')

// 2b. Sortering. Utan Date-rubrik får meddelandena ankomsttiden som datum, och vid lika datum kommer högst UID först.
const subjectsOf = (r) => r.data.messages.map((m) => m.subject)
const oldestFirst = await list('INBOX', '&order=asc')
log(subjectsOf(oldestFirst)[0] === 'Meddelande 1' && subjectsOf(oldestFirst).at(-1).startsWith('Trasig'), 'stigande datum ger äldst först', subjectsOf(oldestFirst).join(' | '))
const bySubject = subjectsOf(await list('INBOX', '&sort=subject&order=asc'))
log(bySubject[0] === 'Meddelande 1' && bySubject[4] === 'Meddelande 2' && bySubject.indexOf('Nyhetsbrev med bild') < bySubject.indexOf('Recept') && bySubject.at(-1).startsWith('Trasig'),
    'sortering efter ämne', bySubject.join(' | '))
log(subjectsOf(await list('INBOX', '&sort=subject'))[0].startsWith('Trasig'), 'fallande är standardriktningen')
const bySender = (await list('INBOX', '&sort=from')).data.messages.map((m) => m.from[0].name)
log(bySender.slice(0, 4).join() === 'Kollega,Gammal,Butik,Avsändare', 'sortering efter avsändare går på namnet', bySender.slice(0, 5).join())
const senderAsc = (await list('INBOX', '&sort=from&order=asc')).data.messages
log(senderAsc[0].subject === 'Recept' && senderAsc.at(-1).from[0].name === 'Kollega', 'vid lika avsändare kommer nyaste först', senderAsc[0].subject)
log(subjectsOf(await list('INBOX', '&sort=size'))[0] === 'Stor bilaga', 'sortering efter storlek')
const bySizeAsc = await api('GET', `/accounts/${id}/messages?folder=INBOX&sort=size&order=asc&pageSize=3&page=1`)
log(bySizeAsc.data.messages.length === 3 && bySizeAsc.data.total === 16 && bySizeAsc.data.messages.every((m) => m.subject !== 'Stor bilaga'), 'sorteringen går att bläddra i sidor')
log((await list('INBOX', '&sort=hemlig')).status === 400 && (await api('POST', '/search', { q: 'x', sort: 'hemlig' })).status === 400, 'okänd sortering avvisas')
const searchDefault = subjectsOf(await api('POST', '/search', { q: 'Meddelande', pageSize: 100 }))
const searchAsc = subjectsOf(await api('POST', '/search', { q: 'Meddelande', sort: 'subject', order: 'asc', pageSize: 100 }))
log(searchDefault.length === 12 && searchDefault[0] === 'Meddelande 12' && searchDefault.at(-1) === 'Meddelande 1' && searchAsc[0] === 'Meddelande 1' && searchAsc.at(-1) === 'Meddelande 9',
    'sökningen i alla mappar sorteras också, nyast först från början', `${searchDefault.slice(0, 3).join()} … ${searchAsc.slice(0, 3).join()}`)

// 3. Läsning
const read = await api('GET', `/accounts/${id}/message?folder=INBOX&uid=${recipe.uid}`)
log(read.data.cached === true && read.data.text.includes('kantarellsoppa') && read.data.seen === true, 'meddelandet läses ur cachen och märks som läst', `${read.ms} ms`)
log((await list()).data.messages.find((m) => m.uid === recipe.uid).seen === true && (await api('GET', `/accounts/${id}/folders`)).data.find((f) => f.path === 'INBOX').unread === 15,
    'listan och antalet olästa uppdateras direkt')
await sleep(1500)
const serverFlags = await imap(USER, ['SELECT INBOX', `UID FETCH ${recipe.uid} (FLAGS)`])
log(/FLAGS \([^)]*\\Seen/.test(serverFlags), 'mejlservern får flaggan strax efteråt')

const newsletter = find(inbox.data.messages, 'Nyhetsbrev med bild')
const html = await api('GET', `/accounts/${id}/message?folder=INBOX&uid=${newsletter.uid}&markSeen=0`)
const inline = html.data.attachments.find((a) => a.inline)
log(html.data.cached && html.data.html.includes('cid:logga@butik') && !html.data.html.includes('data:image') && inline?.contentId === 'logga@butik',
    'HTML sparas utan inbäddade bilder; bilden pekas ut med sitt Content-ID', JSON.stringify(html.data.attachments))
const image = await fetch(`${APP}/api/accounts/${id}/attachment?folder=INBOX&uid=${newsletter.uid}&index=${inline.index}&download=0`, { headers: { Cookie: cookie } })
log(image.status === 200 && image.headers.get('content-type') === 'image/png', 'den inbäddade bilden hämtas som bilaga')

const big = find(inbox.data.messages, 'Stor bilaga')
const bigRead = await api('GET', `/accounts/${id}/message?folder=INBOX&uid=${big.uid}&markSeen=0`)
log(bigRead.status === 200 && bigRead.data.text.includes('blåbärsskörden') && bigRead.data.attachments[0]?.filename === 'rapport.pdf' && bigRead.data.attachments[0].size === 720000,
    'ett stort meddelande läses från mejlservern första gången, med hela bilagelistan', JSON.stringify(bigRead.data.attachments))
const bigAgain = await api('GET', `/accounts/${id}/message?folder=INBOX&uid=${big.uid}&markSeen=0`)
log(bigAgain.data.cached === true && bigAgain.data.partial === false && bigAgain.data.attachments[0].size === 720000, 'därefter ligger det komplett i cachen')
const bigSearch = await list('INBOX', '&q=' + encodeURIComponent('blåbärsskörden'))
log(bigSearch.data.total === 1, 'texten i stora meddelanden är sökbar', JSON.stringify(bigSearch.data).slice(0, 200))

// 4. Ändringar via appen
const m5 = find(inbox.data.messages, 'Meddelande 5')
await api('POST', `/accounts/${id}/messages/flags`, { folder: 'INBOX', uids: [m5.uid], add: ['flagged'] })
log((await list('INBOX', '&flagged=1')).data.total === 1, 'stjärnmärkning syns direkt i cachen')

const m6 = find(inbox.data.messages, 'Meddelande 6')
await api('POST', `/accounts/${id}/messages/move`, { folder: 'INBOX', uids: [m6.uid], target: 'Projekt' })
const afterMove = await list()
const project = await list('Projekt')
log(afterMove.data.total === 15 && !find(afterMove.data.messages, 'Meddelande 6'), 'ett flyttat meddelande försvinner direkt ur källmappen')
log(project.data.total === 1 && project.data.messages[0].subject === 'Meddelande 6', 'målmappen visar meddelandet direkt', project.data.cached ? 'ur cachen' : 'från mejlservern tills den stämts av')
await sync()
await waitFor('avstämning av målmappen', (s) => s.complete && s.state === 'idle')
const projectCached = await list('Projekt')
log(projectCached.data.cached === true && projectCached.data.total === 1, 'efter avstämningen kommer målmappen ur cachen')
const moved = await api('GET', `/accounts/${id}/message?folder=Projekt&uid=${projectCached.data.messages[0].uid}&markSeen=0`)
log(moved.data.cached === true && moved.data.text.includes('nummer 6'), 'och det flyttade meddelandets text är sparad')

const m7 = find(inbox.data.messages, 'Meddelande 7')
await api('POST', `/accounts/${id}/messages/delete`, { folder: 'INBOX', uids: [m7.uid] })
log((await list()).data.total === 14, 'borttagning syns direkt')
await sync()
log((await list('Trash')).data.messages[0]?.subject === 'Meddelande 7', 'det borttagna ligger i papperskorgen')

const sent = await api('POST', `/accounts/${id}/send`, { to: USER, subject: 'Skickat via appen', text: 'Hej från appen' })
const sentList = await list('Sent')
log(sent.status === 200 && sentList.data.messages[0]?.subject === 'Skickat via appen', 'kopian i Skickat syns direkt efter sändning')

// 5. Ändringar från en annan klient
const m8 = find(inbox.data.messages, 'Meddelande 8')
const m9 = find(inbox.data.messages, 'Meddelande 9')
await imap(USER, ['SELECT INBOX', `UID STORE ${m8.uid} +FLAGS (\\Seen \\Flagged)`, `UID STORE ${m9.uid} +FLAGS (\\Deleted)`, 'EXPUNGE'])
await smtpDeliver(USER, plain('Nytt under tiden', 'Det här kom medan appen var öppen.'))
await smtpDeliver(USER, plain('SV: Re: Aha, ett svar', 'Svarsprefix ska inte styra sorteringen.', 'Date: Mon, 1 Jan 2018 10:00:00 +0100\n'))
await sleep(500)
await sync()
await waitFor('avstämning', (s) => s.complete && s.state === 'idle')
const afterExternal = await list()
const m8After = afterExternal.data.messages.find((m) => m.uid === m8.uid)
log(afterExternal.data.cached && find(afterExternal.data.messages, 'Nytt under tiden') && find(afterExternal.data.messages, 'Skickat via appen'), 'ny post hämtas vid synk')
log(afterExternal.data.messages.at(-1).subject === 'SV: Re: Aha, ett svar' && subjectsOf(await list('INBOX', '&sort=subject&order=asc'))[0] === 'SV: Re: Aha, ett svar',
    'datumet i rubriken styr datumordningen, och svarsprefix räknas inte i ämnesordningen', afterExternal.data.messages.at(-1).subject)
log(!afterExternal.data.messages.some((m) => m.uid === m9.uid), 'meddelanden som tagits bort i en annan klient försvinner')
log(m8After.seen && m8After.flagged, 'flaggor som ändrats i en annan klient följer med', JSON.stringify(m8After))

// 6. Flytta mapp: cachade meddelanden följer med utan att hämtas om
const renamed = await api('POST', `/accounts/${id}/folders/rename`, { from: 'Projekt', to: 'Trash.Projekt' })
const renamedList = await list('Trash.Projekt')
log(renamed.status === 200 && renamedList.data.total === 1, 'en flyttad mapps meddelanden finns kvar under den nya sökvägen', JSON.stringify(renamedList.data).slice(0, 120))

// 7. Mejlservern går inte att nå
execSync(`docker network disconnect ${NETWORK} ${MAIL_CONTAINER}`)
try {
  api('POST', `/accounts/${id}/sync`)
  const offline = await waitFor('att kontot räknas som onåbart', (s) => s.state === 'offline', 120)
  log(offline.error.length > 0 && offline.lastSyncAt > 0, 'statusen visar att mejlservern inte svarar', offline.error)
  const offlineList = await list()
  log(offlineList.status === 200 && offlineList.data.cached && offlineList.ms < 1500, 'listan visas ur cachen utan väntan', `${offlineList.ms} ms`)
  const offlineFolders = await api('GET', `/accounts/${id}/folders`)
  log(offlineFolders.status === 200 && offlineFolders.data.length >= 4 && offlineFolders.ms < 1500, 'mapplistan visas ur cachen', `${offlineFolders.ms} ms`)
  const offlineRead = await api('GET', `/accounts/${id}/message?folder=INBOX&uid=${m8.uid}`)
  log(offlineRead.status === 200 && offlineRead.data.text.includes('nummer 8') && offlineRead.ms < 1500, 'meddelanden går att läsa', `${offlineRead.ms} ms`)
  const offlineImage = await fetch(`${APP}/api/accounts/${id}/attachment?folder=INBOX&uid=${newsletter.uid}&index=${inline.index}&download=0`, { headers: { Cookie: cookie } })
  log(offlineImage.status === 200, 'bilagor i nyligen öppnade meddelanden hämtas från disk')
  const offlineSearch = await list('INBOX', '&q=kantarellsoppa')
  log(offlineSearch.data.total === 1, 'sökning fungerar')
  const offlineSent = await list('Sent')
  log(offlineSent.status === 200 && offlineSent.data.cached, 'även mappar som väntade på avstämning visas')
  const offlineMove = await api('POST', `/accounts/${id}/messages/move`, { folder: 'INBOX', uids: [m8.uid], target: 'Trash' })
  log(offlineMove.status === 502 && (await list()).data.messages.some((m) => m.uid === m8.uid), 'ändringar kräver mejlservern och lämnar cachen orörd', JSON.stringify(offlineMove.data))
} finally {
  execSync(`docker network connect ${NETWORK} ${MAIL_CONTAINER}`)
}
await smtpDeliver(USER, plain('Efter avbrottet', 'Servern är tillbaka.'))
await sleep(500)
api('POST', `/accounts/${id}/sync`)
const back = await waitFor('att mejlservern svarar igen', (s) => s.state === 'idle' && s.complete, 120)
log(back.error === '' && find((await list()).data.messages, 'Efter avbrottet'), 'när mejlservern svarar igen hämtas det som kommit under tiden')

// 8. Behörighet och städning
await api('POST', '/users', { username: 'cacheannan', password: 'annanpass1' })
const otherCookie = cookie
cookie = ''
await api('POST', '/login', { username: 'cacheannan', password: 'annanpass1' })
const othersList = await list()
const othersStatus = await api('GET', '/cache')
const othersSync = await api('POST', `/accounts/${id}/sync`)
log(othersList.status === 404 && othersSync.status === 404 && othersStatus.data.accounts.length === 0, 'en annan användare kommer inte åt cachen')
cookie = otherCookie
await api('DELETE', `/accounts/${id}`)
log(!(await api('GET', '/cache')).data.accounts.some((a) => a.accountId === id), 'cachen tas bort med kontot')

console.log(failures ? `\n${failures} test misslyckades` : '\nAlla test gick igenom')
process.exit(failures ? 1 : 0)
