import react from "@vitejs/plugin-react";
import { defineConfig } from "vitest/config";

// od_web listens on :8090 inside WSL; WSL2 forwards localhost, so the dev server
// on Windows can proxy to it directly.
const OD_WEB = process.env.OD_WEB ?? "localhost:8090";

export default defineConfig({
  plugins: [react()],
  server: {
    port: 5173,
    proxy: {
      "/ws": { target: `ws://${OD_WEB}`, ws: true },
      "/api": { target: `http://${OD_WEB}` },
    },
  },
  build: {
    rollupOptions: {
      output: { manualChunks: { echarts: ["echarts", "zrender"], react: ["react", "react-dom", "react-router-dom"] } },
    },
  },
  test: {
    include: ["src/**/*.test.ts"],
  },
});
