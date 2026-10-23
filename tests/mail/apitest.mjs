// End-to-end-test av formaterade meddelanden och flytt av mappar mot GreenMail.
// Körs i samma testmiljö som tests/ai-mock (se README där), före apitest.mjs i den mappen.
const APP = process.env.APP || 'http://localhost:8082'
const MOCK_URL = 'http://brevduva-aimock:9998/v1'

let cookie = ''
let failures = 0
const log = (ok, text, extra = '') => {
  if (!ok) failures++
  console.log(`${ok ? 'OK  ' : 'FEL '} ${text}${extra ? ' — ' + extra : ''}`)
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms))

async function api(method, path, body, form) {
  const headers = { 'X-Brevduva': '1', Cookie: cookie }
  if (!form && body !== undefined) headers['Content-Type'] = 'application/json'
  const res = await fetch(APP + '/api' + path, { method, headers, body: form || (body === undefined ? undefined : JSON.stringify(body)) })
  const setCookie = res.headers.get('set-cookie')
  if (setCookie) cookie = setCookie.split(';')[0]
  const text = await res.text()
  let data = text
  try { data = JSON.parse(text) } catch {}
  return { status: res.status, data }
}

function send(accountId, message, files = []) {
  const form = new FormData()
  form.append('message', new Blob([JSON.stringify(message)], { type: 'application/json' }), 'message.json')
  for (const f of files) form.append('attachments', new Blob([f.data], { type: f.type }), f.name)
  return api('POST', `/accounts/${accountId}/send`, undefined, form)
}

await api('POST', '/login', { username: 'tester', password: 'testerpass1' })
const account = await api('POST', '/accounts', {
  name: 'Skriva', email: 'skriva@localhost', username: 'skriva@localhost', password: 'test',
  imapHost: 'brevduva-aitest-mail', imapPort: 3143, imapSecurity: 'none',
  smtpHost: 'brevduva-aitest-mail', smtpPort: 3025, smtpSecurity: 'none', tlsVerify: false, skipTest: true,
})
const id = account.data.id
log(account.status === 201, 'testkonto skapat')
await api('POST', `/accounts/${id}/folders`, { path: 'Sent' })

// 1. Formaterat meddelande med inbäddad bild och bilaga
const pixel = 'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg=='
const longLine = '<span style="color: rgb(197, 0, 66);">' + 'räksmörgås '.repeat(200) + '</span>'
const html = `<div><b>Hej!</b> Här är en <a href="https://exempel.se">länk</a> och en bild:</div><img src="data:image/png;base64,${pixel}" alt="prick" style="max-width: 100%;"><div>${longLine}</div>`
const sent = await send(id, { to: 'skriva@localhost', subject: 'Formaterat', text: 'Hej! Här är en länk <https://exempel.se> och en bild: [prick]', html },
  [{ name: 'rapport.txt', type: 'text/plain', data: 'bilagans innehåll' }])
log(sent.status === 200 && sent.data.warnings.length === 0, 'formaterat meddelande skickas', JSON.stringify(sent.data))
await sleep(1000)
await api('POST', `/accounts/${id}/sync`) // Post som kommit utifrån syns i mejlcachen efter en synk.

const inbox = await api('GET', `/accounts/${id}/messages?folder=INBOX`)
const uid = inbox.data.messages.find((m) => m.subject === 'Formaterat')?.uid
const message = await api('GET', `/accounts/${id}/message?folder=INBOX&uid=${uid}`)
log(message.data.html.includes('<b>Hej!</b>') && message.data.html.includes('räksmörgås'), 'HTML-delen kommer fram')
log(message.data.text.startsWith('Hej! Här är en länk'), 'textversionen följer med som alternativ', message.data.text)
const names = message.data.attachments.map((a) => `${a.filename}:${a.contentType}:${a.inline}`)
log(JSON.stringify(names) === '["bild1.png:image/png:true","rapport.txt:text/plain:false"]', 'bilden är inbäddad och bilagan en vanlig bilaga', JSON.stringify(names))
// Bilden följer inte med i HTML-delen; klienten hämtar den som bilaga och sätter in den där cid-referensen står.
const inlineImage = message.data.attachments.find((a) => a.inline)
log(message.data.html.includes(`cid:${inlineImage?.contentId}`) && !message.data.html.includes('data:image'), 'HTML-delen pekar på den inbäddade bilden med cid', inlineImage?.contentId)
const imageResponse = await fetch(`${APP}/api/accounts/${id}/attachment?folder=INBOX&uid=${uid}&index=${inlineImage.index}&download=0`, { headers: { Cookie: cookie } })
log(imageResponse.status === 200 && Buffer.from(await imageResponse.arrayBuffer()).toString('base64') === pixel, 'bilden hämtas oförändrad som bilaga')

const source = (await api('GET', `/accounts/${id}/source?folder=INBOX&uid=${uid}`)).data
const order = ['multipart/mixed', 'multipart/alternative', 'text/plain', 'multipart/related', 'text/html', 'image/png', 'rapport.txt'].map((s) => source.indexOf(s))
log(order.every((v, i) => v >= 0 && (i === 0 || v > order[i - 1])), 'MIME-strukturen är mixed > alternative > (text, related > (html, bild)) + bilaga', JSON.stringify(order))
// HTML-delen är base64-kodad här, så själva cid-referensen syns inte i källan. Att bilden ovan räknas som inbäddad
// visar att HTML-delen pekar på den.
log(!source.includes('data:image') && /Content-Id: <bild1\.[0-9a-f]+@localhost>/i.test(source), 'bilden har ett Content-ID och ingen data-URI finns kvar')
const longest = Math.max(...source.split(/\r?\n/).map((l) => l.length))
log(longest <= 998, 'inga rader över 998 tecken trots lång HTML-rad', String(longest))

const sentCopy = await api('GET', `/accounts/${id}/messages?folder=Sent`)
log(sentCopy.data.total === 1, 'kopian i Skickat sparas')

// 2. Ren text fungerar som förut
await send(id, { to: 'skriva@localhost', subject: 'Ren text', text: 'Bara text.' })
await sleep(1000)
await api('POST', `/accounts/${id}/sync`) // Post som kommit utifrån syns i mejlcachen efter en synk.
const plainUid = (await api('GET', `/accounts/${id}/messages?folder=INBOX`)).data.messages.find((m) => m.subject === 'Ren text').uid
const plainSource = (await api('GET', `/accounts/${id}/source?folder=INBOX&uid=${plainUid}`)).data
log(/Content-Type: text\/plain/i.test(plainSource) && !plainSource.includes('multipart'), 'ett meddelande utan HTML skickas som text/plain')

// 3. Utkast med formatering
const draft = await api('POST', `/accounts/${id}/drafts`, { to: 'nagon@exempel.se', subject: 'Utkast', text: 'Fet text', html: '<div><b>Fet text</b></div>' })
const draftMessage = await api('GET', `/accounts/${id}/message?folder=${encodeURIComponent(draft.data.folder)}&uid=${draft.data.uid}`)
log(draftMessage.data.html.includes('<b>Fet text</b>'), 'ett formaterat utkast behåller sin HTML', JSON.stringify(draft.data))

// 4. Tabeller och formaterade signaturer
const table = '<table style="border-collapse: collapse; width: 100%;"><tbody><tr><td style="border: 1px solid #999999;">Artikel</td><td style="border: 1px solid #999999;">Pris</td></tr></tbody></table>'
await send(id, { to: 'skriva@localhost', subject: 'Tabell', text: 'Artikel\tPris', html: table })
await sleep(1000)
await api('POST', `/accounts/${id}/sync`) // Post som kommit utifrån syns i mejlcachen efter en synk.
const tableUid = (await api('GET', `/accounts/${id}/messages?folder=INBOX`)).data.messages.find((m) => m.subject === 'Tabell').uid
const tableMessage = await api('GET', `/accounts/${id}/message?folder=INBOX&uid=${tableUid}`)
log(tableMessage.data.html.includes('<td style="border: 1px solid #999999;">Pris</td>'), 'en tabell kommer fram med sina inline-stilar')

const sigHtml = `<b>Anna Andersson</b><br><span style="color: rgb(0, 97, 224);">Exempel AB</span><img src="data:image/png;base64,${pixel}" alt="logga">`
const withSig = await api('PUT', `/accounts/${id}`, { signature: 'Anna Andersson\nExempel AB', signatureHtml: sigHtml, skipTest: true })
const listed = (await api('GET', '/accounts')).data.find((a) => a.id === id)
log(withSig.status === 200 && listed.signatureHtml === sigHtml && listed.signature === 'Anna Andersson\nExempel AB', 'kontots formaterade signatur sparas tillsammans med textversionen')
const identity = await api('POST', `/accounts/${id}/identities`, { name: 'Support', email: 'support@localhost', signature: 'Support', signatureHtml: '<i>Support</i>' })
log(identity.status === 201 && identity.data.signatureHtml === '<i>Support</i>', 'en avsändare kan ha en egen formaterad signatur', JSON.stringify(identity.data))
const hugeSig = await api('PUT', `/accounts/${id}`, { signatureHtml: 'x'.repeat(2 * 1024 * 1024 + 1), skipTest: true })
log(hugeSig.status === 400, 'en signatur över 2 MB avvisas', JSON.stringify(hugeSig.data).slice(0, 120))

const exported = await fetch(APP + '/api/export/thunderbird', { headers: { Cookie: cookie } })
const zip = Buffer.from(await exported.arrayBuffer()).toString('latin1')
log(zip.includes('htmlSigFormat", true') && zip.includes('Anna Andersson'), 'exporten till Thunderbird behåller den formaterade signaturen')
const prefs = [
  'user_pref("mail.accountmanager.accounts", "account1");',
  'user_pref("mail.account.account1.server", "server1");',
  'user_pref("mail.account.account1.identities", "id1");',
  'user_pref("mail.server.server1.type", "imap");',
  'user_pref("mail.server.server1.hostname", "imap.exempel.se");',
  'user_pref("mail.server.server1.userName", "bo@exempel.se");',
  'user_pref("mail.identity.id1.useremail", "bo@exempel.se");',
  'user_pref("mail.identity.id1.fullName", "Bo");',
  'user_pref("mail.identity.id1.htmlSigFormat", true);',
  'user_pref("mail.identity.id1.htmlSigText", "<b>Bo Berg</b><br>Exempel AB");',
].join('\n')
const preview = await api('POST', '/import/thunderbird/preview', { prefs })
const imported = preview.data.accounts?.[0]
log(imported?.signatureHtml === '<b>Bo Berg</b><br>Exempel AB' && imported?.signature.trim() === 'Bo Berg\nExempel AB',
    'importen från Thunderbird tar med HTML-signaturen och en textversion', JSON.stringify(imported?.signature))

// 5. Flytta mappar
const folders = async () => (await api('GET', `/accounts/${id}/folders`)).data
const d = (await folders()).find((f) => f.path === 'Sent').delimiter
for (const path of ['Projekt', `Projekt${d}Offerter`, 'Kunder']) await api('POST', `/accounts/${id}/folders`, { path })
await api('POST', `/accounts/${id}/messages/move`, { folder: 'INBOX', uids: [plainUid], target: `Projekt${d}Offerter` })

// En regel för automatisk sortering pekar på mappen som flyttas.
await api('PUT', '/ai', { baseUrl: MOCK_URL, model: 'mock-sorter', apiKey: 'test-key' })
await api('PUT', `/accounts/${id}/ai-auto`, { enabled: true, intervalMinutes: 60, targets: ['Projekt', `Projekt${d}Offerter`, 'Kunder'] })

const intoSelf = await api('POST', `/accounts/${id}/folders/rename`, { from: 'Projekt', to: `Projekt${d}Offerter${d}Projekt` })
log(intoSelf.status === 400, 'en mapp kan inte flyttas in i sig själv', JSON.stringify(intoSelf.data))
const moved = await api('POST', `/accounts/${id}/folders/rename`, { from: 'Projekt', to: `Kunder${d}Projekt` })
const paths = (await folders()).map((f) => f.path)
log(moved.status === 200 && paths.includes(`Kunder${d}Projekt`) && paths.includes(`Kunder${d}Projekt${d}Offerter`) && !paths.includes('Projekt'),
    'mappen flyttas med sina undermappar', JSON.stringify(paths))
const kept = await api('GET', `/accounts/${id}/messages?folder=${encodeURIComponent(`Kunder${d}Projekt${d}Offerter`)}`)
log(kept.data.total === 1 && kept.data.messages[0].subject === 'Ren text', 'meddelandena följer med')
const rule = (await api('GET', '/ai/auto')).data.rules.find((r) => r.accountId === id)
log(JSON.stringify(rule.targets) === JSON.stringify([`Kunder${d}Projekt`, `Kunder${d}Projekt${d}Offerter`, 'Kunder']),
    'regeln för automatisk sortering följer med till den nya sökvägen', JSON.stringify(rule.targets))
// GreenMail 2.1.3 svarar OK men gör ingenting när en undermapp (till skillnad från en mapp på översta nivån) byter
// sökväg, så flytt av undermappar går inte att testa här. Kommandot är detsamma (RENAME), och appen kontrollerar
// efteråt att mappen verkligen flyttades.

// Avsändare vars fjärrinnehåll alltid visas
const trusted = await api('POST', '/remote-senders', { sender: 'Nyheter@Butiken.SE' })
await api('POST', '/remote-senders', { sender: 'nyheter@butiken.se' })
log(trusted.status === 200 && JSON.stringify((await api('GET', '/remote-senders')).data.senders) === '["nyheter@butiken.se"]',
    'en betrodd avsändare sparas en gång, med små bokstäver', JSON.stringify(trusted.data))
const badSenders = [await api('POST', '/remote-senders', { sender: 'ingen adress' }), await api('POST', '/remote-senders', { sender: '@butiken.se' }),
                    await api('POST', '/remote-senders', {}), await api('POST', '/remote-senders', { sender: 'a@b.se, c@d.se' })]
log(badSenders.every((r) => r.status === 400), 'ogiltiga avsändare avvisas', badSenders.map((r) => r.status).join())
await api('POST', '/remote-senders', { sender: 'info@exempel.se' })
const untrusted = await api('POST', '/remote-senders/delete', { sender: 'NYHETER@butiken.se' })
log(JSON.stringify(untrusted.data.senders) === '["info@exempel.se"]', 'en avsändare går att sluta lita på', JSON.stringify(untrusted.data))

// Städa, så att testet i tests/ai-mock börjar utan AI-inställningar.
await api('DELETE', `/accounts/${id}`)
await api('DELETE', '/ai')

console.log(failures ? `\n${failures} test misslyckades` : '\nAlla test gick igenom')
process.exit(failures ? 1 : 0)
