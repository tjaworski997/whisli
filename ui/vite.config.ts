import path from "node:path"
import tailwindcss from "@tailwindcss/vite"
import react from "@vitejs/plugin-react"
import { defineConfig, loadEnv } from "vite"

const now = new Date()
const generatedVersion = `0.002.${now.getFullYear()}${String(now.getMonth() + 1).padStart(2, "0")}${String(now.getDate()).padStart(2, "0")}${String(now.getHours()).padStart(2, "0")}${String(now.getMinutes()).padStart(2, "0")}`
const version = process.env.WHISLI_VERSION ?? generatedVersion

export default defineConfig(({ mode }) => {
  const env = loadEnv(mode, process.cwd(), "")
  const deviceUrl = env.WHISLI_DEVICE_URL
  // The panel logs in itself (session cookie through the proxy). Optionally the proxy can log every
  // request in with HTTP Basic instead ("user:password"), which also skips the login screen.
  const deviceAuth = env.WHISLI_DEVICE_AUTH
  const authHeaders = deviceAuth ? { Authorization: `Basic ${Buffer.from(deviceAuth).toString("base64")}` } : undefined

  if (!deviceUrl) console.warn("WHISLI_DEVICE_URL nieustawiony — lokalny panel nie będzie połączony ze sterownikiem.")

  const proxy = deviceUrl ? {
    "/api": { target: deviceUrl, changeOrigin: true, ws: true, headers: authHeaders },
  } : undefined

  return {
    base: "/",
    define: { __WHISLI_VERSION__: JSON.stringify(version) },
    plugins: [react(), tailwindcss()],
    resolve: { alias: { "@": path.resolve(import.meta.dirname, "./src") } },
    server: { proxy },
    build: { outDir: "../data/www", emptyOutDir: true },
  }
})
