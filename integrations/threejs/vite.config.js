import { defineConfig } from 'vite';

// HTTP and WebSocket share an origin in the browser. Inference stays in the
// separately running LocalTalker process; no model is embedded in Three.js.
export default defineConfig({
  server: { proxy: { '/v1': { target: process.env.LOCALTALKER_URL || 'http://127.0.0.1:8765', ws: true } } },
});
