// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Configures the desktop webview's Vite development server and production assets.
import { defineConfig } from "vite";

export default defineConfig({
  clearScreen: false,
  server: { host: "127.0.0.1", port: 1420, strictPort: true },
});
