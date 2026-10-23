// End-to-end-test av Google-inloggning mot låtsas-Google och GreenMail.
import { execSync } from 'node:child_process'

const APP = 'http://localhost:8081'
const MOCK = 'http://localhost:9999'
let cookie = ''
let failures = 0
const log = (ok, text, extra = '') => {
  if (!ok) failures++
  console.log(`${ok ? 'OK  ' : 'FEL '} ${text}${extra ? ' — ' + extra : ''}`)
}

async function api(method, path, body) {
  const res = await fetch(APP + '/api' + path, {
    method,
    headers: { 'X-Brevduva': '1', 'Content-Type': 'application/json', Cookie: cookie },
    body: body === undefined ? undefined : JSON.stringify(body),
    redirect: 'manual',
  })
  const setCookie = res.headers.get('set-cookie')
  if (setCookie) cookie = setCookie.split(';')[0]
  const text = await res.text()
  let data = text
  try { data = JSON.parse(text) } catch {}
  return { status: res.status, data }
}
const mockStats = async () => (await fetch(MOCK + '/control/stats')).json()
const sleep = (ms) => new Promise((r) => setTimeout(r, ms))

// Kör webbläsarens del: följ Googles omdirigering till callback-adressen.
async function browserLogin(url) {
  const auth = await fetch(url, { redirect: 'manual' })
  const location = auth.headers.get('location')
  if (!location) return { error: `auth svarade ${auth.status}: ${await auth.text()}` }
  const callback = await fetch(location, { redirect: 'manual' })
  return { location, html: await callback.text(), status: callback.status }
}

async function googleLogin(loginHint) {
  const start = await api('POST', '/oauth/google/start', { loginHint })
  const browser = await browserLogin(start.data.url)
  const status = await api('GET', '/oauth/google/status?state=' + start.data.state)
  return { start, browser, status }
}

await api('POST', '/login', { username: 'tester', password: 'testerpass1' })

const providers = await api('GET', '/oauth/providers')
log(providers.data.google?.configured === true, 'Google-inloggning rapporteras som aktiverad')
log(providers.data.google?.redirectUri === 'http://localhost:8081/api/oauth/google/callback', 'callback-adress från PUBLIC_URL', providers.data.google?.redirectUri)

// 1. Inloggning och kontoskapande
const first = await googleLogin('oauthtest@gmail.com')
const authUrl = new URL(first.start.data.url)
log(authUrl.searchParams.get('code_challenge_method') === 'S256' && authUrl.searchParams.get('prompt') === 'consent', 'auth-URL har PKCE S256 och prompt=consent')
log(first.browser.html?.includes("BroadcastChannel('brevduva-oauth')") && first.browser.html.includes(first.start.data.state), 'callback-sidan meddelar appen', first.browser.error)
log(first.status.data.ok === true && first.status.data.email === 'oauthtest@gmail.com', 'status: inloggad', JSON.stringify(first.status.data))

const noCookieStatus = await fetch(APP + '/api/oauth/google/status?state=' + first.start.data.state)
log(noCookieStatus.status === 401, 'status kräver sessionen', String(noCookieStatus.status))
const replay = await fetch(first.browser.location, { redirect: 'manual' })
log((await replay.text()).includes('failed'), 'samma callback kan inte användas igen')

const accountBody = {
  authType: 'oauth-google', oauthState: first.start.data.state, email: '', name: '',
  imapHost: 'brevduva-oauthmock', imapPort: 1143, imapSecurity: 'none',
  smtpHost: 'mailserver', smtpPort: 3025, smtpSecurity: 'none', tlsVerify: false,
}
const created = await api('POST', '/accounts', accountBody)
log(created.status === 201 && created.data.authType === 'oauth-google', 'konto skapat med Google-inloggning (anslutningstest via XOAUTH2)', JSON.stringify(created.data).slice(0, 300))
log(created.data.username === 'oauthtest@gmail.com' && created.data.name === 'OAuth Testare', 'användarnamn och namn från Google')
const accountId = created.data.id
const consumed = await api('GET', '/oauth/google/status?state=' + first.start.data.state)
log(consumed.status === 404, 'inloggningens state förbrukas när kontot sparas')
const reuse = await api('POST', '/accounts', accountBody)
log(reuse.status === 400, 'samma state kan inte skapa ett konto till', JSON.stringify(reuse.data))

// 2. Läsa och skicka via proxy/GreenMail
const folders = await api('GET', `/accounts/${accountId}/folders`)
log(folders.status === 200 && folders.data.some((f) => f.path === 'INBOX'), 'mapplistning via IMAP XOAUTH2 (utan SASL-IR)', JSON.stringify(folders.data).slice(0, 200))
const form = new FormData()
form.append('message', new Blob([JSON.stringify({ to: 'oauthtest@gmail.com', subject: 'OAuth-test', text: 'Hej via XOAUTH2' })], { type: 'application/json' }), 'm.json')
const sent = await fetch(`${APP}/api/accounts/${accountId}/send`, { method: 'POST', headers: { 'X-Brevduva': '1', Cookie: cookie }, body: form })
log(sent.status === 200, 'skicka via SMTP XOAUTH2', await sent.text())
const smtpLog = execSync('docker logs brevduva-mailserver-1 2>&1').toString()
const authLines = smtpLog.split('\n').filter((l) => l.includes('C: AUTH XOAUTH2'))
const lastAuth = authLines.at(-1) || ''
const b64 = (lastAuth.match(/AUTH XOAUTH2 ([A-Za-z0-9+/=]+)/) || [])[1]
const decoded = b64 ? Buffer.from(b64, 'base64').toString('latin1') : ''
log(/^user=oauthtest@gmail\.com\x01auth=Bearer ya29\.[\w-]+\x01\x01$/.test(decoded), 'SMTP skickade XOAUTH2 med access-token', decoded.replace(/\x01/g, '^A').slice(0, 60))
await sleep(1500)
const inbox = await api('GET', `/accounts/${accountId}/messages?folder=INBOX`)
log(inbox.data.messages?.some((m) => m.subject === 'OAuth-test'), 'det skickade mejlet syns i inkorgen')

// 3. Förnyelse av access-token (ACCESS_TTL=66 s, marginal 60 s => token räknas som gammal efter ~6 s)
const before = await mockStats()
await sleep(7000)
const sent2 = await fetch(`${APP}/api/accounts/${accountId}/send`, { method: 'POST', headers: { 'X-Brevduva': '1', Cookie: cookie }, body: form })
const after = await mockStats()
log(sent2.status === 200 && after.refreshes > before.refreshes, 'utgången access-token förnyas med refresh-token', `refreshes ${before.refreshes} -> ${after.refreshes}`)

// 4. SASL-IR-varianten
const toSaslIr = await api('PUT', `/accounts/${accountId}`, { imapPort: 1144, skipTest: false })
const statsIr = await mockStats()
log(toSaslIr.status === 200 && statsIr.saslIr > 0, 'IMAP XOAUTH2 med SASL-IR', JSON.stringify(toSaslIr.data).slice(0, 200))

// 5. Servern avvisar token: klienten ska kvittera felet och få NO, inte hänga.
await fetch(MOCK + '/control/fail-next-imap-auth')
await api('PUT', `/accounts/${accountId}`, { imapPort: 1143, skipTest: true })
const t0 = Date.now()
const rejected = await api('GET', `/accounts/${accountId}/folders`)
log(rejected.status === 502 && /rejected/.test(rejected.data.error) && /ack=""/.test(rejected.data.error) && Date.now() - t0 < 10000,
  'avvisad token ger tydligt fel (tom kvittensrad skickades)', `${rejected.status} ${rejected.data.error} (${Date.now() - t0} ms)`)
const recovered = await api('GET', `/accounts/${accountId}/folders`)
log(recovered.status === 200, 'nästa försök lyckas igen')

// 6. Thunderbird-export markerar OAuth2
const zip = Buffer.from(await (await fetch(APP + '/api/export/thunderbird', { headers: { Cookie: cookie } })).arrayBuffer()).toString('latin1')
log(/mail\.server\.server1\.authMethod", 10\)/.test(zip) && /mail\.smtpserver\.smtp1\.authMethod", 10\)/.test(zip), 'export: authMethod 10 (OAuth2) för IMAP och SMTP')

// 7. Nekad inloggning
const denied = await googleLogin('deny@gmail.com')
log(denied.status.data.done && !denied.status.data.ok && /cancelled/.test(denied.status.data.error), 'nekad inloggning ger felmeddelande', JSON.stringify(denied.status.data))

// 8. Byte till lösenord återkallar behörigheten hos Google
const second = await googleLogin('andra@gmail.com')
const acc2 = await api('POST', '/accounts', { ...accountBody, oauthState: second.start.data.state, skipTest: true })
const revokesBefore = (await mockStats()).revokes
const toPassword = await api('PUT', `/accounts/${acc2.data.id}`, { authType: 'password', username: 'andra@gmail.com', password: 'app-losen', skipTest: true })
log(toPassword.status === 200 && toPassword.data.authType === 'password' && (await mockStats()).revokes === revokesBefore + 1, 'byte till lösenord återkallar Google-token')
const missingPassword = await api('PUT', `/accounts/${accountId}`, { authType: 'password', skipTest: true })
log(missingPassword.status === 400, 'byte till lösenord kräver lösenord', JSON.stringify(missingPassword.data))

// 9. Återkallad behörighet hos Google
await fetch(MOCK + '/control/revoke-all')
await api('PUT', `/accounts/${accountId}`, { skipTest: true })
const revoked = await api('GET', `/accounts/${accountId}/folders`)
log(revoked.status === 502 && /sign in with Google again/i.test(revoked.data.error), 'återkallad inloggning ber användaren logga in igen', revoked.data.error)

// 10. Ny inloggning på befintligt konto
const relogin = await googleLogin('oauthtest@gmail.com')
const updated = await api('PUT', `/accounts/${accountId}`, { authType: 'oauth-google', oauthState: relogin.start.data.state, skipTest: false })
const works = await api('GET', `/accounts/${accountId}/folders`)
log(updated.status === 200 && works.status === 200, 'logga in igen på befintligt konto', JSON.stringify(updated.data).slice(0, 160))

// 11. En annan användare kan inte använda state
const other = await api('POST', '/users', { username: 'annan', password: 'annanpass1' })
const third = await googleLogin('oauthtest@gmail.com')
const adminCookie = cookie
cookie = ''
await api('POST', '/login', { username: 'annan', password: 'annanpass1' })
const stolen = await api('POST', '/accounts', { ...accountBody, oauthState: third.start.data.state })
const stolenStatus = await api('GET', '/oauth/google/status?state=' + third.start.data.state)
log(other.status === 201 && stolen.status === 400 && stolenStatus.status === 404, 'annan användare kan inte använda någon annans inloggning', `${stolen.status}/${stolenStatus.status}`)
cookie = adminCookie

// 12. Borttagning återkallar
const revokesBeforeDelete = (await mockStats()).revokes
const deleted = await api('DELETE', `/accounts/${accountId}`)
log(deleted.status === 200 && (await mockStats()).revokes === revokesBeforeDelete + 1, 'borttaget konto återkallar Google-token')

console.log('\nMock-statistik:', JSON.stringify(await mockStats()))
console.log(failures ? `\n${failures} test misslyckades` : '\nAlla test gick igenom')
