const { contextBridge } = require("electron");

contextBridge.exposeInMainWorld("localtalker", {
  desktop: true,
});
