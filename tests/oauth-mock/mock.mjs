// Låtsas-Google för test: OAuth 2.0 (auth, token, revoke), en IMAP-server med SASL XOAUTH2
// som efter godkänd token vidarebefordrar anslutningen till GreenMail, och ett litet Google Drive API
// (filer, mappar och återupptagbar uppladdning) för backup.
import http from 'node:http'
import net from 'node:net'
import crypto from 'node:crypto'

const CLIENT_ID = 'test-client'
const CLIENT_SECRET = 'test-secret'
const ACCESS_TTL = Number(process.env.ACCESS_TTL || 3600)
const UPSTREAM = { host: process.env.UPSTREAM_HOST || 'mailserver', port: Number(process.env.UPSTREAM_PORT || 3143) }

const codes = new Map() // code -> { challenge, redirectUri, email, used }
const accessTokens = new Map() // token -> { email, expires }
const refreshTokens = new Map() // token -> { email, revoked }
const stats = { authRequests: 0, codeExchanges: 0, refreshes: 0, revokes: 0, imapAuthOk: 0, imapAuthFailed: 0, saslIr: 0, pkceFailures: 0, driveChunks: 0, driveChunkFailures: 0, driveResumes: 0 }
let failNextImapAuth = false

const MAIL_SCOPE = 'https://mail.google.com/'
const DRIVE_SCOPE = 'https://www.googleapis.com/auth/drive.file'
const driveFiles = new Map() // id -> { id, name, mimeType, parents, appProperties, createdTime, trashed, size, sha256, owner, data }
const uploadSessions = new Map() // id -> { meta, total, received: Buffer[], length, owner }
let failDriveChunks = 0

const b64url = (buf) => Buffer.from(buf).toString('base64').replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '')
const random = () => b64url(crypto.randomBytes(24))

function issueAccess(email) {
  const token = 'ya29.' + random()
  accessTokens.set(token, { email, expires: Date.now() + ACCESS_TTL * 1000 })
  return token
}

function readBody(req) {
  return new Promise((resolve) => {
    let data = ''
    req.on('data', (c) => (data += c))
    req.on('end', () => resolve(new URLSearchParams(data)))
  })
}

function json(res, status, body) {
  res.writeHead(status, { 'Content-Type': 'application/json' })
  res.end(JSON.stringify(body))
}

http
  .createServer(async (req, res) => {
    const url = new URL(req.url, 'http://mock')
    const q = url.searchParams

    if (req.method === 'GET' && url.pathname === '/auth') {
      stats.authRequests++
      const problems = []
      if (q.get('client_id') !== CLIENT_ID) problems.push('client_id')
      if (q.get('response_type') !== 'code') problems.push('response_type')
      if (q.get('code_challenge_method') !== 'S256' || !q.get('code_challenge')) problems.push('pkce')
      const scopes = (q.get('scope') || '').split(' ')
      if (!scopes.includes(MAIL_SCOPE) && !scopes.includes(DRIVE_SCOPE)) problems.push('scope')
      if (q.get('access_type') !== 'offline') problems.push('access_type')
      if (problems.length) return json(res, 400, { error: 'invalid_request', problems })

      const redirect = new URL(q.get('redirect_uri'))
      redirect.searchParams.set('state', q.get('state'))
      const email = q.get('login_hint') || 'oauthtest@gmail.com'
      if (email.startsWith('deny')) {
        redirect.searchParams.set('error', 'access_denied')
      } else {
        const code = '4/' + random()
        // En inloggning som börjar med "nodrive" motsvarar att användaren avmarkerar Drive på samtyckesskärmen.
        const granted = scopes.filter((scope) => !(email.startsWith('nodrive') && scope === DRIVE_SCOPE))
        codes.set(code, { challenge: q.get('code_challenge'), redirectUri: q.get('redirect_uri'), email, used: false, scope: granted.join(' ') })
        redirect.searchParams.set('code', code)
      }
      res.writeHead(302, { Location: redirect.toString() })
      return res.end()
    }

    if (req.method === 'POST' && url.pathname === '/token') {
      const body = await readBody(req)
      if (body.get('client_id') !== CLIENT_ID || body.get('client_secret') !== CLIENT_SECRET) {
        return json(res, 401, { error: 'invalid_client' })
      }
      if (body.get('grant_type') === 'authorization_code') {
        const entry = codes.get(body.get('code'))
        if (!entry || entry.used || entry.redirectUri !== body.get('redirect_uri')) {
          return json(res, 400, { error: 'invalid_grant', error_description: 'Bad code or redirect_uri' })
        }
        const expected = b64url(crypto.createHash('sha256').update(body.get('code_verifier') || '').digest())
        if (expected !== entry.challenge) {
          stats.pkceFailures++
          return json(res, 400, { error: 'invalid_grant', error_description: 'PKCE verification failed' })
        }
        entry.used = true
        stats.codeExchanges++
        const refresh = '1//' + random()
        refreshTokens.set(refresh, { email: entry.email, revoked: false })
        const idToken = [
          b64url(JSON.stringify({ alg: 'RS256', typ: 'JWT' })),
          b64url(JSON.stringify({ iss: 'https://accounts.google.com', email: entry.email, email_verified: true, name: 'OAuth Testare' })),
          'signatur',
        ].join('.')
        return json(res, 200, { access_token: issueAccess(entry.email), expires_in: ACCESS_TTL, refresh_token: refresh, id_token: idToken, token_type: 'Bearer', scope: entry.scope })
      }
      if (body.get('grant_type') === 'refresh_token') {
        const entry = refreshTokens.get(body.get('refresh_token'))
        if (!entry || entry.revoked) {
          return json(res, 400, { error: 'invalid_grant', error_description: 'Token has been expired or revoked.' })
        }
        stats.refreshes++
        return json(res, 200, { access_token: issueAccess(entry.email), expires_in: ACCESS_TTL, token_type: 'Bearer' })
      }
      return json(res, 400, { error: 'unsupported_grant_type' })
    }

    if (req.method === 'POST' && url.pathname === '/revoke') {
      const body = await readBody(req)
      const entry = refreshTokens.get(body.get('token'))
      if (entry) entry.revoked = true
      stats.revokes++
      return json(res, 200, {})
    }

    if (url.pathname.startsWith('/drive/') || url.pathname.startsWith('/upload/')) return drive(req, res, url)

    // Styrning för testerna.
    if (url.pathname === '/control/drive') {
      return json(res, 200, [...driveFiles.values()].map(({ data, ...f }) => f))
    }
    if (url.pathname.startsWith('/control/drive/content/')) {
      const file = driveFiles.get(url.pathname.split('/').pop())
      if (!file) return json(res, 404, {})
      res.writeHead(200, { 'Content-Type': 'application/zip' })
      return res.end(file.data)
    }
    if (url.pathname === '/control/drive/fail-chunks') {
      failDriveChunks = Number(q.get('count') || 1)
      return json(res, 200, { ok: true })
    }
    if (url.pathname === '/control/stats') return json(res, 200, { ...stats, ACCESS_TTL })
    if (url.pathname === '/control/revoke-all') {
      for (const entry of refreshTokens.values()) entry.revoked = true
      return json(res, 200, { ok: true })
    }
    if (url.pathname === '/control/fail-next-imap-auth') {
      failNextImapAuth = true
      return json(res, 200, { ok: true })
    }
    json(res, 404, { error: 'not_found' })
  })
  .listen(9999, () => console.log('OAuth-mock på :9999'))

// --- Google Drive API v3 (det som backupen använder) ---------------------------

function readRaw(req) {
  return new Promise((resolve) => {
    const chunks = []
    req.on('data', (c) => chunks.push(c))
    req.on('end', () => resolve(Buffer.concat(chunks)))
  })
}

function driveError(res, status, reason, message) {
  json(res, status, { error: { code: status, message, errors: [{ reason, message }] } })
}

// Delar av Drives frågespråk: name, mimeType, trashed, 'id' in parents och appProperties has { key and value }.
function matchesQuery(file, q) {
  if (!q) return true
  const str = (v) => v.replace(/\\(.)/g, '$1')
  const checks = []
  for (const m of q.matchAll(/name = '((?:[^'\\]|\\.)*)'/g)) checks.push(file.name === str(m[1]))
  for (const m of q.matchAll(/mimeType = '((?:[^'\\]|\\.)*)'/g)) checks.push(file.mimeType === str(m[1]))
  if (/trashed = false/.test(q)) checks.push(!file.trashed)
  for (const m of q.matchAll(/'((?:[^'\\]|\\.)*)' in parents/g)) checks.push(file.parents.includes(str(m[1])))
  for (const m of q.matchAll(/appProperties has \{ key='((?:[^'\\]|\\.)*)' and value='((?:[^'\\]|\\.)*)' \}/g)) {
    checks.push(file.appProperties?.[str(m[1])] === str(m[2]))
  }
  return checks.every(Boolean)
}

function publicFile(f) {
  return { id: f.id, name: f.name, mimeType: f.mimeType, createdTime: f.createdTime, trashed: f.trashed, size: String(f.size) }
}

async function drive(req, res, url) {
  const auth = /^Bearer (.+)$/.exec(req.headers.authorization || '')
  const token = auth && accessTokens.get(auth[1])
  if (!token || token.expires < Date.now()) return driveError(res, 401, 'authError', 'Invalid Credentials')
  const owner = token.email
  const own = (f) => f && f.owner === owner

  const fileMatch = /^\/drive\/v3\/files\/([^/]+)$/.exec(url.pathname)
  if (url.pathname === '/drive/v3/files' && req.method === 'GET') {
    let files = [...driveFiles.values()].filter((f) => own(f) && matchesQuery(f, url.searchParams.get('q')))
    if ((url.searchParams.get('orderBy') || '').startsWith('createdTime desc')) {
      files.sort((a, b) => (a.createdTime < b.createdTime ? 1 : a.createdTime > b.createdTime ? -1 : b.seq - a.seq))
    }
    return json(res, 200, { files: files.map(publicFile) })
  }
  if (url.pathname === '/drive/v3/files' && req.method === 'POST') {
    const meta = JSON.parse((await readRaw(req)).toString() || '{}')
    const file = createFile(meta, Buffer.alloc(0), owner)
    return json(res, 200, publicFile(file))
  }
  if (fileMatch && req.method === 'GET') {
    const file = driveFiles.get(fileMatch[1])
    if (!own(file)) return driveError(res, 404, 'notFound', 'File not found')
    return json(res, 200, publicFile(file))
  }
  if (fileMatch && req.method === 'DELETE') {
    const file = driveFiles.get(fileMatch[1])
    if (!own(file)) return driveError(res, 404, 'notFound', 'File not found')
    driveFiles.delete(file.id)
    res.writeHead(204)
    return res.end()
  }
  if (url.pathname === '/upload/drive/v3/files' && req.method === 'POST' && url.searchParams.get('uploadType') === 'resumable') {
    const meta = JSON.parse((await readRaw(req)).toString() || '{}')
    if (meta.parents?.some((p) => !own(driveFiles.get(p)))) return driveError(res, 404, 'notFound', 'Parent not found')
    const id = random()
    uploadSessions.set(id, { meta, total: Number(req.headers['x-upload-content-length']), chunks: [], length: 0, owner })
    res.writeHead(200, { Location: `http://${req.headers.host}/upload/session/${id}` })
    return res.end()
  }
  const sessionMatch = /^\/upload\/session\/([^/]+)$/.exec(url.pathname)
  if (sessionMatch && req.method === 'PUT') {
    const session = uploadSessions.get(sessionMatch[1])
    const body = await readRaw(req)
    if (!session || session.owner !== owner) return driveError(res, 404, 'notFound', 'Upload session not found')
    const range = req.headers['content-range'] || ''
    const status = /^bytes \*\/(\d+)$/.exec(range)
    const ack = () => {
      if (session.length === session.total) {
        const file = createFile(session.meta, Buffer.concat(session.chunks), owner)
        uploadSessions.delete(sessionMatch[1])
        return json(res, 200, publicFile(file))
      }
      res.writeHead(308, session.length ? { Range: `bytes=0-${session.length - 1}` } : {})
      return res.end()
    }
    if (status) {
      stats.driveResumes++
      return ack()
    }
    const m = /^bytes (\d+)-(\d+)\/(\d+)$/.exec(range)
    if (!m || Number(m[2]) - Number(m[1]) + 1 !== body.length) return driveError(res, 400, 'badRequest', 'Bad Content-Range')
    const start = Number(m[1])
    const last = Number(m[2]) + 1 === session.total
    if (!last && body.length % (256 * 1024) !== 0) return driveError(res, 400, 'badRequest', 'Chunk not multiple of 256 KiB')
    if (failDriveChunks > 0) {
      // Ta emot halva biten och svara med ett tillfälligt fel, som vid ett avbrott.
      failDriveChunks--
      stats.driveChunkFailures++
      if (start === session.length) {
        const half = body.subarray(0, Math.floor(body.length / 2 / (256 * 1024)) * 256 * 1024)
        session.chunks.push(half)
        session.length += half.length
      }
      return driveError(res, 503, 'backendError', 'Backend Error')
    }
    if (start !== session.length) return driveError(res, 400, 'badRequest', `Expected offset ${session.length}`)
    stats.driveChunks++
    session.chunks.push(body)
    session.length += body.length
    return ack()
  }
  driveError(res, 404, 'notFound', 'Unknown Drive endpoint')
}

let fileSeq = 0
function createFile(meta, data, owner) {
  const file = {
    id: 'f' + random().slice(0, 16),
    seq: ++fileSeq,
    name: meta.name,
    mimeType: meta.mimeType || 'application/octet-stream',
    parents: meta.parents || [],
    appProperties: meta.appProperties || {},
    createdTime: new Date().toISOString(),
    trashed: false,
    size: data.length,
    sha256: crypto.createHash('sha256').update(data).digest('hex'),
    owner,
    data,
  }
  driveFiles.set(file.id, file)
  return file
}

function imapServer(port, saslIr) {
  net
    .createServer((client) => {
      const caps = `IMAP4rev1 AUTH=XOAUTH2${saslIr ? ' SASL-IR' : ''}`
      client.write(`* OK [CAPABILITY ${caps}] Gmail-mock redo\r\n`)
      let buffer = ''
      let pendingAuthTag = null
      let proxied = false

      const verify = (tag, initial) => {
        const decoded = Buffer.from(initial, 'base64').toString('latin1')
        const match = /^user=([^\x01]*)\x01auth=Bearer ([^\x01]*)\x01\x01$/.exec(decoded)
        const token = match && accessTokens.get(match[2])
        const valid = token && token.email === match[1] && token.expires > Date.now() && !failNextImapAuth
        failNextImapAuth = false
        if (!valid) {
          stats.imapAuthFailed++
          const err = Buffer.from(JSON.stringify({ status: '401', schemes: 'bearer', scope: 'https://mail.google.com/' })).toString('base64')
          client.write(`+ ${err}\r\n`)
          pendingAuthTag = { tag, awaitingAck: true }
          return
        }
        stats.imapAuthOk++
        const upstream = net.connect(UPSTREAM)
        let upBuffer = ''
        let stage = 'greeting'
        upstream.on('data', (chunk) => {
          if (proxied) return client.write(chunk)
          upBuffer += chunk.toString('latin1')
          if (stage === 'greeting' && upBuffer.includes('\r\n')) {
            upBuffer = ''
            stage = 'login'
            upstream.write(`P1 LOGIN "${match[1]}" "x"\r\n`)
          } else if (stage === 'login' && /(^|\r\n)P1 OK/.test(upBuffer)) {
            proxied = true
            client.write(`${tag} OK [CAPABILITY IMAP4rev1 UIDPLUS MOVE] ${match[1]} authenticated (Success)\r\n`)
            if (buffer) {
              upstream.write(buffer)
              buffer = ''
            }
          }
        })
        upstream.on('close', () => client.end())
        upstream.on('error', () => client.destroy())
        client.on('close', () => upstream.destroy())
        client.removeAllListeners('data')
        client.on('data', (chunk) => (proxied ? upstream.write(chunk) : (buffer += chunk.toString('latin1'))))
      }

      client.on('data', (chunk) => {
        buffer += chunk.toString('latin1')
        let nl
        while (!proxied && (nl = buffer.indexOf('\r\n')) >= 0) {
          const line = buffer.slice(0, nl)
          buffer = buffer.slice(nl + 2)
          if (pendingAuthTag && pendingAuthTag.awaitingAck) {
            // Klienten kvitterar felmeddelandet med en tom rad; då kommer det taggade NO-svaret.
            client.write(`${pendingAuthTag.tag} NO [AUTHENTICATIONFAILED] Invalid credentials (Failure) ack=${JSON.stringify(line)}\r\n`)
            pendingAuthTag = null
            continue
          }
          if (pendingAuthTag) {
            const { tag } = pendingAuthTag
            pendingAuthTag = null
            verify(tag, line)
            continue
          }
          const [tag, cmd, mech, initial] = line.split(' ')
          const command = (cmd || '').toUpperCase()
          if (command === 'CAPABILITY') {
            client.write(`* CAPABILITY ${caps}\r\n${tag} OK Success\r\n`)
          } else if (command === 'AUTHENTICATE' && (mech || '').toUpperCase() === 'XOAUTH2') {
            if (initial) {
              if (!saslIr) {
                client.write(`${tag} BAD SASL-IR stöds inte\r\n`)
                continue
              }
              stats.saslIr++
              verify(tag, initial)
            } else {
              pendingAuthTag = { tag, awaitingAck: false }
              client.write('+ \r\n')
            }
          } else if (command === 'LOGOUT') {
            client.write(`* BYE\r\n${tag} OK\r\n`)
            client.end()
          } else if (command === 'LOGIN') {
            client.write(`${tag} NO [ALERT] Application-specific password required\r\n`)
          } else {
            client.write(`${tag} BAD Kräver inloggning\r\n`)
          }
        }
      })
      client.on('error', () => {})
    })
    .listen(port, () => console.log(`IMAP-mock på :${port} (SASL-IR: ${saslIr})`))
}

imapServer(1143, false)
imapServer(1144, true)
