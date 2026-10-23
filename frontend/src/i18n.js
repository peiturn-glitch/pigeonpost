// Translation of the UI. English text is written directly in the source and doubles as the key;
// the Swedish table in ./locales/sv/*.js maps it to Swedish. A missing translation falls back to
// the English text, so new strings never break the UI. English is the default language.
import { ref, watch } from 'vue'

const STORAGE_KEY = 'brevduva.lang'

export const LANGUAGES = [
  { code: 'en', name: 'English' },
  { code: 'sv', name: 'Svenska' },
]

const svModules = import.meta.glob('./locales/sv/*.js', { eager: true })
const tables = {
  sv: Object.assign({}, ...Object.values(svModules).map((m) => m.default)),
}

function initialLanguage() {
  try {
    const saved = localStorage.getItem(STORAGE_KEY)
    if (saved && LANGUAGES.some((l) => l.code === saved)) return saved
  } catch {
    // localStorage can be unavailable (private mode); fall through to the default.
  }
  return 'en'
}

export const lang = ref(initialLanguage())

watch(
  lang,
  (value) => {
    try {
      localStorage.setItem(STORAGE_KEY, value)
    } catch {
      // Not critical: the choice simply does not survive a reload.
    }
    document.documentElement.lang = value
  },
  { immediate: true },
)

export function setLanguage(code) {
  if (LANGUAGES.some((l) => l.code === code)) lang.value = code
}

// BCP 47 tag for Intl.*, toLocaleLowerCase, localeCompare etc.
export function locale() {
  return lang.value
}

// t('Delete {n} messages?', { n: 3 }) -> 'Delete 3 messages?' (or the Swedish equivalent).
// Reads lang.value, so templates and computed values re-render when the language changes.
// A key may carry a context after '|' when the same English word needs different translations,
// e.g. t('Archive|action') (the toolbar verb) next to t('Archive') (the folder); English shows
// only the part before the bar.
export function t(text, params) {
  const table = tables[lang.value]
  let out = table && Object.prototype.hasOwnProperty.call(table, text) ? table[text] : null
  if (!out) {
    const bar = text.indexOf('|')
    out = bar === -1 ? text : text.slice(0, bar)
  }
  if (params) {
    for (const [key, value] of Object.entries(params)) out = out.split(`{${key}}`).join(String(value))
  }
  return out
}
