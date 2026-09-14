<template>
  <div class="phase" :class="tone" role="status">{{ phase }}</div>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import type { SessionPhase } from '../media/sessionPhase'

const props = defineProps<{
  phase: SessionPhase
}>()

const tone = computed(() => {
  switch (props.phase) {
    case 'connecting':
      return 'info'
    case 'connected and waiting for stream':
      return 'wait'
    case 'receiving stream':
    case 'pushing stream':
      return 'ok'
    case 'disconnected':
      return 'off'
    default:
      return 'idle'
  }
})
</script>

<style scoped>
.phase {
  position: absolute;
  left: 10px;
  top: 10px;
  max-width: calc(100% - 20px);
  padding: 6px 10px;
  border-radius: 999px;
  background: rgba(1, 4, 9, 0.72);
  border: 1px solid rgba(48, 54, 61, 0.9);
  color: #e6edf3;
  font-size: 12px;
  line-height: 1.35;
  pointer-events: none;
  backdrop-filter: blur(6px);
}

.phase.idle {
  color: #8b949e;
}

.phase.info {
  color: #79c0ff;
  border-color: rgba(56, 139, 253, 0.55);
}

.phase.wait {
  color: #e3b341;
  border-color: rgba(210, 153, 34, 0.55);
}

.phase.ok {
  color: #3fb950;
  border-color: rgba(46, 160, 67, 0.55);
}

.phase.off {
  color: #f85149;
  border-color: rgba(248, 81, 73, 0.45);
}
</style>
