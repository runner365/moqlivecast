import { createRouter, createWebHistory } from 'vue-router'
import PushView from './views/PushView.vue'
import PullView from './views/PullView.vue'
import MoqPushView from './views/MoqPushView.vue'
import MoqPullView from './views/MoqPullView.vue'
import HttpFlvPullView from './views/HttpFlvPullView.vue'

export const router = createRouter({
  history: createWebHistory(),
  routes: [
    { path: '/', redirect: '/moq' },
    { path: '/moq', component: MoqPushView },
    { path: '/moq-pull', component: MoqPullView },
    { path: '/flv', component: PushView },
    { path: '/pull', component: PullView },
    { path: '/http-flv', component: HttpFlvPullView },
  ],
})
