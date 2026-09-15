<script setup>
import { computed, onMounted, reactive, ref, watch } from 'vue'
import { api } from '../api'
import { useMailStore } from '../store'
import { formatSize } from '../utils'
import Icon from './Icon.vue'

const store = useMailStore()
const draft = store.compose

const form = reactive({
  senderKey: draft.senderKey,
  to: draft.to,
  cc: draft.cc,
  bcc: draft.bcc,
  subject: draft.subject,
  text: draft.text,
})
const files = ref([])
const showCc = ref(!!draft.cc)
const showBcc = ref(!!draft.bcc)
const busy = ref(false)
const error = ref('')
const dragOver = ref(false)
// Sparat utkast, med kontot det ligger i.
const draftRef = ref(draft.draft ? { ...draft.draft, accountId: draft.accountId } : null)
const initial = JSON.stringify(form)

const sender = computed(() => store.senderByKey(form.senderKey))
const accountId = computed(() => sender.value.accountId)

// Byt signatur när avsändaren byts, som i Thunderbird.
watch(
  () => form.senderKey,
  (next, previous) => {
    const oldSig = store.signatureFor(store.senderByKey(previous))
    const newSig = store.signatureFor(store.senderByKey(next))
    if (oldSig === newSig) return
    if (oldSig && form.text.includes(oldSig)) form.text = form.text.replace(oldSig, newSig)
    else if (newSig) form.text += newSig
  },
)

const toInput = ref(null)
const bodyInput = ref(null)
const fileInput = ref(null)

const title = computed(() => form.subject || 'Nytt meddelande')
const dirty = computed(() => JSON.stringify(form) !== initial || files.value.length > 0)

onMounted(() => {
  // Vid svar hamnar markören överst i brödtexten; annars i mottagarfältet.
  if (form.to && bodyInput.value) {
    bodyInput.value.focus()
    bodyInput.value.setSelectionRange(0, 0)
  } else {
    toInput.value?.focus()
  }
})

function payload() {
  // Referenser till originalmeddelandet (besvarat, bilagor) gäller bara dess eget konto.
  const sameAccount = accountId.value === draft.accountId
  const savedDraft = draftRef.value && draftRef.value.accountId === accountId.value ? draftRef.value : null
  return {
    identityId: sender.value.identityId,
    to: form.to,
    cc: form.cc,
    bcc: form.bcc,
    subject: form.subject,
    text: form.text,
    inReplyTo: draft.inReplyTo,
    references: draft.references,
    replyTo: sameAccount ? draft.replyTo : null,
    forwardOf: sameAccount ? draft.forwardOf : null,
    draft: savedDraft ? { folder: savedDraft.folder, uid: savedDraft.uid } : null,
  }
}

async function send() {
  if (!form.to.trim() && !form.cc.trim() && !form.bcc.trim()) {
    error.value = 'Ange minst en mottagare.'
    return
  }
  if (!form.subject.trim() && !window.confirm('Meddelandet saknar ämne. Skicka ändå?')) return
  busy.value = true
  error.value = ''
  try {
    const target = accountId.value
    const result = await api.send(target, payload(), files.value)
    store.notify('Meddelandet skickades')
    for (const w of result.warnings || []) store.notify(w, 'error')
    store.compose = null
    store.onMessageSent(target)
  } catch (e) {
    error.value = e.message
  } finally {
    busy.value = false
  }
}

async function saveDraft() {
  busy.value = true
  error.value = ''
  try {
    const target = accountId.value
    const result = await api.saveDraft(target, payload(), files.value)
    draftRef.value = result.uid ? { folder: result.folder, uid: result.uid, accountId: target } : null
    store.notify('Utkastet sparades')
    store.onMessageSent(target)
  } catch (e) {
    error.value = e.message
  } finally {
    busy.value = false
  }
}

function close() {
  if (dirty.value && !window.confirm('Stäng utan att skicka? Osparade ändringar går förlorade.')) return
  store.compose = null
}

function addFiles(list) {
  for (const f of list) files.value.push(f)
}

function onDrop(event) {
  dragOver.value = false
  if (event.dataTransfer.files.length) addFiles(event.dataTransfer.files)
}

function onKeydown(event) {
  if ((event.ctrlKey || event.metaKey) && event.key === 'Enter') {
    event.preventDefault()
    send()
  } else if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 's') {
    event.preventDefault()
    saveDraft()
  } else if (event.key === 'Escape') {
    close()
  }
}
</script>

<template>
  <div class="modal-backdrop" @keydown="onKeydown">
    <div
      class="modal compose"
      :class="{ 'drag-over': dragOver }"
      @dragover.prevent="dragOver = true"
      @dragleave.self="dragOver = false"
      @drop.prevent="onDrop"
    >
      <div class="modal-header">
        <h2>{{ title }}</h2>
        <button class="btn ghost" title="Stäng" @click="close"><Icon name="close" /></button>
      </div>

      <div class="toolbar">
        <button class="btn primary" :disabled="busy" title="Skicka (Ctrl+Enter)" @click="send">
          <Icon name="sent" /> {{ busy ? 'Arbetar…' : 'Skicka' }}
        </button>
        <button class="btn" :disabled="busy" title="Spara utkast (Ctrl+S)" @click="saveDraft">Spara utkast</button>
        <button class="btn" :disabled="busy" @click="fileInput.click()"><Icon name="paperclip" /> Bifoga</button>
        <input ref="fileInput" type="file" multiple hidden @change="addFiles($event.target.files); $event.target.value = ''" />
      </div>

      <div class="fields">
        <label>Från</label>
        <div class="with-extras">
        <select v-model="form.senderKey" class="input" title="Välj vilken adress och server meddelandet skickas från">
          <optgroup v-for="a in store.accounts" :key="a.id" :label="a.email">
            <option v-for="s in store.senders.filter((x) => x.accountId === a.id)" :key="s.key" :value="s.key">
              {{ s.name }} &lt;{{ s.email }}&gt;{{ s.via ? ` · via ${s.via}` : '' }}
            </option>
          </optgroup>
        </select>
          <button class="link" title="Lägg till eller ändra avsändare för kontot" @click="store.accountDialog = { ...store.account(accountId) }">
            Hantera avsändare
          </button>
        </div>

        <label>Till</label>
        <div class="with-extras">
          <input ref="toInput" v-model="form.to" class="input" type="text" placeholder="namn@exempel.se, …" />
          <button v-if="!showCc" class="link" @click="showCc = true">Kopia</button>
          <button v-if="!showBcc" class="link" @click="showBcc = true">Dold kopia</button>
        </div>

        <template v-if="showCc">
          <label>Kopia</label>
          <input v-model="form.cc" class="input" type="text" />
        </template>
        <template v-if="showBcc">
          <label>Dold kopia</label>
          <input v-model="form.bcc" class="input" type="text" />
        </template>

        <label>Ämne</label>
        <input v-model="form.subject" class="input" type="text" />
      </div>

      <textarea ref="bodyInput" v-model="form.text" class="body" spellcheck="true" />

      <div v-if="files.length || draft.forwardedAttachments.length" class="attachments">
        <span v-for="a in draft.forwardedAttachments" :key="'fwd' + a.index" class="chip" title="Följer med från originalmeddelandet">
          <Icon name="paperclip" :size="12" /> {{ a.filename }} <span class="muted">{{ formatSize(a.size) }}</span>
        </span>
        <span v-for="(f, i) in files" :key="i" class="chip">
          <Icon name="paperclip" :size="12" /> {{ f.name }} <span class="muted">{{ formatSize(f.size) }}</span>
          <button class="remove" title="Ta bort bilaga" @click="files.splice(i, 1)"><Icon name="close" :size="12" /></button>
        </span>
      </div>

      <p v-if="error" class="error-text error">{{ error }}</p>
    </div>
  </div>
</template>

<style scoped>
.compose {
  max-width: 860px;
  height: min(760px, calc(100vh - 32px));
}

.compose.drag-over {
  outline: 3px dashed var(--accent);
  outline-offset: -6px;
}

.toolbar {
  display: flex;
  gap: 6px;
  padding: 8px 16px;
  border-bottom: 1px solid var(--border);
}

.fields {
  display: grid;
  grid-template-columns: max-content minmax(0, 1fr);
  gap: 6px 10px;
  align-items: center;
  padding: 10px 16px;
  border-bottom: 1px solid var(--border);
}

.fields label {
  color: var(--text-muted);
  text-align: right;
}

.with-extras {
  display: flex;
  gap: 8px;
  align-items: center;
}

.link {
  border: 0;
  background: none;
  color: var(--accent);
  cursor: pointer;
  white-space: nowrap;
  padding: 0;
}

.body {
  flex: 1;
  border: 0;
  resize: none;
  padding: 12px 16px;
  outline: none;
  background: var(--pane);
  font: 14px/1.5 ui-monospace, 'Cascadia Mono', Consolas, monospace;
  min-height: 160px;
}

.attachments {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
  padding: 8px 16px;
  border-top: 1px solid var(--border);
}

.chip {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  padding: 3px 8px;
  border: 1px solid var(--border);
  border-radius: 12px;
  font-size: 12px;
}

.remove {
  border: 0;
  background: none;
  padding: 0;
  cursor: pointer;
  display: inline-flex;
  color: var(--text-muted);
}

.error {
  margin: 0;
  padding: 8px 16px;
  border-top: 1px solid var(--border);
}
</style>
