<script setup>
import { computed, ref, watch } from 'vue'
import { attachmentUrl, sourceUrl } from '../api'
import { t } from '../i18n'
import { useMailStore } from '../store'
import { formatAddress, formatFullDate, formatSize, linkify } from '../utils'
import Icon from './Icon.vue'

const store = useMailStore()
const allowRemote = ref(false)

const msg = computed(() => store.openMessage)
const isDraft = computed(() => (msg.value && store.folderInfo(msg.value.accountId, msg.value.folder)?.specialUse === 'drafts') || msg.value?.draft)
const visibleAttachments = computed(() => (msg.value?.attachments || []).filter((a) => !a.inline))
const attachmentCount = computed(() => (visibleAttachments.value.length === 1 ? t('1 attachment') : t('{n} attachments', { n: visibleAttachments.value.length })))

watch(
  () => msg.value?.key,
  () => {
    allowRemote.value = false
  },
)

// Avsändaren som användaren kan välja att alltid lita på. Bara när det finns exakt en avsändaradress.
const senderEmail = computed(() => (msg.value?.from?.length === 1 ? (msg.value.from[0].email || '').trim().toLowerCase() : ''))
const trustedSender = computed(() => !!senderEmail.value && store.remoteSenders.has(senderEmail.value))
const showRemote = computed(() => allowRemote.value || trustedSender.value)

const remotePattern = /(?:src|background|poster)\s*=\s*["']?\s*(?:https?:)?\/\/|url\(\s*["']?\s*(?:https?:)?\/\//i
const hasRemoteContent = computed(() => !!msg.value?.html && remotePattern.test(msg.value.html))

// HTML-meddelanden visas i en sandlådad iframe utan skript. CSP blockerar fjärrinnehåll tills användaren tillåter det.
const srcdoc = computed(() => {
  if (!msg.value?.html) return ''
  const remote = showRemote.value ? ' https: http:' : ''
  const csp = [
    "default-src 'none'",
    `img-src data:${remote}`,
    `style-src 'unsafe-inline'${remote}`,
    `font-src data:${remote}`,
    `media-src data:${remote}`,
  ].join('; ')
  // Inbäddade bilder (cid:) sätts in av store.loadInlineImages när de har hämtats. Till dess visas en tom bild,
  // så att läsrutans CSP inte loggar ett fel för varje bild.
  const blank = 'data:image/gif;base64,R0lGODlhAQABAAAAACH5BAEKAAEALAAAAAABAAEAAAICTAEAOw=='
  const body = msg.value.html.replace(/<meta\b[^>]*>/gi, '').replace(/(\bsrc\s*=\s*["']?)cid:[^"'\s>]+/gi, `$1${blank}`)
  return `<!doctype html><html><head><meta charset="utf-8">
<meta http-equiv="Content-Security-Policy" content="${csp}">
<base target="_blank">
<style>html{color-scheme:light}body{margin:12px;font:14px/1.5 system-ui,-apple-system,'Segoe UI',Roboto,sans-serif;color:#15141a;background:#fff;overflow-wrap:anywhere}img{max-width:100%;height:auto}blockquote{border-left:2px solid #ccc;margin-left:0;padding-left:10px;color:#555}</style>
</head><body>${body}</body></html>`
})

const textSegments = computed(() => (msg.value && !msg.value.html ? linkify(msg.value.text || '') : []))

function addressesTitle(list) {
  return list.map(formatAddress).join(', ')
}

// Bilagor öppnas alltid i en ny flik. De typer som servern visar direkt (samma lista som i backend)
// öppnas för läsning; övriga laddas ned, och den nya fliken stängs då av webbläsaren.
const viewableTypes = new Set(['image/png', 'image/jpeg', 'image/gif', 'image/webp', 'application/pdf', 'text/plain'])

function isViewable(a) {
  return viewableTypes.has((a.contentType || '').split(';')[0].trim().toLowerCase())
}

function attachmentHref(a) {
  return attachmentUrl(msg.value.accountId, msg.value.folder, msg.value.uid, a.index, !isViewable(a))
}
</script>

<template>
  <div class="message-view">
    <div v-if="store.loadingMessage && !msg" class="placeholder muted">{{ t('Loading message…') }}</div>
    <div v-else-if="store.messageError" class="placeholder error-text">{{ store.messageError }}</div>
    <div v-else-if="!msg" class="placeholder muted">
      <Icon name="mail" :size="48" />
      <p v-if="store.selection.size > 1">{{ t('{n} messages selected', { n: store.selection.size }) }}</p>
      <p v-else>{{ t('Select a message to read it.') }}</p>
    </div>

    <template v-else>
      <header class="headers">
        <div class="actions">
          <button v-if="isDraft" class="btn primary" @click="store.startCompose('editDraft')">{{ t('Edit draft') }}</button>
          <button class="btn" :title="t('Reply')" @click="store.startCompose('reply')"><Icon name="reply" /> {{ t('Reply') }}</button>
          <button class="btn" :title="t('Reply all')" @click="store.startCompose('replyAll')"><Icon name="replyAll" /></button>
          <button class="btn" :title="t('Forward')" @click="store.startCompose('forward')"><Icon name="forward" /></button>
          <button class="btn" :title="msg.flagged ? t('Remove star') : t('Star')" @click="store.setFlag('flagged', !msg.flagged, [msg.key])">
            <Icon name="star" class="star" :class="{ on: msg.flagged }" />
          </button>
          <button class="btn" :title="t('Mark as unread')" @click="store.setFlag('seen', false, [msg.key])">
            <Icon name="unread" />
          </button>
          <a class="btn" :title="t('View source')" :href="sourceUrl(msg.accountId, msg.folder, msg.uid)" target="_blank" rel="noopener">
            <Icon name="code" />
          </a>
          <button class="btn danger" :title="t('Delete')" @click="store.deleteMessages([msg.key])"><Icon name="trash" /></button>
        </div>

        <dl>
          <dt>{{ t('From') }}</dt>
          <dd>
            <button v-for="a in msg.from" :key="a.email" class="address" :title="t('Write to {email}', { email: a.email })" @click="store.composeTo(a)">
              <strong>{{ a.name || a.email }}</strong>
              <span v-if="a.name" class="muted"> &lt;{{ a.email }}&gt;</span>
            </button>
          </dd>
          <dt>{{ t('Subject') }}</dt>
          <dd class="subject">{{ msg.subject || t('(no subject)') }}</dd>
          <template v-if="msg.to.length">
            <dt>{{ t('To') }}</dt>
            <dd class="clip" :title="addressesTitle(msg.to)">{{ addressesTitle(msg.to) }}</dd>
          </template>
          <template v-if="msg.cc.length">
            <dt>{{ t('Cc') }}</dt>
            <dd class="clip" :title="addressesTitle(msg.cc)">{{ addressesTitle(msg.cc) }}</dd>
          </template>
          <dt>{{ t('Date') }}</dt>
          <dd>{{ formatFullDate(msg.date) }}</dd>
        </dl>
      </header>

      <div v-if="hasRemoteContent && !showRemote" class="remote-banner">
        <span>{{ t('Remote content has been blocked to protect your privacy.') }}</span>
        <button class="btn" @click="allowRemote = true">{{ t('Show remote content') }}</button>
        <button
          v-if="senderEmail"
          class="btn"
          :title="t('Remote content will be shown right away in all mail from {email}, including future messages', { email: senderEmail })"
          @click="store.setRemoteSenderTrusted(senderEmail, true)"
        >
          {{ t('Always show from {email}', { email: senderEmail }) }}
        </button>
      </div>
      <div v-else-if="hasRemoteContent && trustedSender" class="remote-banner trusted">
        <span>{{ t('Remote content is always shown from {email}.', { email: senderEmail }) }}</span>
        <button class="link" @click="store.setRemoteSenderTrusted(senderEmail, false)">{{ t('Stop trusting this sender') }}</button>
      </div>

      <div class="body">
        <iframe
          v-if="msg.html"
          :key="`${msg.key}-${showRemote}`"
          class="html-frame"
          sandbox="allow-popups allow-popups-to-escape-sandbox"
          referrerpolicy="no-referrer"
          :srcdoc="srcdoc"
          :title="t('Message content')"
        />
        <pre v-else class="plain"><template v-for="(seg, i) in textSegments" :key="i"><a v-if="seg.type === 'link'" :href="seg.value" target="_blank" rel="noopener noreferrer">{{ seg.value }}</a><template v-else>{{ seg.value }}</template></template></pre>
      </div>

      <p v-if="msg.partial" class="partial-note muted">
        {{ t('The mail server cannot be reached. The text is saved, but attachments may be missing from the list and cannot be opened right now.') }}
      </p>
      <footer v-if="visibleAttachments.length" class="attachments">
        <span class="muted"><Icon name="paperclip" :size="14" /> {{ attachmentCount }}</span>
        <a
          v-for="a in visibleAttachments"
          :key="a.index"
          class="attachment"
          :href="attachmentHref(a)"
          target="_blank"
          rel="noopener"
          :title="isViewable(a) ? t('Open {name} in a new tab', { name: a.filename }) : t('Download {name}', { name: a.filename })"
        >
          <Icon :name="isViewable(a) ? 'link' : 'download'" :size="14" />
          <span class="att-name">{{ a.filename }}</span>
          <span class="muted">{{ formatSize(a.size) }}</span>
        </a>
      </footer>
    </template>
  </div>
</template>

<style scoped>
.partial-note {
  margin: 0;
  padding: 6px 16px;
  font-size: 12px;
  border-top: 1px solid var(--border);
}

.message-view {
  height: 100%;
  display: flex;
  flex-direction: column;
  background: var(--pane);
  min-height: 0;
}

.placeholder {
  flex: 1;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 8px;
  opacity: 0.8;
}

.headers {
  padding: 10px 14px;
  border-bottom: 1px solid var(--border);
  background: var(--pane-alt);
  position: relative;
}

.actions {
  float: right;
  display: flex;
  gap: 4px;
  margin-left: 12px;
  flex-wrap: wrap;
  justify-content: flex-end;
}

.actions .btn {
  padding: 4px 8px;
  text-decoration: none;
}

dl {
  display: grid;
  grid-template-columns: max-content minmax(0, 1fr);
  gap: 2px 12px;
  margin: 0;
}

dt {
  color: var(--text-muted);
  text-align: right;
}

dd {
  margin: 0;
  min-width: 0;
}

.subject {
  font-weight: 600;
  font-size: 14px;
}

.clip {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.address {
  border: 0;
  background: none;
  padding: 0;
  cursor: pointer;
  text-align: left;
}

.address:hover strong {
  text-decoration: underline;
}

.star.on {
  color: var(--star);
  fill: var(--star);
}

.remote-banner {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 6px 12px;
  padding: 6px 14px;
  background: #fff4cc;
  color: #4a3b00;
  border-bottom: 1px solid var(--border);
}

.remote-banner.trusted {
  background: var(--pane-alt);
  color: var(--text-muted);
  font-size: 12px;
  padding: 3px 14px;
}

.remote-banner .link {
  border: 0;
  background: none;
  padding: 0;
  color: var(--accent);
  cursor: pointer;
  text-decoration: underline;
  font-size: inherit;
}

.body {
  flex: 1;
  min-height: 0;
  display: flex;
  overflow: auto;
}

.html-frame {
  flex: 1;
  border: 0;
  background: #fff;
  width: 100%;
}

.plain {
  margin: 0;
  padding: 14px;
  white-space: pre-wrap;
  overflow-wrap: anywhere;
  font: 14px/1.5 ui-monospace, 'Cascadia Mono', Consolas, monospace;
  width: 100%;
}

.plain a {
  color: var(--accent);
}

.attachments {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 8px 14px;
  border-top: 1px solid var(--border);
  flex-wrap: wrap;
  background: var(--pane-alt);
}

.attachment {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  padding: 4px 10px;
  border: 1px solid var(--border);
  border-radius: var(--radius);
  background: var(--pane);
  color: inherit;
  text-decoration: none;
  max-width: 280px;
}

.attachment:hover {
  background: var(--hover);
}

.att-name {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
</style>
