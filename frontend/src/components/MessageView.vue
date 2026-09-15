<script setup>
import { computed, ref, watch } from 'vue'
import { attachmentUrl, sourceUrl } from '../api'
import { useMailStore } from '../store'
import { formatAddress, formatFullDate, formatSize, linkify } from '../utils'
import Icon from './Icon.vue'

const store = useMailStore()
const allowRemote = ref(false)

const msg = computed(() => store.openMessage)
const isDraft = computed(() => store.currentFolderInfo?.specialUse === 'drafts' || msg.value?.draft)
const visibleAttachments = computed(() => (msg.value?.attachments || []).filter((a) => !a.inline))

watch(
  () => msg.value?.uid,
  () => {
    allowRemote.value = false
  },
)

const remotePattern = /(?:src|background|poster)\s*=\s*["']?\s*(?:https?:)?\/\/|url\(\s*["']?\s*(?:https?:)?\/\//i
const hasRemoteContent = computed(() => !!msg.value?.html && remotePattern.test(msg.value.html))

// HTML-meddelanden visas i en sandlådad iframe utan skript. CSP blockerar fjärrinnehåll tills användaren tillåter det.
const srcdoc = computed(() => {
  if (!msg.value?.html) return ''
  const remote = allowRemote.value ? ' https: http:' : ''
  const csp = [
    "default-src 'none'",
    `img-src data:${remote}`,
    `style-src 'unsafe-inline'${remote}`,
    `font-src data:${remote}`,
    `media-src data:${remote}`,
  ].join('; ')
  const body = msg.value.html.replace(/<meta\b[^>]*>/gi, '')
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

function attachmentHref(a, download = true) {
  return attachmentUrl(msg.value.accountId, msg.value.folder, msg.value.uid, a.index, download)
}
</script>

<template>
  <div class="message-view">
    <div v-if="store.loadingMessage && !msg" class="placeholder muted">Hämtar meddelande…</div>
    <div v-else-if="store.messageError" class="placeholder error-text">{{ store.messageError }}</div>
    <div v-else-if="!msg" class="placeholder muted">
      <Icon name="mail" :size="48" />
      <p v-if="store.selection.size > 1">{{ store.selection.size }} meddelanden markerade</p>
      <p v-else>Välj ett meddelande för att läsa det.</p>
    </div>

    <template v-else>
      <header class="headers">
        <div class="actions">
          <button v-if="isDraft" class="btn primary" @click="store.startCompose('editDraft')">Redigera utkast</button>
          <button class="btn" title="Svara" @click="store.startCompose('reply')"><Icon name="reply" /> Svara</button>
          <button class="btn" title="Svara alla" @click="store.startCompose('replyAll')"><Icon name="replyAll" /></button>
          <button class="btn" title="Vidarebefordra" @click="store.startCompose('forward')"><Icon name="forward" /></button>
          <button class="btn" :title="msg.flagged ? 'Ta bort stjärna' : 'Stjärnmärk'" @click="store.setFlag('flagged', !msg.flagged, [msg.uid])">
            <Icon name="star" class="star" :class="{ on: msg.flagged }" />
          </button>
          <button class="btn" title="Markera som oläst" @click="store.setFlag('seen', false, [msg.uid])">
            <Icon name="unread" />
          </button>
          <a class="btn" title="Visa källkod" :href="sourceUrl(msg.accountId, msg.folder, msg.uid)" target="_blank" rel="noopener">
            <Icon name="code" />
          </a>
          <button class="btn danger" title="Ta bort" @click="store.deleteMessages([msg.uid])"><Icon name="trash" /></button>
        </div>

        <dl>
          <dt>Från</dt>
          <dd>
            <button v-for="a in msg.from" :key="a.email" class="address" :title="`Skriv till ${a.email}`" @click="store.composeTo(a)">
              <strong>{{ a.name || a.email }}</strong>
              <span v-if="a.name" class="muted"> &lt;{{ a.email }}&gt;</span>
            </button>
          </dd>
          <dt>Ämne</dt>
          <dd class="subject">{{ msg.subject || '(inget ämne)' }}</dd>
          <template v-if="msg.to.length">
            <dt>Till</dt>
            <dd class="clip" :title="addressesTitle(msg.to)">{{ addressesTitle(msg.to) }}</dd>
          </template>
          <template v-if="msg.cc.length">
            <dt>Kopia</dt>
            <dd class="clip" :title="addressesTitle(msg.cc)">{{ addressesTitle(msg.cc) }}</dd>
          </template>
          <dt>Datum</dt>
          <dd>{{ formatFullDate(msg.date) }}</dd>
        </dl>
      </header>

      <div v-if="hasRemoteContent && !allowRemote" class="remote-banner">
        <span>Fjärrinnehåll har blockerats för att skydda din integritet.</span>
        <button class="btn" @click="allowRemote = true">Visa fjärrinnehåll</button>
      </div>

      <div class="body">
        <iframe
          v-if="msg.html"
          :key="`${msg.uid}-${allowRemote}`"
          class="html-frame"
          sandbox="allow-popups allow-popups-to-escape-sandbox"
          referrerpolicy="no-referrer"
          :srcdoc="srcdoc"
          title="Meddelandets innehåll"
        />
        <pre v-else class="plain"><template v-for="(seg, i) in textSegments" :key="i"><a v-if="seg.type === 'link'" :href="seg.value" target="_blank" rel="noopener noreferrer">{{ seg.value }}</a><template v-else>{{ seg.value }}</template></template></pre>
      </div>

      <footer v-if="visibleAttachments.length" class="attachments">
        <span class="muted"><Icon name="paperclip" :size="14" /> {{ visibleAttachments.length }} bilaga(or)</span>
        <a
          v-for="a in visibleAttachments"
          :key="a.index"
          class="attachment"
          :href="attachmentHref(a)"
          :title="`Ladda ned ${a.filename}`"
        >
          <Icon name="download" :size="14" />
          <span class="att-name">{{ a.filename }}</span>
          <span class="muted">{{ formatSize(a.size) }}</span>
        </a>
      </footer>
    </template>
  </div>
</template>

<style scoped>
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
  align-items: center;
  gap: 12px;
  padding: 6px 14px;
  background: #fff4cc;
  color: #4a3b00;
  border-bottom: 1px solid var(--border);
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
