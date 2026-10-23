// Låtsas-AI med OpenAI-kompatibelt API för test av AI-sorteringen. Modellnamnet styr beteendet.
import http from 'node:http'

const PORT = Number(process.env.PORT || 9998)
const API_KEY = process.env.API_KEY || 'test-key'

const requests = [] // Senaste anropen till /chat/completions, för kontroll i testet.
let flakyCalls = 0

function send(res, status, body) {
  res.writeHead(status, { 'Content-Type': 'application/json' })
  res.end(JSON.stringify(body))
}

// Enkla regler i stället för en riktig modell: ämnet avgör mappen.
function sort(input) {
  const byName = (part) => input.folders.find((f) => f.name.toLowerCase().includes(part))
  const byRole = (role) => input.folders.find((f) => f.role === role)
  // En rättning för samma avsändare går före reglerna nedan, som för en modell som lär sig av exemplen.
  const address = (text) => (text.match(/<([^>]+)>/) || [null, text])[1].toLowerCase()
  // Bara rättningar som pekar ut en mapp följs, så att en ångrad flytt i ett test inte styr alla senare.
  const corrected = (m) => (input.corrections || []).find((c) => c.correct !== null && address(c.from) === address(m.from))
  return input.messages.map((m) => {
    const subject = m.subject.toLowerCase()
    let folder = null
    let reason = 'Ser ut som personlig post'
    const lesson = corrected(m)
    if (lesson) {
      const taught = input.folders.find((f) => f.name === lesson.correct)
      return { id: m.id, folder: taught ? taught.id : null, reason: 'Enligt en tidigare rättning' }
    }
    if (subject.includes('faktura')) [folder, reason] = [byName('ekonomi'), 'Faktura hör till ekonomi']
    else if (subject.includes('nyhetsbrev')) [folder, reason] = [byName('nyhetsbrev'), 'Nyhetsbrev']
    else if (subject.includes('vinn')) [folder, reason] = [byRole('junk'), 'Ser ut som skräppost']
    else if (m.text.toLowerCase().includes('kvitto')) [folder, reason] = [byName('ekonomi'), 'Texten nämner ett kvitto']
    return { id: m.id, folder: folder ? folder.id : null, reason }
  })
}

// Tolkar en sökfråga med enkla regler, som en modell skulle ha gjort.
function interpret(input) {
  const question = input.question.toLowerCase()
  const year = Number(input.today.slice(0, 4))
  const out = { keywords: [], from: '', to: '', after: null, before: null, unread: false, flagged: false, attachments: false, account: null, folder: null, explanation: 'Söker enligt frågan.' }
  if (question.includes('faktur')) out.keywords.push(['faktur', 'invoice', 'att betala'])
  if (question.includes('middag')) out.keywords.push(['middag', 'dinner'])
  if (question.includes('elbolaget')) out.from = 'elbolaget'
  if (question.includes('förra året')) [out.after, out.before] = [`${year - 1}-01-01`, `${year - 1}-12-31`]
  if (question.includes('i år')) [out.after, out.before] = [`${year}-01-01`, `${year}-12-31`]
  if (question.includes('oläs')) out.unread = true
  if (question.includes('bilaga')) out.attachments = true
  if (question.includes('ekonomi')) {
    const account = input.accounts.find((a) => a.folders.includes('Ekonomi'))
    if (account) [out.account, out.folder] = [account.id, 'Ekonomi']
  }
  if (question.includes('påhittad')) Object.assign(out, { account: 99, folder: 'Finns inte', after: 'i går', keywords: [['a|b "c', 7], 'ensam'], from: { x: 1 } })
  return out
}

const server = http.createServer((req, res) => {
  let body = ''
  req.on('data', (chunk) => (body += chunk))
  req.on('end', () => {
    const url = new URL(req.url, 'http://localhost')
    if (url.pathname === '/control/requests') return send(res, 200, requests)
    if (url.pathname === '/control/reset') {
      requests.length = 0
      flakyCalls = 0
      return send(res, 200, { ok: true })
    }

    if (req.headers.authorization !== `Bearer ${API_KEY}`) {
      return send(res, 401, { error: { message: 'Incorrect API key provided' } })
    }
    if (req.method === 'GET' && url.pathname === '/v1/models') {
      return send(res, 200, { object: 'list', data: [{ id: 'models/mock-sorter' }, { id: 'mock-fenced' }] })
    }
    if (req.method !== 'POST' || url.pathname !== '/v1/chat/completions') {
      return send(res, 404, { error: { message: 'Not found' } })
    }

    const request = JSON.parse(body)
    requests.push(request)
    const input = JSON.parse(request.messages.find((m) => m.role === 'user').content)
    const reply = (content) => send(res, 200, { choices: [{ finish_reason: 'stop', message: { role: 'assistant', content } }] })
    // En sökfråga i stället för meddelanden att sortera.
    if (input.question !== undefined && request.model === 'mock-sorter') return reply(JSON.stringify(interpret(input)))

    switch (request.model) {
      case 'mock-sorter':
        return reply(JSON.stringify({ results: sort(input) }))
      case 'mock-fenced':
        // Stöder inte response_format och lägger text och kodstaket runt svaret.
        if (request.response_format) return send(res, 400, { error: { message: 'Unknown parameter: response_format' } })
        return reply('Här är sorteringen:\n```json\n' + JSON.stringify(sort(input)) + '\n```')
      case 'mock-evil':
        // Påhittade mappar, fel typer och saknade poster ska aldrig bli förslag.
        return reply(JSON.stringify({
          results: [
            { id: 0, folder: 999, reason: 'Påhittad mapp' },
            { id: 1, folder: 'INBOX/../Trash', reason: 'Sökväg i stället för id' },
            { id: 77, folder: 0, reason: 'Okänt meddelande' },
            'skräp',
          ],
        }))
      case 'mock-flaky':
        if (flakyCalls++ === 0) return send(res, 503, { error: { message: 'The model is overloaded' } })
        return reply(JSON.stringify({ results: sort(input) }))
      case 'mock-busy':
        return send(res, 429, [{ error: { message: 'Quota exceeded for requests per minute' } }])
      case 'mock-prose':
        return reply('Jag kan tyvärr inte hjälpa till med det.')
      default:
        return send(res, 404, { error: { message: `Model ${request.model} not found` } })
    }
  })
})

server.listen(PORT, () => console.log(`Låtsas-AI lyssnar på ${PORT}`))
