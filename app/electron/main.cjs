const { app, BrowserWindow, shell, dialog, session } = require("electron");
const { spawn } = require("child_process");
const path = require("path");
const fs = require("fs");
const http = require("http");
const PORT = process.env.LOCALTALKER_PORT || "8765";
const BASE = `http://127.0.0.1:${PORT}`;
const root = path.resolve(__dirname, "..", "..");
const legacyUserData = app.getPath("userData");
app.setName("PersonalityCore");
app.setPath("userData", legacyUserData);
if (process.platform === "win32") app.setAppUserModelId("com.robotfuture.personalitycore");
let runtime = null;
let quitting = false;
let startupError = "";

function healthy() {
  return new Promise(resolve => {
    const req = http.get(`${BASE}/v1/health`, res => {
      let data = "";
      res.on("data", d => data += d);
      res.on("end", () => { try { resolve(res.statusCode === 200 && JSON.parse(data).ok === true); } catch { resolve(false); } });
    });
    req.setTimeout(1500, () => req.destroy());
    req.on("error", () => resolve(false));
  });
}
function startRuntime() {
  const bundled = path.join(process.resourcesPath, "runtime", "localtalker-runtime.exe");
  const venv = path.join(root, ".venv", process.platform === "win32" ? "Scripts/python.exe" : "bin/python");
  let command = process.env.LOCALTALKER_PYTHON || (fs.existsSync(venv) ? venv : process.platform === "win32" ? "py" : "python3");
  let args = [...(command === "py" ? ["-3.11"] : []), "-m", "localtalker", "serve", "--host", "127.0.0.1", "--port", String(PORT)];
  if (app.isPackaged && fs.existsSync(bundled)) { command = bundled; args = ["serve", "--host", "127.0.0.1", "--port", String(PORT)]; }
  const dataDir = process.env.LOCALTALKER_HOME || "";
  if (dataDir) args.push("--data-dir", dataDir);
  const env = { ...process.env, PYTHONPATH: path.join(root, "src"), PYTHONUNBUFFERED: "1" };
  runtime = spawn(command, args, {cwd: root, env, stdio: ["ignore", "pipe", "pipe"], windowsHide: true});
  const logPath = path.join(app.getPath("userData"), "runtime.log");
  fs.mkdirSync(path.dirname(logPath), {recursive: true});
  const output = fs.createWriteStream(logPath, {flags: "a"});
  runtime.stdout.pipe(output, {end: false});
  runtime.stderr.on("data", data => { startupError = (startupError + data.toString()).slice(-3000); output.write(data); });
  runtime.on("error", error => { startupError = error.message; });
  runtime.on("exit", code => {
    output.end();
    if (!quitting && code) dialog.showErrorBox("PersonalityCore runtime stopped", `${startupError}\n\nRestart PersonalityCore to reconnect. Log: ${logPath}`);
  });
}
async function createWindow() {
  const window = new BrowserWindow({width: 1360, height: 860, minWidth: 700, minHeight: 600,
    backgroundColor: "#12110f", title: "PersonalityCore", icon: path.join(__dirname, "personalitycore.png"), show: false,
    webPreferences: { preload: path.join(__dirname, "preload.cjs"), contextIsolation: true, nodeIntegration: false, sandbox: true },
  });
  window.removeMenu();
  const target = process.env.LOCALTALKER_DEV_URL || BASE;
  const origin = new URL(target).origin;
  window.webContents.setWindowOpenHandler(({url}) => {
    if (/^https?:/.test(url)) void shell.openExternal(url);
    return {action: "deny"};
  });
  window.webContents.on("will-navigate", (event, url) => { if (new URL(url).origin !== origin) event.preventDefault(); });
  session.defaultSession.setPermissionRequestHandler((contents, permission, callback, details) => {
    callback(contents === window.webContents && new URL(details.requestingUrl || target).origin === origin && permission === "media");
  });
  session.defaultSession.setPermissionCheckHandler((contents, permission, requestingOrigin) => contents === window.webContents && requestingOrigin === origin && permission === "media");
  await window.loadURL(target);
  window.show();
}
if (!app.requestSingleInstanceLock()) app.quit();
else {
  app.on("second-instance", () => { const win = BrowserWindow.getAllWindows()[0]; if (win) {win.restore(); win.focus();} });
  app.whenReady().then(async () => {
    try {
      if (!(await healthy())) startRuntime();
      let ready = false;
      for (let i = 0; i < 100; i++) { if (await healthy()) {ready = true; break;} await new Promise(r => setTimeout(r, 300)); }
      if (!ready) throw new Error(`Runtime did not start.\n${startupError}\n\nRun scripts/setup.ps1 to install dependencies.`);
      await createWindow();
    } catch (error) { dialog.showErrorBox("PersonalityCore could not start", error.message); app.quit(); }
  });
}
app.on("before-quit", () => {
  quitting = true;
  if (runtime && runtime.exitCode === null) {
    // Stop only our process tree, including a managed llama-server on Windows.
    if (process.platform === "win32") spawn("taskkill", ["/pid", String(runtime.pid), "/T", "/F"], {windowsHide: true});
    else runtime.kill();
  }
});
app.on("window-all-closed", () => app.quit());
