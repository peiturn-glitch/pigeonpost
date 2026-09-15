let unauthorizedHandler = null

export function onUnauthorized(handler) {
  unauthorizedHandler = handler
}

function buildUrl(path, query) {
  let url = '/api' + path
  if (query) {
    const params = new URLSearchParams()
    for (const [key, value] of Object.entries(query)) {
      if (value !== undefined && value !== null && value !== '' && value !== false) {
        params.set(key, value === true ? '1' : String(value))
      }
    }
    const qs = params.toString()
    if (qs) url += '?' + qs
  }
  return url
}

async function request(method, path, { query, body, form } = {}) {
  const headers = { 'X-TBK': '1' }
  let payload
  if (form) {
    payload = form
  } else if (body !== undefined) {
    headers['Content-Type'] = 'application/json'
    payload = JSON.stringify(body)
  }

  const res = await fetch(buildUrl(path, query), { method, headers, body: payload, credentials: 'same-origin' })
  const contentType = res.headers.get('content-type') || ''
  const data = contentType.includes('json') ? await res.json() : await res.text()

  if (!res.ok) {
    const error = new Error((data && data.error) || `HTTP ${res.status}`)
    error.status = res.status
    error.data = data
    if (res.status === 401 && !['/login', '/session'].includes(path) && unauthorizedHandler) unauthorizedHandler()
    throw error
  }
  return data
}

function composeForm(message, files) {
  const form = new FormData()
  form.append('message', new Blob([JSON.stringify(message)], { type: 'application/json' }), 'message.json')
  for (const file of files) form.append('attachments', file, file.name)
  return form
}

export const api = {
  session: () => request('GET', '/session'),
  setup: (username, password) => request('POST', '/setup', { body: { username, password } }),
  login: (username, password) => request('POST', '/login', { body: { username, password } }),
  logout: () => request('POST', '/logout'),
  changePassword: (currentPassword, newPassword) =>
    request('POST', '/me/password', { body: { currentPassword, newPassword } }),

  users: () => request('GET', '/users'),
  createUser: (user) => request('POST', '/users', { body: user }),
  updateUser: (id, changes) => request('PUT', `/users/${id}`, { body: changes }),
  deleteUser: (id) => request('DELETE', `/users/${id}`),

  accounts: () => request('GET', '/accounts'),
  testAccount: (account) => request('POST', '/accounts/test', { body: account }),
  createAccount: (account) => request('POST', '/accounts', { body: account }),
  updateAccount: (id, account) => request('PUT', `/accounts/${id}`, { body: account }),
  deleteAccount: (id) => request('DELETE', `/accounts/${id}`),

  oauthProviders: () => request('GET', '/oauth/providers'),
  oauthStart: (loginHint) => request('POST', '/oauth/google/start', { body: { loginHint } }),
  oauthStatus: (state) => request('GET', '/oauth/google/status', { query: { state } }),

  thunderbirdImportPreview: (prefs) => request('POST', '/import/thunderbird/preview', { body: { prefs } }),

  testIdentity: (accountId, identity) => request('POST', `/accounts/${accountId}/identities/test`, { body: identity }),
  createIdentity: (accountId, identity) => request('POST', `/accounts/${accountId}/identities`, { body: identity }),
  updateIdentity: (accountId, id, identity) =>
    request('PUT', `/accounts/${accountId}/identities/${id}`, { body: identity }),
  deleteIdentity: (accountId, id) => request('DELETE', `/accounts/${accountId}/identities/${id}`),

  folders: (accountId) => request('GET', `/accounts/${accountId}/folders`),
  createFolder: (accountId, path) => request('POST', `/accounts/${accountId}/folders`, { body: { path } }),
  renameFolder: (accountId, from, to) =>
    request('POST', `/accounts/${accountId}/folders/rename`, { body: { from, to } }),
  deleteFolder: (accountId, path) => request('POST', `/accounts/${accountId}/folders/delete`, { body: { path } }),

  messages: (accountId, query) => request('GET', `/accounts/${accountId}/messages`, { query }),
  message: (accountId, folder, uid, markSeen = true) =>
    request('GET', `/accounts/${accountId}/message`, { query: { folder, uid, markSeen: markSeen ? '1' : '0' } }),
  setFlags: (accountId, folder, uids, add = [], remove = []) =>
    request('POST', `/accounts/${accountId}/messages/flags`, { body: { folder, uids, add, remove } }),
  move: (accountId, folder, uids, target) =>
    request('POST', `/accounts/${accountId}/messages/move`, { body: { folder, uids, target } }),
  remove: (accountId, folder, uids) =>
    request('POST', `/accounts/${accountId}/messages/delete`, { body: { folder, uids } }),

  send: (accountId, message, files) =>
    request('POST', `/accounts/${accountId}/send`, { form: composeForm(message, files) }),
  saveDraft: (accountId, message, files) =>
    request('POST', `/accounts/${accountId}/drafts`, { form: composeForm(message, files) }),
}

export function attachmentUrl(accountId, folder, uid, index, download = true) {
  return buildUrl(`/accounts/${accountId}/attachment`, { folder, uid, index, download })
}

export const thunderbirdExportUrl = '/api/export/thunderbird'

export function sourceUrl(accountId, folder, uid) {
  return buildUrl(`/accounts/${accountId}/source`, { folder, uid })
}
