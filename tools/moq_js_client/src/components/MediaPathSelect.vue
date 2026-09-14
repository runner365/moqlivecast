<template>
  <label class="lbl" for="fmt">Format</label>
  <select
    id="fmt"
    class="sel"
    :value="modelValue"
    :disabled="disabled"
    @change="onChange"
  >
    <option v-for="item in MEDIA_PATHS" :key="item.path" :value="item.path">
      /{{ item.path }} — {{ item.desc }}
    </option>
  </select>
</template>

<script setup lang="ts">
import { MEDIA_PATHS } from '../config'

defineProps<{
  modelValue: string
  disabled?: boolean
}>()

const emit = defineEmits<{
  'update:modelValue': [value: string]
}>()

function onChange(ev: Event) {
  const el = ev.target as HTMLSelectElement
  emit('update:modelValue', el.value)
}
</script>

<style scoped>
.lbl {
  flex: none;
  font-size: 14px;
  color: #c9d1d9;
}

.sel {
  flex: none;
  height: 40px;
  max-width: 280px;
  padding: 0 10px;
  border: 1px solid #30363d;
  border-radius: 8px;
  background: #161b22;
  color: #e6edf3;
  font-size: 13px;
}
</style>
