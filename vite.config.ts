import react from "@vitejs/plugin-react";
import { fileURLToPath, URL } from "node:url";
import { defineConfig } from "vite";

export default defineConfig({
  root: "apps/mico",
  plugins: [react()],
  resolve: {
    alias: {
      "@mico/render-core": fileURLToPath(new URL("./packages/render-core/src/index.ts", import.meta.url)),
      "@mico/render-dom": fileURLToPath(new URL("./packages/render-dom/src/index.ts", import.meta.url)),
      "@mico/render-dom/styles.css": fileURLToPath(new URL("./packages/render-dom/src/styles.css", import.meta.url))
    }
  },
  server: {
    host: "127.0.0.1",
    port: 5173,
    strictPort: false
  },
  build: {
    outDir: "../../dist/app",
    emptyOutDir: true
  }
});
