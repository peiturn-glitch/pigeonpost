<script setup>
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
      placeholder="info@exempel.se&#10;kontakt@exempel.se&#10;Namn Namnsson &lt;namn@exempel.se&gt;"
      @blur="$emit('blur')"
    />
    <p class="muted small">
      En adress per rad, eller åtskilda med komma. <slot name="hint" />
    </p>
    <p v-if="parsed.invalid.length" class="error-text small">Kunde inte tolka: {{ parsed.invalid.join(', ') }}</p>
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
