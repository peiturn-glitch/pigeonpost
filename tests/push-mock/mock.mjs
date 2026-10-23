// Låtsas-Firebase Cloud Messaging för test av push-notiserna: /token byter en RS256-signerad JWT mot en access-token
// och /v1/projects/:projekt/messages:send tar emot utskicken. Tokenens början styr svaret: "bad-" ger 404 UNREGISTERED
// (avinstallerad app), "flaky-" ger 503 första gången. /control/requests visar vad appen har skickat.
import http from 'node:http'
import crypto from 'node:crypto'
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const PORT = Number(process.env.PORT || 9997)
const ACCESS_TTL = Number(process.env.ACCESS_TTL || 3600)
const account = JSON.parse(fs.readFileSync(path.join(path.dirname(fileURLToPath(import.meta.url)), 'service-account.json'), 'utf8'))
const publicKey = crypto.createPublicKey(account.private_key)

const requests = [] // Utskick: { token, data, android, status }
const tokens = new Map() // access-token -> utgång (ms)
const stats = { tokenRequests: 0, badJwt: 0, unauthenticated: 0 }
const flakyDone = new Set()

function json(res, status, body) {
  res.writeHead(status, { 'Content-Type': 'application/json' })
  res.end(JSON.stringify(body))
}
function readBody(req) {
  return new Promise((resolve) => {
    let data = ''
    req.on('data', (c) => (data += c))
    req.on('end', () => resolve(data))
  })
}
const b64url = (s) => Buffer.from(s, 'base64url')

// Kontrollerar JWT:n som Googles tokenendpoint skulle ha gjort: signatur, utgivare, målgrupp, omfattning och tid.
function verifyJwt(jwt) {
  const parts = jwt.split('.')
  if (parts.length !== 3) return 'format'
  const header = JSON.parse(b64url(parts[0]).toString())
  const claims = JSON.parse(b64url(parts[1]).toString())
  if (header.alg !== 'RS256' || header.typ !== 'JWT') return 'header'
  if (!crypto.verify('RSA-SHA256', Buffer.from(parts[0] + '.' + parts[1]), publicKey, b64url(parts[2]))) return 'signatur'
  if (claims.iss !== account.client_email) return 'iss'
  if (claims.aud !== account.token_uri) return 'aud'
  if (claims.scope !== 'https://www.googleapis.com/auth/firebase.messaging') return 'scope'
  const now = Math.floor(Date.now() / 1000)
  if (!(claims.iat <= now + 60 && claims.exp > now && claims.exp - claims.iat <= 3600)) return 'tid'
  return null
}

http
  .createServer(async (req, res) => {
    const url = new URL(req.url, 'http://mock')

    if (req.method === 'POST' && url.pathname === '/token') {
      stats.tokenRequests++
      const body = new URLSearchParams(await readBody(req))
      if (body.get('grant_type') !== 'urn:ietf:params:oauth:grant-type:jwt-bearer') return json(res, 400, { error: 'unsupported_grant_type' })
      const problem = verifyJwt(body.get('assertion') || '')
      if (problem) {
        stats.badJwt++
        return json(res, 400, { error: 'invalid_grant', error_description: 'JWT: ' + problem })
      }
      const token = 'fcm-' + crypto.randomBytes(12).toString('hex')
      tokens.set(token, Date.now() + ACCESS_TTL * 1000)
      return json(res, 200, { access_token: token, expires_in: ACCESS_TTL, token_type: 'Bearer' })
    }

    const send = url.pathname.match(/^\/v1\/projects\/([^/]+)\/messages:send$/)
    if (req.method === 'POST' && send) {
      const auth = req.headers.authorization || ''
      const token = auth.startsWith('Bearer ') ? auth.slice(7) : ''
      const expires = tokens.get(token)
      if (!expires || expires < Date.now()) {
        stats.unauthenticated++
        return json(res, 401, { error: { code: 401, status: 'UNAUTHENTICATED', message: 'Request had invalid authentication credentials.' } })
      }
      if (send[1] !== account.project_id) return json(res, 403, { error: { code: 403, status: 'PERMISSION_DENIED' } })
      let message
      try { message = JSON.parse(await readBody(req)).message } catch { message = null }
      if (!message || !message.token || !message.data) return json(res, 400, { error: { code: 400, status: 'INVALID_ARGUMENT', message: 'message saknas' } })
      // Riktiga FCM avvisar reserverade nycklar i data (sågs i produktion 2026-10-23 med "from").
      const reserved = Object.keys(message.data).find((k) => ['from', 'notification', 'message_type'].includes(k) || /^(google|gcm)/.test(k))
      if (reserved) return json(res, 400, { error: { code: 400, status: 'INVALID_ARGUMENT', message: `Invalid data payload key: ${reserved}` } })
      const entry = { token: message.token, data: message.data, android: message.android || {}, at: Date.now() }
      if (message.token.startsWith('bad-')) {
        entry.status = 404
        requests.push(entry)
        return json(res, 404, { error: { code: 404, status: 'NOT_FOUND', message: 'Requested entity was not found.', details: [{ '@type': 'type.googleapis.com/google.firebase.fcm.v1.FcmError', errorCode: 'UNREGISTERED' }] } })
      }
      if (message.token.startsWith('flaky-') && !flakyDone.has(message.token)) {
        flakyDone.add(message.token)
        entry.status = 503
        requests.push(entry)
        return json(res, 503, { error: { code: 503, status: 'UNAVAILABLE' } })
      }
      entry.status = 200
      requests.push(entry)
      return json(res, 200, { name: `projects/${send[1]}/messages/${requests.length}` })
    }

    if (req.method === 'GET' && url.pathname === '/control/requests') return json(res, 200, requests)
    if (req.method === 'GET' && url.pathname === '/control/stats') return json(res, 200, stats)
    if (req.method === 'POST' && url.pathname === '/control/reset') {
      requests.length = 0
      flakyDone.clear()
      return json(res, 200, { ok: true })
    }
    // Alla access-tokens blir ogiltiga, som när Google återkallar dem: nästa utskick ska ge 401 och appen hämta en ny.
    if (req.method === 'POST' && url.pathname === '/control/expire-tokens') {
      tokens.clear()
      return json(res, 200, { ok: true })
    }
    json(res, 404, { error: 'okänd sökväg' })
  })
  .listen(PORT, () => console.log(`FCM-mock lyssnar på ${PORT}`))
