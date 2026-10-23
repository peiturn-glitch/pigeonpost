<script setup>
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
import { useMailStore } from '../store'
import { isInsideFolder, specialFolderNames } from '../utils'
import Icon from './Icon.vue'

const store = useMailStore()
const { accountId, folder } = store.folderMoveDialog
const busy = ref(false)

// Alla mappar utom mappen själv och dess undermappar. Tom sökväg = översta nivån.
const destinations = computed(() =>
  (store.folders[accountId] || [])
    .filter((f) => !isInsideFolder(f.path, folder) && !['all', 'flagged', 'important'].includes(f.specialUse))
    .map((f) => ({
      path: f.path,
      label: specialFolderNames[f.specialUse] || f.path,
      depth: f.delimiter ? f.path.split(f.delimiter).length : 1,
    })),
)
const parent = ref(store.parentPath(folder))

async function submit() {
  busy.value = true
  const moved = await store.moveFolder(accountId, folder, parent.value, { confirm: false })
  busy.value = false
  if (moved) store.folderMoveDialog = null
}

function onKey(event) {
  if (event.key === 'Escape') store.folderMoveDialog = null
}
onMounted(() => window.addEventListener('keydown', onKey))
onBeforeUnmount(() => window.removeEventListener('keydown', onKey))
</script>

<template>
  <div class="modal-backdrop">
    <form class="modal move" @submit.prevent="submit">
      <div class="modal-header">
        <h2>Flytta mappen ”{{ folder.name }}”</h2>
        <button type="button" class="btn ghost" title="Stäng" @click="store.folderMoveDialog = null"><Icon name="close" /></button>
      </div>
      <div class="content">
        <label>
          <span>Flytta till</span>
          <select v-model="parent" class="input" size="12" autofocus>
            <option value="">(Översta nivån)</option>
            <option v-for="d in destinations" :key="d.path" :value="d.path" :style="{ paddingLeft: d.depth * 12 + 'px' }">
              {{ d.label }}
            </option>
          </select>
        </label>
        <p class="muted">Undermappar och meddelanden följer med. Du kan också dra mappen dit du vill ha den i mapplistan.</p>
      </div>
      <div class="modal-footer">
        <button type="button" class="btn" @click="store.folderMoveDialog = null">Avbryt</button>
        <button type="submit" class="btn primary" :disabled="busy || parent === store.parentPath(folder)">
          {{ busy ? 'Flyttar…' : 'Flytta' }}
        </button>
      </div>
    </form>
  </div>
</template>

<style scoped>
.move {
  max-width: 440px;
}

.content {
  padding: 16px;
  display: flex;
  flex-direction: column;
  gap: 12px;
}

label {
  display: flex;
  flex-direction: column;
  gap: 4px;
}

label span {
  font-size: 12px;
  color: var(--text-muted);
}

select {
  padding: 4px;
}

p {
  margin: 0;
  font-size: 12px;
}
</style>
