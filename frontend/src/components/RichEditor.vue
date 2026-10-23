<script setup>
import { onBeforeUnmount, onMounted, reactive, ref } from 'vue'
import { editorToText, imageToDataUrl, sanitizeHtml, CELL_STYLE, QUOTE_STYLE, TABLE_STYLE } from '../richtext'
import Icon from './Icon.vue'
import { t } from '../i18n'

// Redigerare för formaterad text. Innehållet sätts en gång (initialHtml ska redan vara tvättad med sanitizeHtml)
// och läses sedan med getHtml() och getText(), så att markören inte störs av omritningar.
const props = defineProps({
  initialHtml: { type: String, default: '' },
  compact: { type: Boolean, default: false }, // Lägre yta, t.ex. för signaturer i en dialog.
})
const emit = defineEmits(['change', 'error'])

const editor = ref(null)
const imageInput = ref(null)
const palette = ref(null) // Öppen meny: 'foreColor', 'hiliteColor', 'table' eller null
const inTable = ref(false)
const grid = reactive({ rows: 0, cols: 0 }) // Storleken som pekas ut i rutnätet för ny tabell.
const GRID_ROWS = 6
const GRID_COLS = 8
const active = reactive({ bold: false, italic: false, underline: false, strikeThrough: false, insertUnorderedList: false, insertOrderedList: false })
let savedRange = null

const colors = ['#000000', '#5b5b66', '#c50042', '#e8590c', '#e8a200', '#017a40', '#0061e0', '#6f42c1', '#ffffff']
const highlights = ['transparent', '#fff3a3', '#ffd6a5', '#ffc9de', '#c8f7c5', '#c5e4ff', '#e4d4ff']
const sizes = [
  { label: 'Small', value: '2' },
  { label: 'Normal', value: '3' },
  { label: 'Large', value: '5' },
  { label: 'Largest', value: '6' },
]
const blocks = [
  { label: 'Body text', value: 'p' },
  { label: 'Heading 1', value: 'h1' },
  { label: 'Heading 2', value: 'h2' },
  { label: 'Heading 3', value: 'h3' },
  { label: 'Monospace', value: 'pre' },
]
const fonts = [
  { label: 'Default', value: 'Helvetica, Arial, sans-serif' },
  { label: 'Serif', value: 'Georgia, "Times New Roman", serif' },
  { label: 'Monospace', value: '"Courier New", monospace' },
]

function exec(command, value = null) {
  editor.value.focus()
  restoreSelection()
  document.execCommand('styleWithCSS', false, true)
  document.execCommand(command, false, value)
  palette.value = null
  refreshState()
  emit('change')
}

function refreshState() {
  for (const key of Object.keys(active)) {
    try {
      active[key] = document.queryCommandState(key)
    } catch {
      active[key] = false
    }
  }
}

// Markeringen sparas när fokus lämnar redigeraren, t.ex. till en rullista eller en fråga om länkadress.
function saveSelection() {
  if (!editor.value) return // Fokus lämnar redigeraren när skrivfönstret stängs.
  const selection = window.getSelection()
  if (selection.rangeCount && editor.value.contains(selection.anchorNode)) savedRange = selection.getRangeAt(0).cloneRange()
}

function restoreSelection() {
  if (!savedRange) return
  const selection = window.getSelection()
  selection.removeAllRanges()
  selection.addRange(savedRange)
}

function onSelectionChange() {
  if (!editor.value || document.activeElement !== editor.value) return
  saveSelection()
  refreshState()
}

function onSelect(command, event) {
  const value = event.target.value
  event.target.selectedIndex = 0
  if (value) exec(command, value)
}

function addLink() {
  saveSelection()
  const selection = window.getSelection()
  const existing = selection.anchorNode?.parentElement?.closest('a')
  const url = window.prompt(t('Link address:'), existing?.getAttribute('href') || 'https://')
  if (url === null) return
  const href = url.trim()
  if (!href || href === 'https://') return exec('unlink')
  if (!/^(https?:|mailto:)/i.test(href)) {
    emit('error', t('The link must start with https://, http:// or mailto:'))
    return
  }
  if (savedRange?.collapsed && !existing) exec('insertHTML', `<a href="${href.replace(/"/g, '&quot;')}">${href.replace(/</g, '&lt;')}</a>`)
  else exec('createLink', href)
}

function toggleQuote() {
  restoreSelection()
  const inQuote = window.getSelection().anchorNode?.parentElement?.closest('blockquote')
  if (inQuote && editor.value.contains(inQuote)) return exec('outdent')
  exec('formatBlock', 'blockquote')
  const quote = window.getSelection().anchorNode?.parentElement?.closest('blockquote')
  if (quote) quote.setAttribute('style', QUOTE_STYLE)
}

// --- Tabeller ---------------------------------------------------------------
// execCommand saknar tabellkommandon, så rader och kolumner ändras direkt i DOM:en.

function currentCell() {
  const node = window.getSelection().anchorNode
  const element = node?.nodeType === Node.ELEMENT_NODE ? node : node?.parentElement
  const cell = element?.closest('td, th')
  return cell && editor.value.contains(cell) ? cell : null
}

function placeCaret(cell) {
  const range = document.createRange()
  range.selectNodeContents(cell)
  range.collapse(true)
  const selection = window.getSelection()
  selection.removeAllRanges()
  selection.addRange(range)
  saveSelection()
}

function emptyCopy(cell) {
  const copy = cell.cloneNode(false)
  copy.innerHTML = '<br>'
  return copy
}

function openTableMenu() {
  restoreSelection()
  inTable.value = !!currentCell()
  grid.rows = 0
  grid.cols = 0
  palette.value = palette.value === 'table' ? null : 'table'
}

function insertTable(rows, cols) {
  const row = `<tr>${`<td style="${CELL_STYLE}"><br></td>`.repeat(cols)}</tr>`
  // En tom rad efter tabellen, så att det går att skriva vidare under den.
  // Tabellen märks tillfälligt, så att markören kan ställas i första cellen i stället för efter tabellen.
  exec('insertHTML', `<table data-new style="${TABLE_STYLE}"><tbody>${row.repeat(rows)}</tbody></table><div><br></div>`)
  const table = editor.value.querySelector('table[data-new]')
  if (!table) return
  table.removeAttribute('data-new')
  // Webbläsaren rensar bort stilar som den anser överflödiga vid infogningen, så de sätts igen här.
  table.setAttribute('style', TABLE_STYLE)
  for (const cell of table.querySelectorAll('td')) cell.setAttribute('style', CELL_STYLE)
  placeCaret(table.rows[0].cells[0])
}

function tableOp(op) {
  editor.value.focus()
  restoreSelection()
  palette.value = null
  const cell = currentCell()
  if (!cell) return
  const row = cell.parentElement
  const table = cell.closest('table')
  const index = cell.cellIndex
  let focusCell = cell

  if (op === 'rowAbove' || op === 'rowBelow') {
    const copy = row.cloneNode(false)
    for (const c of row.cells) copy.append(emptyCopy(c))
    if (op === 'rowAbove') row.before(copy)
    else row.after(copy)
    focusCell = copy.cells[Math.min(index, copy.cells.length - 1)]
  } else if (op === 'colLeft' || op === 'colRight') {
    for (const r of table.rows) {
      const ref = r.cells[Math.min(index, r.cells.length - 1)]
      if (!ref) continue
      const copy = emptyCopy(ref)
      if (op === 'colLeft') ref.before(copy)
      else ref.after(copy)
      if (r === row) focusCell = copy
    }
  } else if (op === 'deleteRow' && table.rows.length > 1) {
    const next = row.nextElementSibling || row.previousElementSibling
    row.remove()
    focusCell = next.cells[Math.min(index, next.cells.length - 1)]
  } else if (op === 'deleteCol' && row.cells.length > 1) {
    for (const r of [...table.rows]) r.cells[index]?.remove()
    focusCell = row.cells[Math.min(index, row.cells.length - 1)]
  } else {
    // Hela tabellen, eller dess sista rad eller kolumn.
    const after = table.nextElementSibling
    table.remove()
    focusCell = null
    if (after) placeCaret(after)
  }
  if (focusCell) placeCaret(focusCell)
  emit('change')
}

// Tabb flyttar mellan celler som i ett kalkylblad, och ger en ny rad efter den sista cellen.
function tabInTable(event) {
  const cell = currentCell()
  if (!cell) return false
  event.preventDefault()
  const table = cell.closest('table')
  const cells = [...table.querySelectorAll('td, th')].filter((c) => c.closest('table') === table)
  const next = cells[cells.indexOf(cell) + (event.shiftKey ? -1 : 1)]
  if (next) placeCaret(next)
  else if (!event.shiftKey) tableOp('rowBelow')
  if (!next && !event.shiftKey) placeCaret(table.rows[table.rows.length - 1].cells[0])
  return true
}

async function insertImages(files) {
  for (const file of files) {
    try {
      const url = await imageToDataUrl(file)
      exec('insertHTML', `<img src="${url}" alt="${file.name.replace(/["<>&]/g, '')}" style="max-width: 100%;">`)
    } catch (e) {
      emit('error', e.message)
    }
  }
}

function onPaste(event) {
  const data = event.clipboardData
  if (!data) return
  const images = [...data.files].filter((f) => f.type.startsWith('image/'))
  if (images.length) {
    event.preventDefault()
    insertImages(images)
    return
  }
  const html = data.getData('text/html')
  if (!html) return // Ren text klistras in av webbläsaren.
  event.preventDefault()
  exec('insertHTML', sanitizeHtml(html))
}

function onKeydown(event) {
  if (event.key === 'Tab' && !event.ctrlKey && !event.metaKey && !event.altKey && tabInTable(event)) return
  if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'k') {
    event.preventDefault()
    addLink()
  }
}

function focus(atStart = true) {
  editor.value.focus()
  if (!atStart) return
  const range = document.createRange()
  range.setStart(editor.value, 0)
  range.collapse(true)
  const selection = window.getSelection()
  selection.removeAllRanges()
  selection.addRange(range)
}

defineExpose({
  getHtml: () => editor.value.innerHTML,
  getText: () => editorToText(editor.value),
  // Textversion utan platshållare för bilder, t.ex. för en signatur med logotyp.
  getTextWithoutImages: () => {
    const copy = editor.value.cloneNode(true)
    copy.querySelectorAll('img').forEach((img) => img.remove())
    return editorToText(copy)
  },
  element: () => editor.value,
  insertImages,
  focus,
})

onMounted(() => {
  editor.value.innerHTML = props.initialHtml
  document.execCommand('defaultParagraphSeparator', false, 'div')
  document.addEventListener('selectionchange', onSelectionChange)
})
onBeforeUnmount(() => document.removeEventListener('selectionchange', onSelectionChange))
</script>

<template>
  <div class="rich" :class="{ compact }">
    <div class="format-bar" @mousedown="$event.target.tagName !== 'SELECT' && $event.preventDefault()">
      <select class="pick" :title="t('Paragraph style')" @mousedown="saveSelection" @change="onSelect('formatBlock', $event)">
        <option value="">{{ t('Paragraph') }}</option>
        <option v-for="b in blocks" :key="b.value" :value="b.value">{{ t(b.label) }}</option>
      </select>
      <select class="pick" :title="t('Font')" @mousedown="saveSelection" @change="onSelect('fontName', $event)">
        <option value="">{{ t('Font') }}</option>
        <option v-for="f in fonts" :key="f.label" :value="f.value">{{ t(f.label) }}</option>
      </select>
      <select class="pick" :title="t('Text size')" @mousedown="saveSelection" @change="onSelect('fontSize', $event)">
        <option value="">{{ t('Size') }}</option>
        <option v-for="s in sizes" :key="s.value" :value="s.value">{{ t(s.label) }}</option>
      </select>
      <span class="sep" />
      <button type="button" :class="{ on: active.bold }" :title="t('Bold (Ctrl+B)')" @click="exec('bold')"><b>{{ t('B') }}</b></button>
      <button type="button" :class="{ on: active.italic }" :title="t('Italic (Ctrl+I)')" @click="exec('italic')"><i>{{ t('I') }}</i></button>
      <button type="button" :class="{ on: active.underline }" :title="t('Underline (Ctrl+U)')" @click="exec('underline')"><u>U</u></button>
      <button type="button" :class="{ on: active.strikeThrough }" :title="t('Strikethrough')" @click="exec('strikeThrough')"><s>S</s></button>
      <div class="palette-wrap">
        <button type="button" :title="t('Text color')" @click="palette = palette === 'foreColor' ? null : 'foreColor'">
          <span class="color-a">A</span>
        </button>
        <div v-if="palette === 'foreColor'" class="palette">
          <button v-for="c in colors" :key="c" type="button" class="swatch" :style="{ background: c }" :title="c" @click="exec('foreColor', c)" />
        </div>
      </div>
      <div class="palette-wrap">
        <button type="button" :title="t('Highlight')" @click="palette = palette === 'hiliteColor' ? null : 'hiliteColor'">
          <span class="color-bg">A</span>
        </button>
        <div v-if="palette === 'hiliteColor'" class="palette">
          <button
            v-for="c in highlights"
            :key="c"
            type="button"
            class="swatch"
            :class="{ none: c === 'transparent' }"
            :style="{ background: c }"
            :title="c === 'transparent' ? t('None') : c"
            @click="exec('hiliteColor', c)"
          />
        </div>
      </div>
      <span class="sep" />
      <button type="button" :class="{ on: active.insertUnorderedList }" :title="t('Bulleted list')" @click="exec('insertUnorderedList')"><Icon name="listBullets" /></button>
      <button type="button" :class="{ on: active.insertOrderedList }" :title="t('Numbered list')" @click="exec('insertOrderedList')"><Icon name="listNumbers" /></button>
      <button type="button" :title="t('Decrease indent')" @click="exec('outdent')"><Icon name="outdent" /></button>
      <button type="button" :title="t('Increase indent')" @click="exec('indent')"><Icon name="indent" /></button>
      <button type="button" :title="t('Quote')" @click="toggleQuote"><Icon name="quote" /></button>
      <span class="sep" />
      <button type="button" :title="t('Align left')" @click="exec('justifyLeft')"><Icon name="alignLeft" /></button>
      <button type="button" :title="t('Center')" @click="exec('justifyCenter')"><Icon name="alignCenter" /></button>
      <button type="button" :title="t('Align right')" @click="exec('justifyRight')"><Icon name="alignRight" /></button>
      <span class="sep" />
      <button type="button" :title="t('Link (Ctrl+K)')" @click="addLink"><Icon name="link" /></button>
      <button type="button" :title="t('Insert image')" @click="saveSelection(); imageInput.click()"><Icon name="image" /></button>
      <div class="palette-wrap">
        <button type="button" :title="t('Table')" @click="openTableMenu"><Icon name="table" /></button>
        <div v-if="palette === 'table'" class="palette table-menu">
          <template v-if="inTable">
            <button type="button" @click="tableOp('rowAbove')">{{ t('Insert row above') }}</button>
            <button type="button" @click="tableOp('rowBelow')">{{ t('Insert row below') }}</button>
            <button type="button" @click="tableOp('colLeft')">{{ t('Insert column to the left') }}</button>
            <button type="button" @click="tableOp('colRight')">{{ t('Insert column to the right') }}</button>
            <hr />
            <button type="button" @click="tableOp('deleteRow')">{{ t('Delete row') }}</button>
            <button type="button" @click="tableOp('deleteCol')">{{ t('Delete column') }}</button>
            <button type="button" class="danger" @click="tableOp('deleteTable')">{{ t('Delete table') }}</button>
            <hr />
          </template>
          <div class="grid-label">{{ grid.rows ? t('New table: {rows} × {cols}', { rows: grid.rows, cols: grid.cols }) : t('New table') }}</div>
          <div class="grid" @mouseleave="grid.rows = 0; grid.cols = 0">
            <template v-for="r in GRID_ROWS" :key="r">
              <button
                v-for="c in GRID_COLS"
                :key="`${r}-${c}`"
                type="button"
                class="grid-cell"
                :class="{ picked: r <= grid.rows && c <= grid.cols }"
                :title="`${r} × ${c}`"
                @mouseenter="grid.rows = r; grid.cols = c"
                @click="insertTable(r, c)"
              />
            </template>
          </div>
        </div>
      </div>
      <button type="button" :title="t('Horizontal line')" @click="exec('insertHorizontalRule')"><Icon name="rule" /></button>
      <button type="button" :title="t('Clear formatting')" @click="exec('removeFormat')"><Icon name="clearFormat" /></button>
      <input ref="imageInput" type="file" accept="image/png,image/jpeg,image/gif,image/webp" multiple hidden @change="insertImages($event.target.files); $event.target.value = ''" />
    </div>
    <div
      ref="editor"
      class="surface"
      contenteditable="true"
      spellcheck="true"
      role="textbox"
      aria-multiline="true"
      :aria-label="t('Message')"
      @input="emit('change')"
      @paste="onPaste"
      @keydown="onKeydown"
      @blur="saveSelection"
      @click="palette = null"
    />
  </div>
</template>

<style scoped>
.rich {
  flex: 1;
  min-height: 160px;
  display: flex;
  flex-direction: column;
}

.rich.compact {
  flex: none;
  min-height: 0;
  border: 1px solid var(--border);
  border-radius: var(--radius);
  overflow: hidden;
}

.rich.compact .surface {
  min-height: 110px;
  max-height: 240px;
}

.format-bar {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 2px;
  padding: 4px 12px;
  border-bottom: 1px solid var(--border);
  background: var(--pane-alt);
}

.format-bar button {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  min-width: 28px;
  height: 28px;
  padding: 0 6px;
  border: 0;
  border-radius: 4px;
  background: none;
  cursor: pointer;
  font-size: 14px;
}

.format-bar button b {
  font-weight: 800;
}

.format-bar button:hover {
  background: var(--hover);
}

.format-bar button.on {
  background: var(--selection-inactive);
}

.pick {
  height: 28px;
  border: 1px solid var(--border);
  border-radius: 4px;
  background: var(--pane);
  max-width: 110px;
}

.sep {
  width: 1px;
  height: 18px;
  background: var(--border);
  margin: 0 4px;
}

.color-a {
  border-bottom: 3px solid var(--danger);
  line-height: 1;
  padding: 0 2px;
  font-weight: 600;
}

.color-bg {
  background: #fff3a3;
  color: #15141a;
  line-height: 1.2;
  padding: 0 4px;
  border-radius: 2px;
  font-weight: 600;
}

.palette-wrap {
  position: relative;
}

.palette {
  position: absolute;
  top: 32px;
  left: 0;
  z-index: 5;
  display: grid;
  grid-template-columns: repeat(5, 22px);
  gap: 4px;
  padding: 6px;
  background: var(--pane);
  border: 1px solid var(--border);
  border-radius: var(--radius);
  box-shadow: var(--shadow);
}

.palette.table-menu {
  display: flex;
  flex-direction: column;
  gap: 0;
  min-width: 210px;
  right: 0;
  left: auto;
}

.format-bar .table-menu > button {
  justify-content: flex-start;
  width: 100%;
  font-size: 13px;
  padding: 0 8px;
}

.format-bar .table-menu > button.danger {
  color: var(--danger);
}

.table-menu hr {
  width: 100%;
  border: 0;
  border-top: 1px solid var(--border);
  margin: 4px 0;
}

.grid-label {
  font-size: 12px;
  color: var(--text-muted);
  padding: 2px 8px 4px;
}

.grid {
  display: grid;
  grid-template-columns: repeat(8, 18px);
  gap: 2px;
  padding: 0 8px 4px;
}

.format-bar .grid-cell {
  min-width: 18px;
  width: 18px;
  height: 18px;
  padding: 0;
  border: 1px solid var(--border);
  border-radius: 2px;
}

.format-bar .grid-cell.picked {
  background: var(--accent);
  border-color: var(--accent);
}

.format-bar .swatch {
  min-width: 22px;
  width: 22px;
  height: 22px;
  padding: 0;
  border: 1px solid var(--border);
}

.format-bar .swatch.none {
  background: linear-gradient(to top right, transparent 45%, var(--danger) 45%, var(--danger) 55%, transparent 55%) !important;
}

/* Ytan är alltid ljus, så att färger ser ut som hos mottagaren även i mörkt tema. */
.surface {
  flex: 1;
  overflow: auto;
  padding: 12px 16px;
  outline: none;
  background: #ffffff;
  color: #15141a;
  color-scheme: light;
  font: 14px/1.5 Helvetica, Arial, sans-serif;
  overflow-wrap: anywhere;
}

.surface :deep(blockquote) {
  margin: 0 0 0 0.8ex;
  border-left: 2px solid #729fcf;
  padding-left: 1ex;
}

.surface :deep(img) {
  max-width: 100%;
  height: auto;
}

.surface :deep(a) {
  color: #0061e0;
}

.surface :deep(pre) {
  white-space: pre-wrap;
  font-family: 'Courier New', monospace;
}

.surface :deep(h1),
.surface :deep(h2),
.surface :deep(h3) {
  margin: 0.6em 0 0.3em;
  line-height: 1.25;
}

/* Tomma celler ska gå att klicka i. */
.surface :deep(td),
.surface :deep(th) {
  min-width: 2em;
}
</style>
