<script setup>
import { onBeforeUnmount, onMounted } from 'vue'
import { DEFAULT_LAYOUT, useMailStore } from '../store'
import Icon from './Icon.vue'

const store = useMailStore()
const layout = store.layout

const folderOptions = [
  { value: 'left', label: 'Vänster' },
  { value: 'right', label: 'Höger' },
  { value: 'hidden', label: 'Dold' },
]

const previewOptions = [
  { value: 'bottom', label: 'Under listan' },
  { value: 'top', label: 'Ovanför listan' },
  { value: 'right', label: 'Till höger om listan' },
  { value: 'left', label: 'Till vänster om listan' },
  { value: 'hidden', label: 'Dold' },
]

const presets = [
  { name: 'Klassisk', folders: 'left', preview: 'bottom' },
  { name: 'Bred', folders: 'left', preview: 'right' },
  { name: 'Spegelvänd', folders: 'right', preview: 'left' },
  { name: 'Utan läsruta', folders: 'left', preview: 'hidden' },
  { name: 'Bara lista och läsruta', folders: 'hidden', preview: 'right' },
]

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
  store.layoutDialog = false
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
        <h2>Layout</h2>
        <button class="btn ghost" title="Stäng" @click="close"><Icon name="close" /></button>
      </div>

      <div class="content">
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
            <span class="preset-name">{{ preset.name }}</span>
          </button>
        </div>

        <div class="controls">
          <fieldset>
            <legend>Mappfält</legend>
            <label v-for="o in folderOptions" :key="o.value" class="radio">
              <input v-model="layout.folders" type="radio" :value="o.value" /> {{ o.label }}
            </label>
          </fieldset>
          <fieldset>
            <legend>Läsruta</legend>
            <label v-for="o in previewOptions" :key="o.value" class="radio">
              <input v-model="layout.preview" type="radio" :value="o.value" /> {{ o.label }}
            </label>
          </fieldset>

          <div class="big mini" :class="[`folders-${layout.folders}`, `preview-${layout.preview}`]" aria-hidden="true">
            <span v-if="layout.folders !== 'hidden'" class="box f">Mappar</span>
            <span class="stack">
              <span class="box l">Meddelanden</span>
              <span v-if="layout.preview !== 'hidden'" class="box p">Läsruta</span>
            </span>
          </div>
        </div>

        <p class="muted hint">
          Dra i kanterna mellan panelerna för att ändra storlek. <kbd>F8</kbd> visar eller döljer läsrutan och
          <kbd>F9</kbd> mappfältet. Utan läsruta öppnas ett meddelande med dubbelklick eller <kbd>Enter</kbd>.
        </p>
      </div>

      <div class="modal-footer">
        <button class="btn" @click="resetSizes">Återställ storlekar</button>
        <div class="spacer" />
        <button class="btn primary" @click="close">Klar</button>
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

kbd {
  font-family: ui-monospace, Consolas, monospace;
  font-size: 11px;
  border: 1px solid var(--border);
  border-bottom-width: 2px;
  border-radius: 3px;
  padding: 0 4px;
  background: var(--pane-alt);
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
