<template>
  <div class="stats" aria-label="media stats">
    <div><span class="k">fps</span>{{ fmt(stats.videoFps, 1) }}</div>
    <div><span class="k">V</span>{{ fmt(stats.videoKbps, 0) }} <span class="u">kbps</span></div>
    <div><span class="k">A</span>{{ fmt(stats.audioKbps, 0) }} <span class="u">kbps</span></div>
    <div><span class="k">GOP</span>{{ gop }}</div>
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import type { MediaStatsSnapshot } from '../media/mediaStats'

const props = defineProps<{
  stats: MediaStatsSnapshot
}>()

function fmt(n: number | null, digits: number): string {
  if (n == null || !Number.isFinite(n)) return '--'
  return n.toFixed(digits)
}

const gop = computed(() => {
  if (props.stats.gopSec == null || !Number.isFinite(props.stats.gopSec)) return '--'
  return `${props.stats.gopSec.toFixed(1)} s`
})
</script>

<style scoped>
.stats {
  position: absolute;
  right: 10px;
  bottom: 10px;
  min-width: 118px;
  padding: 8px 10px;
  border-radius: 8px;
  background: rgba(1, 4, 9, 0.72);
  border: 1px solid rgba(48, 54, 61, 0.9);
  color: #e6edf3;
  font-size: 12px;
  line-height: 1.55;
  font-variant-numeric: tabular-nums;
  pointer-events: none;
  backdrop-filter: blur(6px);
}

.k {
  display: inline-block;
  width: 32px;
  color: #8b949e;
}

.u {
  color: #8b949e;
}
</style>
