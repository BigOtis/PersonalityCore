import { test, expect } from "@playwright/test";

test("conversation streams to the UI and audio, survives reload, and switches characters", async ({ page }) => {
  const errors: string[] = [];
  page.on("pageerror", e => errors.push(e.message));
  await page.addInitScript(() => {
    (window as any).playedBuffers = 0;
    const start = AudioBufferSourceNode.prototype.start;
    AudioBufferSourceNode.prototype.start = function(...args: Parameters<typeof start>) {
      (window as any).playedBuffers++;
      return start.apply(this, args);
    };
  });
  const events: string[] = [];
  page.on("websocket", ws => ws.on("framereceived", frame => events.push(JSON.parse(String(frame.payload)).type)));
  await page.goto("/");
  await expect(page.getByText("Ready", {exact: true})).toBeVisible();
  await page.getByLabel("Message", {exact: true}).fill("A room by the river, please.");
  await page.getByRole("button", {name: "Send", exact: true}).click();
  await expect(page.locator(".line.assistant")).toContainText("A room by the river, please.");
  await expect.poll(() => page.evaluate(() => (window as any).playedBuffers)).toBeGreaterThan(0);
  expect(events).toEqual(expect.arrayContaining(["token", "dialogue", "audio", "reply"]));
  await page.reload();
  await expect(page.locator(".line.assistant")).toContainText("A room by the river, please.");
  await page.getByRole("button", {name: /Rook/}).first().click();
  await expect(page.locator(".character-heading h2")).toHaveText("Rook");
  await expect(page.locator(".line")).toHaveCount(0);
  await page.getByRole("button", {name: /Mira/}).first().click();
  await expect(page.locator(".line.assistant")).toContainText("A room by the river, please.");
  await page.screenshot({path: "test-results/studio.png", animations: "disabled"});
  expect(errors).toEqual([]);
});

test("character editing, context injection, and model selection are usable", async ({ page }) => {
  await page.goto("/");
  await expect(page.getByText("Ready", {exact: true})).toBeVisible();
  await page.getByRole("button", {name: "New character", exact: true}).click();
  await page.getByLabel("Name", {exact: true}).fill("Nox");
  await page.getByLabel("Personality").fill("A patient lighthouse keeper.");
  await page.getByRole("button", {name: "Save", exact: true}).click();
  await expect(page.getByRole("dialog")).toHaveCount(0);
  await expect(page.locator(".character-heading h2")).toHaveText("Nox");
  await page.getByRole("button", {name: "Inspect", exact: true}).click();
  await page.getByRole("button", {name: "context", exact: true}).click();
  await page.getByLabel("Game context", {exact: true}).fill('{"weather":"fog","inventory":["lantern"]}');
  await page.getByRole("button", {name: "Inject context"}).click();
  await page.getByRole("button", {name: "setup", exact: true}).click();
  await page.getByLabel("Provider", {exact: true}).selectOption("mock");
  await page.getByRole("button", {name: "Apply model"}).click();
  await expect(page.locator(".model-badge")).toContainText("Demo");
  await page.getByRole("button", {name: "Edit Nox"}).click();
  await page.getByLabel("Voice", {exact: true}).selectOption("af_bella");
  await page.getByRole("button", {name: "Save", exact: true}).click();
  await page.reload();
  await page.getByRole("button", {name: /Nox/}).first().click();
  await page.getByRole("button", {name: "Inspect", exact: true}).click();
  await page.getByRole("button", {name: "context", exact: true}).click();
  await expect(page.getByLabel("Game context", {exact: true})).toHaveValue(/lantern/);
});

test("microphone capture travels through transcription and conversation", async ({ page }) => {
  await page.goto("/");
  await expect(page.getByText("Ready", {exact: true})).toBeVisible();
  const mic = page.getByRole("button", {name: "Microphone", exact: true});
  await mic.hover();
  await page.mouse.down();
  await expect(page.getByRole("meter", {name: "Microphone level"})).toBeVisible();
  // Allow real AudioWorklet callbacks to accumulate samples from Chromium's fake device.
  await page.waitForTimeout(1000);
  await page.mouse.up();
  await expect(page.locator(".line.user").last()).toContainText("hello there");
  await expect(page.locator(".line.assistant").last()).toContainText("hello there");
});

test("group chat lets multiple characters answer the player", async ({ page }) => {
  await page.goto("/");
  await expect(page.getByText("Ready", { exact: true })).toBeVisible();
  await page.getByRole("button", { name: "New group chat", exact: true }).click();
  const dialog = page.getByRole("dialog", { name: "New group chat" });
  await dialog.getByLabel("Mira").check();
  await dialog.getByLabel("Rook").check();
  await dialog.getByRole("button", { name: "Start group chat", exact: true }).click();
  await expect(page.locator(".character-heading h2")).toContainText("Mira");
  await expect(page.locator(".character-heading h2")).toContainText("Rook");
  await page.getByLabel("Message", { exact: true }).fill("Hello everyone, what should we do?");
  await page.getByRole("button", { name: "Send", exact: true }).click();
  await expect(page.locator(".line.user")).toContainText("Hello everyone, what should we do?");
  await expect(page.locator(".line.assistant")).toHaveCount(2, { timeout: 20000 });
  await expect(page.locator(".line.assistant .who").first()).toHaveText(/Mira|Rook/);
  await expect(page.locator(".line.assistant .who").nth(1)).toHaveText(/Mira|Rook/);
});
