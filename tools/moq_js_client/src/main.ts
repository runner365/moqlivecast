import { createApp } from 'vue'
import App from './App.vue'
import { router } from './router'
import { get_log_level, log_info } from './logger'
import './style.css'

log_info('app', `moq-js-client start log=${get_log_level()}`)
createApp(App).use(router).mount('#app')
