import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'

// 前端页面本身用 http 即可：127.0.0.1/localhost 是 secure context 例外，
// 页面里可以调用 WebTransport API。
// WebTransport 连接目标（wt_chat_server）必须是 https（HTTP/3 over QUIC 需 TLS），
// 那个 URL 由 App.vue 里配置，不在这里。
export default defineConfig({
  plugins: [vue()],
  server: {
    host: '127.0.0.1',
    port: 5173,
  },
})
