<template>
  <div class="shell" :class="mode">
    <nav class="nav">
      <div class="mode-tabs" role="tablist" aria-label="Push or Pull">
        <RouterLink
          :to="pushHome"
          class="mode-tab push"
          :class="{ active: mode === 'push' }"
          role="tab"
          :aria-selected="mode === 'push'"
        >Push</RouterLink>
        <RouterLink
          :to="pullHome"
          class="mode-tab pull"
          :class="{ active: mode === 'pull' }"
          role="tab"
          :aria-selected="mode === 'pull'"
        >Pull</RouterLink>
      </div>
      <div class="proto-row">
        <span class="proto-label">{{ mode === 'push' ? 'Publish with' : 'Play with' }}</span>
        <div class="proto-tabs" role="tablist" aria-label="Protocol">
          <template v-if="mode === 'push'">
            <RouterLink to="/moq" class="proto">MoQ</RouterLink>
            <RouterLink to="/flv" class="proto">WT FLV</RouterLink>
          </template>
          <template v-else>
            <RouterLink to="/moq-pull" class="proto">MoQ</RouterLink>
            <RouterLink to="/pull" class="proto">WT FLV</RouterLink>
            <RouterLink to="/http-flv" class="proto">HTTP-FLV</RouterLink>
          </template>
        </div>
      </div>
    </nav>
    <RouterView />
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import { useRoute } from 'vue-router'

const route = useRoute()

const mode = computed(() =>
  route.path === '/moq' || route.path === '/flv' ? 'push' : 'pull',
)

const pushHome = computed(() => {
  if (route.path === '/moq-pull') return '/moq'
  if (route.path === '/pull' || route.path === '/http-flv') return '/flv'
  return route.path === '/flv' ? '/flv' : '/moq'
})

const pullHome = computed(() => {
  if (route.path === '/flv') return '/pull'
  if (route.path === '/moq') return '/moq-pull'
  if (route.path === '/http-flv') return '/http-flv'
  return route.path === '/pull' ? '/pull' : '/moq-pull'
})
</script>

<style scoped>
.shell {
  padding-top: 16px;
}

.nav {
  max-width: 960px;
  margin: 0 auto 16px;
  padding: 0 20px;
  display: flex;
  flex-direction: column;
  gap: 12px;
}

.mode-tabs {
  display: flex;
  gap: 8px;
}

.mode-tab {
  min-width: 96px;
  padding: 8px 18px;
  border-radius: 8px;
  border: 1px solid #30363d;
  background: #161b22;
  color: #8b949e;
  text-decoration: none;
  text-align: center;
  font-size: 16px;
  font-weight: 700;
  letter-spacing: 0.04em;
}

.mode-tab.push.active {
  color: #fff;
  background: #9a3412;
  border-color: #c2410c;
}

.mode-tab.pull.active {
  color: #fff;
  background: #0f4c5c;
  border-color: #0e7490;
}

.proto-row {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 10px 14px;
}

.proto-label {
  font-size: 13px;
  color: #8b949e;
}

.proto-tabs {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
}

.proto {
  padding: 5px 12px;
  border-radius: 999px;
  border: 1px solid #30363d;
  color: #8b949e;
  text-decoration: none;
  font-size: 13px;
}

.proto.router-link-exact-active {
  color: #e6edf3;
  font-weight: 600;
  border-color: #6e7681;
  background: #21262d;
}

.shell.push .proto.router-link-exact-active {
  border-color: #c2410c;
}

.shell.pull .proto.router-link-exact-active {
  border-color: #0e7490;
}
</style>
