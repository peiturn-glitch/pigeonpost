<script setup>
import { nextTick, onBeforeUnmount, onMounted, ref } from 'vue'
import { useMailStore } from '../store'

const store = useMailStore()
const el = ref(null)
const pos = ref({ left: store.contextMenu.x, top: store.contextMenu.y })

function close() {
  store.contextMenu = null
}

function run(item) {
  close()
  item.action()
}

function onKey(e) {
  if (e.key === 'Escape') close()
}

onMounted(async () => {
  await nextTick()
  // Håll menyn inom fönstret.
  const rect = el.value.getBoundingClientRect()
  pos.value = {
    left: Math.min(pos.value.left, window.innerWidth - rect.width - 8),
    top: Math.min(pos.value.top, window.innerHeight - rect.height - 8),
  }
  window.addEventListener('keydown', onKey)
})

onBeforeUnmount(() => window.removeEventListener('keydown', onKey))
</script>

<template>
  <div class="overlay" @click="close" @contextmenu.prevent="close">
    <div ref="el" class="context-menu" :style="{ left: pos.left + 'px', top: pos.top + 'px' }" @click.stop>
      <button v-for="item in store.contextMenu.items" :key="item.label" :class="{ danger: item.danger }" @click="run(item)">
        {{ item.label }}
      </button>
    </div>
  </div>
</template>

<style scoped>
.overlay {
  position: fixed;
  inset: 0;
  z-index: 60;
}

.context-menu {
  position: fixed;
  background: var(--pane);
  border: 1px solid var(--border);
  border-radius: var(--radius);
  box-shadow: var(--shadow);
  padding: 4px;
  min-width: 200px;
  display: flex;
  flex-direction: column;
}

button {
  text-align: left;
  padding: 7px 10px;
  border: 0;
  background: none;
  border-radius: 4px;
  cursor: pointer;
}

button:hover {
  background: var(--hover);
}

button.danger {
  color: var(--danger);
}
</style>
