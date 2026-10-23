import { computed, reactive } from 'vue'
import { parseAddresses } from './utils'

// Gemensam logik för att lägga till flera adresser på en gång, i kontodialogen och i avsändardialogen.
// existing() ger de adresser som redan finns, och create(entry, skipTest) skapar en adress hos servern.
export function useBulkAddresses({ existing, create }) {
  const state = reactive({
    enabled: false,
    text: '',
    usernameMode: 'address', // 'address' = adressen är användarnamn, 'same' = ett gemensamt
    test: true,
    entries: [], // { name, email, status, message, canSkipTest }
  })

  const parsed = computed(() => parseAddresses(state.text))
  const taken = computed(() => new Set(existing().map((email) => email.toLowerCase())))
  // Adresser som saknas; de som redan finns läggs inte till igen.
  const pending = computed(() => parsed.value.addresses.filter((a) => !taken.value.has(a.email.toLowerCase())))
  const failures = computed(() => state.entries.filter((e) => e.status === 'error'))

  function reset() {
    state.entries = []
  }

  // Ett anslutningstest som misslyckats beskrivs med serverns eget felmeddelande.
  function describeError(e) {
    const test = e.data?.test
    if (!test) return e.message
    if (test.imap || test.smtp) {
      const parts = [test.imap?.ok ? null : `IMAP: ${test.imap?.error}`, test.smtp?.ok ? null : `SMTP: ${test.smtp?.error}`]
      return parts.filter(Boolean).join(' · ') || e.message
    }
    return test.error || e.message
  }

  // Bygger listan från textrutan. Adresser som redan lagts till behåller sitt resultat.
  function prepare() {
    const done = new Map(state.entries.filter((e) => e.status === 'ok').map((e) => [e.email, e]))
    state.entries = parsed.value.addresses.map(
      (a) =>
        done.get(a.email) || {
          ...a,
          status: taken.value.has(a.email.toLowerCase()) ? 'exists' : 'queued',
          message: '',
          canSkipTest: false,
        },
    )
  }

  // Skapar en adress i taget och returnerar de som lyckades.
  async function run({ skipTest = false, onlyFailed = false } = {}) {
    if (!onlyFailed) prepare()
    const created = []
    for (const entry of state.entries) {
      if (entry.status === 'ok' || entry.status === 'exists') continue
      if (onlyFailed && entry.status !== 'error') continue
      entry.status = 'working'
      entry.message = ''
      try {
        created.push(await create(entry, skipTest))
        entry.status = 'ok'
      } catch (e) {
        entry.status = 'error'
        entry.canSkipTest = !!e.data?.test
        entry.message = describeError(e)
      }
    }
    return created
  }

  return { state, parsed, pending, failures, reset, run }
}
