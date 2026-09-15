import { api } from './api'

const TIMEOUT_MS = 10 * 60 * 1000

// Loggar in med Google i ett popup-fönster och väntar tills servern har tagit emot svaret.
// Google stänger ofta kopplingen till window.opener (COOP), så callback-sidan meddelar via
// BroadcastChannel och vi frågar dessutom servern regelbundet.
// Returnerar { promise, cancel }. promise ger { state, email, name }.
export function signInWithGoogle(loginHint = '') {
  // Fönstret måste öppnas direkt i klickhändelsen, annars stoppas det av popup-blockeraren.
  const popup = window.open('', 'tbk-google-login', 'popup,width=520,height=680')
  let cancelled = false
  let cancelFn = () => {
    cancelled = true
  }

  const promise = (async () => {
    let started
    try {
      started = await api.oauthStart(loginHint)
    } catch (e) {
      popup?.close()
      throw e
    }
    if (cancelled) {
      popup?.close()
      throw new Error('Inloggningen avbröts.')
    }
    if (popup) popup.location.href = started.url
    else if (!window.open(started.url, '_blank')) {
      throw new Error('Webbläsaren blockerade inloggningsfönstret. Tillåt popup-fönster för sidan och försök igen.')
    }

    return new Promise((resolve, reject) => {
      const channel = 'BroadcastChannel' in window ? new BroadcastChannel('tbk-oauth') : null
      const deadline = Date.now() + TIMEOUT_MS
      let finished = false

      const finish = (fn, value) => {
        if (finished) return
        finished = true
        clearInterval(timer)
        channel?.close()
        fn(value)
      }

      const check = async () => {
        if (finished) return
        try {
          const status = await api.oauthStatus(started.state)
          if (!status.done) return
          if (status.ok) finish(resolve, { state: started.state, email: status.email, name: status.name })
          else finish(reject, new Error(status.error || 'Google-inloggningen misslyckades.'))
        } catch (e) {
          finish(reject, e)
        }
      }

      const timer = setInterval(() => {
        if (Date.now() > deadline) finish(reject, new Error('Inloggningen tog för lång tid. Försök igen.'))
        else check()
      }, 1500)
      if (channel) {
        channel.onmessage = (event) => {
          if (event.data?.state === started.state) check()
        }
      }
      cancelFn = () => {
        try {
          popup?.close()
        } catch {
          // Fönstret kan redan vara stängt eller frikopplat.
        }
        finish(reject, new Error('Inloggningen avbröts.'))
      }
    })
  })()

  return { promise, cancel: () => cancelFn() }
}

export function isGoogleMailHost(host) {
  return ['imap.gmail.com', 'imap.googlemail.com'].includes((host || '').toLowerCase())
}

export function isGoogleMailAddress(email) {
  const domain = (email || '').split('@')[1]?.toLowerCase()
  return domain === 'gmail.com' || domain === 'googlemail.com'
}
