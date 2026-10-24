// End-to-end-test av reglerna för inkommande post (/api/rules): skapa, ändra ordning, körning på ny post i
// inkorgen, ångra, provkörning mot cachen, körning på en hel mapp, förslag ur mapparnas innehåll (med och utan
// låtsas-AI:n), namnbyte på mappar och att AI-sorteringen följer reglerna. Körs i testmiljön från
// tests/ai-mock/README.md; appcontainern ska ha RULE_CHECK_SECONDS=2 så att reglerna letar efter ny post varannan
// sekund.
import net from 'node:net'

const APP = process.env.APP || 'http://localhost:8082'
const SMTP_PORT = Number(process.env.SMTP_PORT || 13025)
const MOCK_URL = 'http://brevduva-aimock:9998/v1'
const USER = 'regler@pigeonpost.cc'

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

function smtpDeliver(to, from, subject, body = 'Hello!') {
  const raw = `From: ${from}\nTo: ${to}\nSubject: ${subject}\nMessage-ID: <${Math.random().toString(36).slice(2)}@example.com>\nDate: ${new Date().toUTCString()}\nContent-Type: text/plain; charset=utf-8\n\n${body}\n`
  const sender = from.match(/<([^>]+)>/)?.[1] || from
  return new Promise((resolve, reject) => {
    const socket = net.connect(SMTP_PORT, 'localhost')
    const steps = ['HELO test', `MAIL FROM:<${sender}>`, `RCPT TO:<${to}>`, 'DATA', raw.replace(/\r?\n/g, '\r\n') + '\r\n.', 'QUIT']
    let step = 0
    socket.on('data', () => {
      if (step >= steps.length) return socket.end()
      socket.write(steps[step++] + '\r\n')
    })
    socket.on('end', resolve)
    socket.on('error', reject)
  })
}

async function waitFor(check, timeoutMs = 30000, every = 500) {
  const start = Date.now()
  while (Date.now() - start < timeoutMs) {
    const value = await check()
    if (value) return value
    await sleep(every)
  }
  return null
}

await api('POST', '/login', { username: 'tester', password: 'testerpass1' })

// 1. Konto och mappar
const account = await api('POST', '/accounts', {
  name: 'Regler', email: USER, username: USER, password: 'test',
  imapHost: 'brevduva-aitest-mail', imapPort: 3143, imapSecurity: 'none',
  smtpHost: 'brevduva-aitest-mail', smtpPort: 3025, smtpSecurity: 'none', tlsVerify: false, skipTest: true,
})
const id = account.data.id
log(account.status === 201, 'testkonto skapat')
for (const path of ['Newsletters', 'Invoices', 'Shop', 'Bank', 'Trash']) await api('POST', `/accounts/${id}/folders`, { path })

let r = await api('GET', '/rules')
const aiAlready = r.data.aiConfigured === true  // Andra sviter kan ha lämnat en AI-tjänst inställd.
log(r.status === 200 && !r.data.rules.some((x) => x.accountId === id) && r.data.cache === true && r.data.accounts.some((a) => a.accountId === id && a.enabledRules === 0),
    'GET /rules: inga regler för kontot, cachen är på', JSON.stringify(r.data).slice(0, 300))
r = await api('GET', '/rules', undefined, '')
log(r.status === 401, 'reglerna kräver inloggning')

// 2. Kontroll av regler
const bad = [
  [{ name: 'x', conditions: [], actions: { move: 'Newsletters' } }, 'utan villkor'],
  [{ name: 'x', conditions: [{ field: 'subject', op: 'regex', value: '.*' }], actions: { move: 'Newsletters' } }, 'okänd operator'],
  [{ name: 'x', conditions: [{ field: 'subject', op: 'domain', value: '@a.se' }], actions: { move: 'Newsletters' } }, 'domän på ämne'],
  [{ name: 'x', conditions: [{ field: 'from', op: 'is', value: '' }], actions: { move: 'Newsletters' } }, 'tomt värde'],
  [{ name: 'x', conditions: [{ field: 'from', op: 'is', value: 'a@b.se' }], actions: { move: 'Finns inte' } }, 'mapp som inte finns'],
  [{ name: 'x', conditions: [{ field: 'from', op: 'is', value: 'a@b.se' }], actions: {} }, 'utan åtgärd'],
  [{ name: 'x', conditions: [{ field: 'from', op: 'is', value: 'a@b.se' }], actions: { move: 'Newsletters', trash: true } }, 'två mål'],
]
for (const [body, what] of bad) {
  r = await api('POST', `/accounts/${id}/rules`, body)
  log(r.status === 400, `regel avvisas: ${what}`, JSON.stringify(r.data))
}

// 3. Regler och ordning
r = await api('POST', `/accounts/${id}/rules`, {
  name: 'Nyhetsbrev', conditions: [{ field: 'from', op: 'domain', value: 'News.Example' }], actions: { move: 'Newsletters', read: true, stop: true },
})
log(r.status === 200 && r.data.savedId > 0, 'regel 1 sparad (domän → Newsletters, läst)')
const r1 = r.data.savedId
r = await api('POST', `/accounts/${id}/rules`, {
  name: 'Fakturor', matchAll: true, conditions: [{ field: 'subject', op: 'contains', value: 'Invoice' }], actions: { move: 'Invoices', star: true },
})
const r2 = r.data.savedId
r = await api('POST', `/accounts/${id}/rules`, {
  name: 'Chefen', conditions: [{ field: 'from', op: 'is', value: 'Boss@Example.com' }], actions: { stop: true },
})
const r3 = r.data.savedId
log(r.status === 200 && r.data.rules.filter((x) => x.accountId === id).length === 3, 'tre regler finns för kontot')
const byId = (rules, ruleId) => rules.find((x) => x.id === ruleId)
log(byId(r.data.rules, r1).conditions[0].value === '@news.example' && byId(r.data.rules, r3).conditions[0].value === 'boss@example.com',
    'domän och adress normaliseras till gemener med @', JSON.stringify(r.data.rules.map((x) => x.conditions)))
log(byId(r.data.rules, r1).position < byId(r.data.rules, r2).position && byId(r.data.rules, r2).position < byId(r.data.rules, r3).position,
    'nya regler hamnar sist')
r = await api('PUT', `/accounts/${id}/rules/order`, { ids: [r2, r1] })
log(r.status === 200 && byId(r.data.rules, r2).position === 1 && byId(r.data.rules, r1).position === 2 && byId(r.data.rules, r3).position === 3,
    'ny ordning: fakturor, nyhetsbrev, chefen', JSON.stringify(r.data.rules.map((x) => [x.name, x.position])))
r = await api('PUT', `/rules/${r3}`, { name: 'Chefen', enabled: true, conditions: [{ field: 'from', op: 'is', value: 'boss@example.com' }], actions: { stop: true, star: true } })
log(r.status === 200 && byId(r.data.rules, r3).actions.star === true && byId(r.data.rules, r3).position === 3, 'PUT /rules/:id ändrar regeln och behåller platsen')

// Reglerna sätter sitt märke i inkorgen innan posten kommer, så att bara ny post tas.
await sleep(3000)

// 4. Ny post i inkorgen
await smtpDeliver(USER, 'News <a@news.example>', 'Weekly digest')
await smtpDeliver(USER, 'Shop <billing@shop.example>', 'Invoice 42')
await smtpDeliver(USER, 'Boss <boss@example.com>', 'Hello')
await smtpDeliver(USER, 'Friend <friend@gmail.com>', 'Lunch?')
await sleep(500)
await api('POST', `/accounts/${id}/sync`)
const list = (folder) => api('GET', `/accounts/${id}/messages?folder=${encodeURIComponent(folder)}&pageSize=50`)
const subjects = async (folder) => ((await list(folder)).data.messages || []).map((m) => m.subject).sort()
const sorted = await waitFor(async () => {
  const [news, inv, inbox] = await Promise.all([subjects('Newsletters'), subjects('Invoices'), subjects('INBOX')])
  return news.includes('Weekly digest') && inv.includes('Invoice 42') && inbox.length === 2 ? { news, inv, inbox } : null
}, 60000, 1000)
log(!!sorted, 'reglerna flyttade nyhetsbrevet och fakturan; resten ligger kvar', JSON.stringify(sorted))
if (sorted) log(sorted.inbox.join(',') === 'Hello,Lunch?', 'inkorgen har chefens mejl och lunchen', sorted.inbox.join(','))
const weekly = ((await list('Newsletters')).data.messages || []).find((m) => m.subject === 'Weekly digest')
const invoice = ((await list('Invoices')).data.messages || []).find((m) => m.subject === 'Invoice 42')
const hello = ((await list('INBOX')).data.messages || []).find((m) => m.subject === 'Hello')
log(weekly && weekly.seen === true, 'nyhetsbrevet märktes som läst')
log(invoice && invoice.flagged === true, 'fakturan fick en stjärna')
log(hello && hello.flagged === true, 'chefens mejl fick en stjärna och ligger kvar (stopp utan flytt)')

// 5. Historik och ångra
r = await api('GET', '/rules')
const logEntries = r.data.log.filter((e) => e.accountId === id)
log(logEntries.length === 2 && logEntries.every((e) => e.ruleId > 0 && e.reason.startsWith('Rule: ')), 'två flyttar i historiken, med regelns namn', JSON.stringify(logEntries))
const weeklyEntry = logEntries.find((e) => e.subject === 'Weekly digest')
r = await api('POST', `/ai/log/${weeklyEntry.id}/undo`)
log(r.status === 200, 'flytten av nyhetsbrevet ångrades')
const back = await waitFor(async () => ((await subjects('INBOX')).includes('Weekly digest') ? true : null), 20000, 500)
log(!!back, 'nyhetsbrevet är tillbaka i inkorgen')
await sleep(5000)
log((await subjects('INBOX')).includes('Weekly digest'), 'ett ångrat meddelande sorteras inte om av reglerna')
r = await api('GET', '/rules')
log(r.data.log.find((e) => e.id === weeklyEntry.id).undoneAt > 0, 'historiken visar att flytten är ångrad')
r = await api('GET', '/ai/learning')
log(r.status === 200 && !r.data.corrections.some((c) => c.subject === 'Weekly digest'), 'en ångrad regelflytt blir ingen rättning för AI:n')

// 6. Provkörning mot cachen
r = await api('POST', `/accounts/${id}/rules/test`, { rule: { conditions: [{ field: 'subject', op: 'contains', value: 'LUNCH' }], actions: { star: true } }, folder: 'INBOX' })
log(r.status === 200 && r.data.matches === 1 && r.data.total === 3 && r.data.sample[0].subject === 'Lunch?', 'provkörning: 1 av 3 i inkorgen träffas', JSON.stringify(r.data))
r = await api('POST', `/accounts/${id}/rules/test`, { rule: { matchAll: false, conditions: [{ field: 'from', op: 'contains', value: 'friend' }, { field: 'from', op: 'is', value: 'boss@example.com' }], actions: { star: true } }, folder: 'INBOX' })
log(r.status === 200 && r.data.matches === 2, 'provkörning: "något av villkoren" träffar två', JSON.stringify(r.data))
r = await api('POST', `/accounts/${id}/rules/test`, { rule: { conditions: [{ field: 'subject', op: 'contains', value: 'x' }], actions: { star: true } }, folder: 'Finns inte' })
log(r.status === 404, 'provkörning mot en okänd mapp ger 404')

// 7. Kör reglerna på hela inkorgen
r = await api('POST', `/accounts/${id}/rules`, { name: 'Lunch', conditions: [{ field: 'subject', op: 'starts', value: 'lunch' }], actions: { move: 'Newsletters' } })
const r4 = r.data.savedId
r = await api('POST', `/accounts/${id}/rules/run`, { folder: 'INBOX' })
log(r.status === 200 && r.data.jobs.some((j) => j.accountId === id && j.folder === 'INBOX'), 'jobbet köades')
const job = await waitFor(async () => {
  const s = await api('GET', '/rules')
  const j = s.data.jobs.find((x) => x.accountId === id && x.folder === 'INBOX' && (x.state === 'done' || x.state === 'failed'))
  return j || null
}, 60000, 1000)
log(job && job.state === 'done' && job.total === 3 && job.moved === 2, 'jobbet gick igenom 3 meddelanden och flyttade 2 (lunchen och det ångrade nyhetsbrevet)', JSON.stringify(job))
log((await subjects('INBOX')).join(',') === 'Hello', 'bara chefens mejl ligger kvar i inkorgen', (await subjects('INBOX')).join(','))
r = await api('DELETE', `/rules/${r4}`)
log(r.status === 200 && !byId(r.data.rules, r4), 'regeln togs bort')

// 8. Förslag ur mapparnas innehåll
for (let i = 1; i <= 3; i++) await smtpDeliver(USER, 'Shop <orders@shop.example>', `Order ${i}`)
for (let i = 1; i <= 3; i++) await smtpDeliver(USER, 'Bank <alerts@bank.example>', `Alert ${i}`)
for (let i = 1; i <= 3; i++) await smtpDeliver(USER, 'Bank <info@bank.example>', `Info ${i}`)
await smtpDeliver(USER, 'Friend <friend@gmail.com>', 'Dinner?')
await smtpDeliver(USER, 'News <a@news.example>', 'Weekly digest 2')  // flyttas av regel 1 till Newsletters
await sleep(500)
await api('POST', `/accounts/${id}/sync`)
const arrived = await waitFor(async () => ((await list('INBOX')).data.messages.length === 11 ? true : null), 60000, 1000)
log(!!arrived, 'tio nya mejl i inkorgen (inga regler träffar dem); det nya nyhetsbrevet flyttades av regel 1')
const inboxNow = (await list('INBOX')).data.messages
const uidsWhere = (f) => inboxNow.filter(f).map((m) => m.uid)
await api('POST', `/accounts/${id}/messages/move`, { folder: 'INBOX', uids: uidsWhere((m) => m.subject.startsWith('Order')), target: 'Shop' })
await api('POST', `/accounts/${id}/messages/move`, { folder: 'INBOX', uids: uidsWhere((m) => m.subject.startsWith('Alert') || m.subject.startsWith('Info')), target: 'Bank' })
const moved = await waitFor(async () => ((await list('Shop')).data.total === 3 && (await list('Bank')).data.total === 6 ? true : null), 30000, 500)
log(!!moved, 'mejlen sorterades för hand till Shop och Bank')

// Flyttarna syns i cachen först när synken har stämt av målmapparna.
const suggested = await waitFor(async () => {
  const s = await api('POST', `/accounts/${id}/rules/suggest`, { useAi: false })
  const b = s.data.suggestions?.find((x) => x.key === 'domain:@bank.example')
  return b && b.inFolder === 6 && s.data.suggestions.some((x) => x.key === 'sender:orders@shop.example' && x.inFolder === 3) ? s : null
}, 60000, 1000)
r = suggested || (await api('POST', `/accounts/${id}/rules/suggest`, { useAi: false }))
log(r.status === 200 && r.data.aiUsed === false, 'förslag utan AI', JSON.stringify(r.data).slice(0, 300))
const shop = r.data.suggestions.find((s) => s.key === 'sender:orders@shop.example')
log(shop && shop.folder === 'Shop' && shop.inFolder === 3 && shop.elsewhere === 0 && shop.source === 'history' && shop.conditions[0].op === 'is',
    'förslag: orders@shop.example → Shop (3 av 3)', JSON.stringify(shop))
const bank = r.data.suggestions.find((s) => s.key === 'domain:@bank.example')
log(bank && bank.folder === 'Bank' && bank.inFolder === 6 && bank.conditions[0].op === 'domain', 'två avsändare från samma domän ger en domänregel', JSON.stringify(bank))
log(!r.data.suggestions.some((s) => s.key === 'sender:alerts@bank.example'), 'avsändarna i domänregeln föreslås inte var för sig')
log(!r.data.suggestions.some((s) => s.key.includes('gmail.com') || s.key.includes('friend')), 'en avsändare i inkorgen ger inget förslag')
r = await api('POST', `/accounts/${id}/rules/dismiss`, { sender: 'orders@shop.example' })
log(r.status === 200, 'nej tack till ett förslag')
r = await api('POST', `/accounts/${id}/rules/suggest`, { useAi: false })
log(!r.data.suggestions.some((s) => s.key === 'sender:orders@shop.example'), 'ett avböjt förslag kommer inte igen')
r = await api('POST', `/accounts/${id}/rules`, { name: bank.name, matchAll: bank.matchAll, conditions: bank.conditions, actions: bank.actions })
log(r.status === 200, 'domänförslaget blev en regel')
r = await api('POST', `/accounts/${id}/rules/suggest`, { useAi: false })
log(!r.data.suggestions.some((s) => s.key === 'domain:@bank.example'), 'en avsändare som redan har en regel föreslås inte')
if (!aiAlready) {
  r = await api('POST', `/accounts/${id}/rules/suggest`, { useAi: true })
  log(r.status === 400, 'förslag med AI kräver en AI-tjänst', JSON.stringify(r.data))
}

// 9. Förslag med låtsas-AI:n: bara giltiga regler med träffar släpps igenom
r = await api('PUT', '/ai', { baseUrl: MOCK_URL, model: 'mock-sorter', apiKey: 'test-key' })
log(r.status === 200, 'AI-tjänsten ställdes in', JSON.stringify(r.data).slice(0, 200))
r = await api('POST', `/accounts/${id}/rules/suggest`, { useAi: true })
const fromAi = (r.data.suggestions || []).filter((s) => s.source === 'ai')
log(r.status === 200 && r.data.aiUsed === true && fromAi.length === 1 && fromAi[0].name === 'Veckobrev' && fromAi[0].folder === 'Newsletters' && fromAi[0].inFolder === 2 && fromAi[0].conditions[0].value === 'weekly',
    'AI-förslag: ett giltigt förslag med träff; påhittad mapp, okänd operator, tomma villkor och utan träff sållas bort', JSON.stringify(fromAi))
const mockRequests = await (await fetch('http://localhost:9998/control/requests')).json()
const last = JSON.parse(mockRequests.at(-1).messages.find((m) => m.role === 'user').content)
log(last.folders && last.inboxSample && !JSON.stringify(last).includes('Hello!'), 'AI:n fick mappar, avsändare och ämnesrader men inget innehåll', Object.keys(last).join(','))

// 10. AI-sorteringen följer reglerna
const newsUid = ((await list('Newsletters')).data.messages || []).find((m) => m.subject === 'Weekly digest')?.uid
await api('POST', `/accounts/${id}/messages/move`, { folder: 'Newsletters', uids: [newsUid], target: 'INBOX' })
await sleep(1000)
const inboxList = (await list('INBOX')).data.messages
const weeklyUid = inboxList.find((m) => m.subject === 'Weekly digest')?.uid
const dinnerUid = inboxList.find((m) => m.subject === 'Dinner?')?.uid
r = await api('POST', `/accounts/${id}/ai-sort`, { folder: 'INBOX', uids: [weeklyUid, dinnerUid] })
const weeklySuggestion = r.data.suggestions?.find((s) => s.uid === weeklyUid)
log(r.status === 200 && weeklySuggestion && weeklySuggestion.target === 'Newsletters' && weeklySuggestion.reason.includes('Your rule'),
    'AI-sorteringen använder regeln i stället för att fråga AI:n', JSON.stringify(r.data).slice(0, 300))

// 11. Namnbyte på en mapp följer med i reglerna
r = await api('POST', `/accounts/${id}/folders/rename`, { from: 'Newsletters', to: 'News' })
log(r.status === 200, 'mappen bytte namn')
r = await api('GET', '/rules')
log(byId(r.data.rules, r1).actions.move === 'News', 'regeln pekar på den nya sökvägen', JSON.stringify(byId(r.data.rules, r1).actions))

// 12. Behörighet
await api('POST', '/users', { username: 'regelgranne', password: 'grannepass1' })
const other = await fetch(APP + '/api/login', { method: 'POST', headers: { 'X-Brevduva': '1', 'Content-Type': 'application/json' }, body: JSON.stringify({ username: 'regelgranne', password: 'grannepass1' }) })
const otherCookie = other.headers.get('set-cookie').split(';')[0]
r = await api('PUT', `/rules/${r1}`, { name: 'x', conditions: [{ field: 'from', op: 'is', value: 'a@b.se' }], actions: { stop: true } }, otherCookie)
log(r.status === 404, 'en annan användare kan inte ändra regeln')
r = await api('DELETE', `/rules/${r1}`, undefined, otherCookie)
log(r.status === 404, 'en annan användare kan inte ta bort regeln')
r = await api('GET', '/rules', undefined, otherCookie)
log(r.status === 200 && r.data.rules.length === 0, 'en annan användare ser inga regler')
r = await api('POST', `/accounts/${id}/rules`, { name: 'x', conditions: [{ field: 'from', op: 'is', value: 'a@b.se' }], actions: { stop: true } }, otherCookie)
log(r.status === 404 || r.status === 403, 'en annan användare kan inte lägga regler på kontot')

// Städa
await api('DELETE', '/ai')
await api('DELETE', `/accounts/${id}`)
console.log(failures ? `\n${failures} fel` : '\nAlla regeltester gick igenom')
process.exit(failures ? 1 : 0)
