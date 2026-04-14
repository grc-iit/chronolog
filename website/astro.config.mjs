// @ts-check
import { defineConfig } from 'astro/config';
import tailwindcss from '@tailwindcss/vite';
import sitemap from '@astrojs/sitemap';

export default defineConfig({
  site: 'https://chronolog.dev',
  server: {
    host: true,   // 0.0.0.0 - accessible on the network
    port: 3013
  },
  integrations: [sitemap()],
  vite: {
    plugins: [tailwindcss()]
  }
});
