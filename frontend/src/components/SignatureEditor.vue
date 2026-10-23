<script setup>
import { nextTick, ref } from 'vue'
import { sanitizeHtml, textToHtml } from '../richtext'
import RichEditor from './RichEditor.vue'

// Signatur i ren text eller med formatering. Textversionen sparas alltid, eftersom den används i meddelanden
// utan formatering; html är tom för en signatur i ren text.
const props = defineProps({
  text: { type: String, default: '' },
  html: { type: String, default: '' },
  inputId: { type: String, default: undefined },
})
const emit = defineEmits(['update:text', 'update:html'])

const rich = ref(!!props.html)
const editor = ref(null)
const initialHtml = ref(props.html ? sanitizeHtml(props.html) : '')

function onChange() {
  const root = editor.value.element()
  const text = editor.value.getTextWithoutImages()
  // En tömd redigerare lämnar kvar tomma taggar; då finns ingen signatur.
  const empty = !text.trim() && !root.querySelector('img, table, hr')
  emit('update:text', empty ? '' : text)
  emit('update:html', empty ? '' : editor.value.getHtml())
}

async function toggle(event) {
  if (event.target.checked) {
    initialHtml.value = textToHtml(props.text)
    rich.value = true
    await nextTick()
    // Signaturen räknas som formaterad direkt, även innan något har ändrats.
    if (props.text.trim()) emit('update:html', initialHtml.value)
    return
  }
  const formatted = editor.value.element().querySelector('b, i, u, s, strong, em, font, span[style], h1, h2, h3, ul, ol, img, a, table, hr')
  if (formatted && !window.confirm('Byta till ren text? Formatering och bilder i signaturen försvinner.')) {
    event.target.checked = true
    return
  }
  emit('update:text', editor.value.getTextWithoutImages())
  emit('update:html', '')
  rich.value = false
}
</script>

<template>
  <div class="signature-editor">
    <RichEditor v-if="rich" ref="editor" compact :initial-html="initialHtml" @change="onChange" />
    <textarea
      v-else
      :id="inputId"
      :value="text"
      class="input"
      rows="3"
      placeholder="Läggs till i nya meddelanden"
      @input="emit('update:text', $event.target.value)"
    />
    <label class="toggle">
      <input type="checkbox" :checked="rich" @change="toggle" />
      Formaterad signatur (typsnitt, färger, länkar, bilder och tabeller)
    </label>
    <p v-if="rich" class="muted hint">
      Används i meddelanden med formatering. I meddelanden i ren text används signaturens text utan formatering.
    </p>
  </div>
</template>

<style scoped>
.signature-editor {
  display: flex;
  flex-direction: column;
  gap: 6px;
  min-width: 0;
}

textarea {
  resize: vertical;
}

.toggle {
  display: flex;
  align-items: center;
  gap: 6px;
}

.hint {
  margin: 0;
  font-size: 12px;
}
</style>
