import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'

export default defineConfig({
  plugins: [vue()],
  server: {
    // Vid lokal utveckling (npm run dev) skickas API-anrop vidare till backend-containern.
    proxy: {
      '/api': 'http://localhost:8080',
    },
  },
})
