import DOMPurify from 'dompurify'

// Hjälpfunktioner för formaterade meddelanden (HTML) i skrivfönstret.

export const QUOTE_STYLE = 'margin: 0 0 0 0.8ex; border-left: 2px solid #729fcf; padding-left: 1ex;'
export const SIGNATURE_CLASS = 'brevduva-signature'
const MAX_IMAGE_WIDTH = 1600

// Främmande HTML (citerade meddelanden, utkast, inklistrat innehåll) hamnar i en redigerbar yta i appens eget
// ursprung. Därför tas allt bort som kan köra skript eller hämta något utifrån.
const purifier = DOMPurify()
purifier.addHook('afterSanitizeAttributes', (node) => {
  if (node.nodeName === 'IMG') {
    // Bara inbäddade bilder behålls. Fjärrbilder skulle avslöja för avsändaren att meddelandet öppnats.
    if (!/^data:image\/(png|jpeg|gif|webp);base64,/i.test(node.getAttribute('src') || '')) node.remove()
    else node.removeAttribute('srcset')
    return
  }
  if (node.hasAttribute?.('style') && /url\s*\(|expression|position\s*:|@import/i.test(node.getAttribute('style'))) {
    node.removeAttribute('style')
  }
  for (const attr of ['background', 'poster']) node.removeAttribute?.(attr)
  if (node.nodeName === 'A' && node.hasAttribute('href')) {
    node.setAttribute('target', '_blank')
    node.setAttribute('rel', 'noopener noreferrer')
  }
})

export function sanitizeHtml(html) {
  return purifier.sanitize(html || '', {
    FORBID_TAGS: ['style', 'form', 'input', 'button', 'textarea', 'select', 'option', 'iframe', 'object', 'embed',
      'video', 'audio', 'source', 'svg', 'math', 'link', 'meta', 'base', 'picture'],
    FORBID_ATTR: ['id', 'name', 'contenteditable', 'tabindex'],
    ALLOW_DATA_ATTR: false,
  })
}

export function escapeHtml(text) {
  return String(text ?? '').replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;')
}

// Ren text som HTML: radbrytningar behålls och upprepade mellanslag kollapsar inte.
export function textToHtml(text) {
  return escapeHtml(text).replace(/ {2}/g, ' &nbsp;').replace(/\r?\n/g, '<br>')
}

// Avsändarens signatur som HTML: den formaterade versionen om det finns en, annars textversionen.
export function signatureHtml(sender) {
  const body = sender?.signatureHtml ? sanitizeHtml(sender.signatureHtml) : textToHtml(sender?.signature || '')
  return body ? `<div class="${SIGNATURE_CLASS}">-- <br>${body}</div>` : ''
}

// Inline-stilar, eftersom e-postprogram inte läser stilmallar.
export const TABLE_STYLE = 'border-collapse: collapse; width: 100%; margin: 4px 0;'
export const CELL_STYLE = 'border: 1px solid #999999; padding: 4px 8px; vertical-align: top;'

// Meddelandets innehåll som säker HTML, för citat vid svar och vidarebefordran.
export function messageHtml(message) {
  if (message.html && message.html.trim()) return sanitizeHtml(message.html)
  return textToHtml(message.text || '')
}

// Textversionen av ett formaterat meddelande, för mottagare som läser ren text. Citat får ">" som i vanlig e-post.
export function editorToText(root) {
  const blocks = new Set(['P', 'DIV', 'H1', 'H2', 'H3', 'H4', 'H5', 'H6', 'PRE', 'UL', 'OL', 'LI', 'TABLE', 'HR', 'BLOCKQUOTE'])

  function render(node, list) {
    if (node.nodeType === Node.TEXT_NODE) return node.textContent.replace(/\u00a0/g, ' ')
    if (node.nodeType !== Node.ELEMENT_NODE) return ''
    const tag = node.nodeName
    if (tag === 'BR') return '\n'
    if (tag === 'HR') return '\n----------\n'
    if (tag === 'IMG') {
      // En logotyp i signaturen nämns inte i textversionen.
      if (node.closest(`.${SIGNATURE_CLASS}`)) return ''
      return node.getAttribute('alt') ? `[${node.getAttribute('alt')}]` : '[bild]'
    }

    let index = 0
    let inner = ''
    for (const child of node.childNodes) {
      if (child.nodeName === 'LI') index++
      inner += render(child, { ordered: tag === 'OL', index })
    }

    if (tag === 'A') {
      const href = node.getAttribute('href') || ''
      return href && !href.startsWith('mailto:') && href !== inner.trim() ? `${inner} <${href}>` : inner
    }
    if (tag === 'LI') return `${list?.ordered ? `${list.index}.` : '•'} ${inner.trim()}\n`
    if (tag === 'TD' || tag === 'TH') return `${inner.trim()}\t`
    if (tag === 'TR') return `${inner.replace(/\t$/, '')}\n`
    if (tag === 'BLOCKQUOTE') {
      const quoted = inner.replace(/\n{3,}/g, '\n\n').trim().split('\n')
      return `\n${quoted.map((line) => (line.startsWith('>') ? `>${line}` : `> ${line}`)).join('\n')}\n`
    }
    if (blocks.has(tag)) return `\n${inner}\n`
    return inner
  }

  return render(root)
    .replace(/[ \t]+\n/g, '\n')
    .replace(/\n{3,}/g, '\n\n')
    .trim()
}

function readAsDataUrl(blob) {
  return new Promise((resolve, reject) => {
    const reader = new FileReader()
    reader.onload = () => resolve(reader.result)
    reader.onerror = () => reject(reader.error)
    reader.readAsDataURL(blob)
  })
}

// En bildfil som data-URI för infogning i meddelandet. Stora foton skalas ned, så att mejlet inte blir onödigt tungt.
// Servern gör om data-URI:n till en inbäddad bild (cid) när meddelandet skickas.
export async function imageToDataUrl(file) {
  if (!/^image\/(png|jpeg|gif|webp)$/.test(file.type)) throw new Error('Bildformatet stöds inte. Använd PNG, JPEG, GIF eller WebP.')
  const original = await readAsDataUrl(file)
  if (file.type === 'image/gif') return original // En animerad GIF skulle bli en stillbild.
  const image = new Image()
  await new Promise((resolve, reject) => {
    image.onload = resolve
    image.onerror = () => reject(new Error('Bilden kunde inte läsas'))
    image.src = original
  })
  if (image.naturalWidth <= MAX_IMAGE_WIDTH) return original
  const canvas = document.createElement('canvas')
  canvas.width = MAX_IMAGE_WIDTH
  canvas.height = Math.round((image.naturalHeight * MAX_IMAGE_WIDTH) / image.naturalWidth)
  canvas.getContext('2d').drawImage(image, 0, 0, canvas.width, canvas.height)
  return canvas.toDataURL(file.type === 'image/png' ? 'image/png' : 'image/jpeg', 0.85)
}
