<script setup>
import { computed, onBeforeUnmount, onMounted } from 'vue'
import { LANGUAGES, lang, setLanguage, t } from '../i18n'
import { DEFAULT_LAYOUT, useMailStore } from '../store'
import Icon from './Icon.vue'

const store = useMailStore()
const layout = store.layout

// Hur länge meddelanden ligger i papperskorgen innan servern tar bort dem (0 = aldrig). Sparas per användare på
// servern, så valet gäller även i appen. Ett värde som inte finns i listan (satt via API:t) visas ändå.
const TRASH_CHOICES = [0, 7, 14, 30, 90]
const trashDays = computed(() => store.user?.trashDays ?? 0)
const trashOptions = computed(() =>
  (TRASH_CHOICES.includes(trashDays.value) ? TRASH_CHOICES : [...TRASH_CHOICES, trashDays.value].sort((a, b) => a - b)).map(
    (n) => ({ value: n, label: n === 0 ? t('Never') : n === 1 ? t('After 1 day') : t('After {n} days', { n }) }),
  ),
)

// Labels are translated with t() when rendered, so a language change updates them.
const folderOptions = [
  { value: 'left', label: 'Left' },
  { value: 'right', label: 'Right' },
  { value: 'hidden', label: 'Hidden' },
]

const previewOptions = [
  { value: 'bottom', label: 'Below the list' },
  { value: 'top', label: 'Above the list' },
  { value: 'right', label: 'Right of the list' },
  { value: 'left', label: 'Left of the list' },
  { value: 'hidden', label: 'Hidden' },
]

const presets = [
  { name: 'Classic', folders: 'left', preview: 'bottom' },
  { name: 'Wide', folders: 'left', preview: 'right' },
  { name: 'Mirrored', folders: 'right', preview: 'left' },
  { name: 'No reading pane', folders: 'left', preview: 'hidden' },
  { name: 'List and reading pane only', folders: 'hidden', preview: 'right' },
]

// The hint contains <kbd> markup, so the key names are inserted as placeholders and rendered with v-html.
const hintHtml = computed(() =>
  t(
    'Drag the edges between the panes to resize them. {f8} shows or hides the reading pane and {f9} the folder pane. Without a reading pane, a message opens with a double-click or {enter}.',
    { f8: '<kbd>F8</kbd>', f9: '<kbd>F9</kbd>', enter: '<kbd>Enter</kbd>' },
  ),
)

function applyPreset(preset) {
  layout.folders = preset.folders
  layout.preview = preset.preview
}

function isActive(preset) {
  return layout.folders === preset.folders && layout.preview === preset.preview
}

function resetSizes() {
  layout.folderWidth = DEFAULT_LAYOUT.folderWidth
  layout.listHeight = DEFAULT_LAYOUT.listHeight
  layout.listWidth = DEFAULT_LAYOUT.listWidth
}

function close() {
  store.settingsDialog = false
}

function onKey(event) {
  if (event.key === 'Escape') close()
}
onMounted(() => window.addEventListener('keydown', onKey))
onBeforeUnmount(() => window.removeEventListener('keydown', onKey))
</script>

<template>
  <div class="modal-backdrop" @click.self="close">
    <div class="modal layout-dialog">
      <div class="modal-header">
        <h2>{{ t('Settings') }}</h2>
        <button class="btn ghost" :title="t('Close')" @click="close"><Icon name="close" /></button>
      </div>

      <div class="content">
        <h3 class="section-title">{{ t('Layout') }}</h3>
        <div class="presets">
          <button
            v-for="preset in presets"
            :key="preset.name"
            class="preset"
            :class="{ active: isActive(preset) }"
            @click="applyPreset(preset)"
          >
            <span class="mini" :class="[`folders-${preset.folders}`, `preview-${preset.preview}`]">
              <span v-if="preset.folders !== 'hidden'" class="box f" />
              <span class="stack">
                <span class="box l" />
                <span v-if="preset.preview !== 'hidden'" class="box p" />
              </span>
            </span>
            <span class="preset-name">{{ t(preset.name) }}</span>
          </button>
        </div>

        <div class="controls">
          <fieldset>
            <legend>{{ t('Folder pane') }}</legend>
            <label v-for="o in folderOptions" :key="o.value" class="radio">
              <input v-model="layout.folders" type="radio" :value="o.value" /> {{ t(o.label) }}
            </label>
          </fieldset>
          <fieldset>
            <legend>{{ t('Reading pane') }}</legend>
            <label v-for="o in previewOptions" :key="o.value" class="radio">
              <input v-model="layout.preview" type="radio" :value="o.value" /> {{ t(o.label) }}
            </label>
          </fieldset>

          <div class="big mini" :class="[`folders-${layout.folders}`, `preview-${layout.preview}`]" aria-hidden="true">
            <span v-if="layout.folders !== 'hidden'" class="box f">{{ t('Folders') }}</span>
            <span class="stack">
              <span class="box l">{{ t('Messages') }}</span>
              <span v-if="layout.preview !== 'hidden'" class="box p">{{ t('Reading pane') }}</span>
            </span>
          </div>
        </div>

        <!-- v-html is safe here: the markup comes from our own translation table, never from user data. -->
        <!-- eslint-disable-next-line vue/no-v-html -->
        <p class="muted hint" v-html="hintHtml" />

        <div class="prefs">
          <label class="pref-row">
            <span class="pref-label">{{ t('Language') }}</span>
            <select class="input pref-select" :value="lang" @change="setLanguage($event.target.value)">
              <option v-for="l in LANGUAGES" :key="l.code" :value="l.code">{{ l.name }}</option>
            </select>
          </label>
          <label class="pref-row">
            <span class="pref-label">{{ t('Empty trash automatically') }}</span>
            <select class="input pref-select" :value="trashDays" @change="store.saveSettings({ trashDays: Number($event.target.value) })">
              <option v-for="o in trashOptions" :key="o.value" :value="o.value">{{ o.label }}</option>
            </select>
          </label>
          <p class="muted hint pref-hint">
            {{ t('Messages that have been in the trash longer than this are deleted for good. The time counts from when a message ended up in the trash, also when it was deleted from another email program.') }}
          </p>
        </div>
      </div>

      <div class="modal-footer">
        <button class="btn" @click="resetSizes">{{ t('Reset sizes') }}</button>
        <div class="spacer" />
        <button class="btn primary" @click="close">{{ t('Done') }}</button>
      </div>
    </div>
  </div>
</template>

<style scoped>
.layout-dialog {
  max-width: 640px;
}

.content {
  padding: 16px;
  overflow-y: auto;
}

.presets {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(104px, 1fr));
  gap: 8px;
  margin-bottom: 16px;
}

.preset {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 6px;
  padding: 8px;
  border: 1px solid var(--border);
  border-radius: var(--radius);
  background: var(--pane);
  cursor: pointer;
}

.preset:hover {
  background: var(--hover);
}

.preset.active {
  border-color: var(--accent);
  outline: 1px solid var(--accent);
}

.preset-name {
  font-size: 12px;
  text-align: center;
}

.controls {
  display: grid;
  grid-template-columns: auto auto 1fr;
  gap: 16px;
  align-items: start;
}

fieldset {
  border: 0;
  margin: 0;
  padding: 0;
  display: flex;
  flex-direction: column;
  gap: 4px;
}

legend {
  font-weight: 600;
  margin-bottom: 4px;
}

.radio {
  display: flex;
  align-items: center;
  gap: 6px;
  white-space: nowrap;
}

/* Miniatyr av layouten; samma flex-riktningar som i huvudfönstret. */
.mini {
  display: flex;
  gap: 2px;
  width: 88px;
  height: 56px;
}

.mini.folders-right {
  flex-direction: row-reverse;
}

.stack {
  flex: 1;
  display: flex;
  flex-direction: column;
  gap: 2px;
  min-width: 0;
}

.mini.preview-top .stack {
  flex-direction: column-reverse;
}

.mini.preview-right .stack {
  flex-direction: row;
}

.mini.preview-left .stack {
  flex-direction: row-reverse;
}

.box {
  border-radius: 2px;
  display: flex;
  align-items: center;
  justify-content: center;
  font-size: 11px;
  overflow: hidden;
  min-width: 0;
}

.box.f {
  width: 26%;
  background: var(--pane-alt);
  border: 1px solid var(--border);
}

.box.l {
  flex: 1;
  background: var(--selection-inactive);
  border: 1px solid var(--border);
}

.box.p {
  flex: 1.3;
  background: var(--pane);
  border: 1px solid var(--border);
}

.big {
  width: 100%;
  height: 150px;
  min-width: 180px;
}

.hint {
  margin: 16px 0 0;
  font-size: 12px;
}

/* :deep because the kbd elements are inserted with v-html and lack the scoped attribute. */
.hint :deep(kbd) {
  font-family: ui-monospace, Consolas, monospace;
  font-size: 11px;
  border: 1px solid var(--border);
  border-bottom-width: 2px;
  border-radius: 3px;
  padding: 0 4px;
  background: var(--pane-alt);
}

.section-title {
  margin: 0 0 12px;
  font-size: 14px;
  font-weight: 600;
}

.prefs {
  margin-top: 16px;
  padding-top: 16px;
  border-top: 1px solid var(--border);
  display: flex;
  flex-direction: column;
  gap: 10px;
}

.pref-row {
  display: flex;
  align-items: center;
  gap: 8px;
}

.pref-label {
  font-weight: 600;
  min-width: 200px;
}

.pref-select {
  width: auto;
}

.pref-hint {
  margin: 0;
}

.spacer {
  flex: 1;
}

@media (max-width: 560px) {
  .controls {
    grid-template-columns: 1fr 1fr;
  }

  .big {
    grid-column: 1 / -1;
  }
}
</style>
