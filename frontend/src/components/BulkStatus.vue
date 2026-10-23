<script setup>
import { t } from '../i18n'

// Resultat per adress när flera adresser läggs till på en gång.
defineProps({
  entries: { type: Array, required: true },
  // Vad en färdig rad ska säga, t.ex. "Tillagt".
  doneText: { type: String, default: '' },
  existsText: { type: String, default: '' },
})

const marks = { ok: '✓', error: '✗', exists: '•', working: '…' }
</script>

<template>
  <ul v-if="entries.length" class="bulk-status">
    <li v-for="entry in entries" :key="entry.email" :class="entry.status">
      <span class="mark">{{ marks[entry.status] || '·' }}</span>
      <span class="addr">{{ entry.email }}</span>
      <span class="msg">
        <template v-if="entry.status === 'ok'">{{ doneText || t('Added') }}</template>
        <template v-else-if="entry.status === 'exists'">{{ existsText || t('Already exists') }}</template>
        <template v-else-if="entry.status === 'working'">{{ t('Adding…') }}</template>
        <template v-else>{{ entry.message || t('Waiting') }}</template>
      </span>
    </li>
  </ul>
</template>

<style scoped>
.bulk-status {
  list-style: none;
  margin: 8px 0 0;
  padding: 0;
  font-size: 12px;
}

.bulk-status li {
  display: flex;
  gap: 8px;
  padding: 2px 0;
  align-items: baseline;
}

.mark {
  width: 12px;
  text-align: center;
  color: var(--text-muted);
}

.addr {
  min-width: 180px;
  overflow-wrap: anywhere;
}

.msg {
  color: var(--text-muted);
  overflow-wrap: anywhere;
}

li.ok .mark {
  color: var(--success);
}

li.error .mark,
li.error .msg {
  color: var(--danger);
}
</style>
