<script setup>
import { t } from '../i18n'

// Textruta för flera adresser, delad av kontodialogen och avsändardialogen.
defineProps({
  state: { type: Object, required: true },
  parsed: { type: Object, required: true },
  inputId: { type: String, default: 'bulk-addresses' },
})

defineEmits(['blur'])
</script>

<template>
  <div>
    <textarea
      :id="inputId"
      v-model="state.text"
      class="input addresses"
      rows="4"
      :placeholder="t('info@example.com\ncontact@example.com\nJane Doe <jane@example.com>')"
      @blur="$emit('blur')"
    />
    <p class="muted small">
      {{ t('One address per line, or separated by commas.') }} <slot name="hint" />
    </p>
    <p v-if="parsed.invalid.length" class="error-text small">{{ t('Could not parse: {list}', { list: parsed.invalid.join(', ') }) }}</p>
  </div>
</template>

<style scoped>
.addresses {
  resize: vertical;
}

.small {
  font-size: 12px;
  margin: 4px 0 0;
}
</style>
