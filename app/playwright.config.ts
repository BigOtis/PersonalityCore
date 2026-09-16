import { defineConfig } from "@playwright/test";
export default defineConfig({
  testDir: "./tests",
  timeout: 30000,
  workers: 1,
  use: { baseURL: "http://127.0.0.1:8876", viewport: { width: 1360, height: 860 }, trace: "retain-on-failure",
    launchOptions: { args: ["--use-fake-device-for-media-stream", "--use-fake-ui-for-media-stream"] } },
  webServer: { command: `"${process.env.LOCALTALKER_TEST_PYTHON || '..\\.venv\\Scripts\\python.exe'}" ../scripts/test_server.py`, url: "http://127.0.0.1:8876/v1/health", reuseExistingServer: false },
});
