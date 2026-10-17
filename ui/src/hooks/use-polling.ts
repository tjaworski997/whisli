import { useEffect, useRef } from "react"

/**
 * Calls `callback` immediately and then every `intervalMs` while `enabled`. Always uses the latest callback.
 * Skips a tick while the previous call is still running (the ESP32 serves one request at a time)
 * and pauses while the tab is hidden, catching up as soon as it becomes visible again.
 */
export function usePolling(callback: () => unknown, intervalMs: number, enabled = true) {
  const callbackRef = useRef(callback)
  useEffect(() => { callbackRef.current = callback })

  useEffect(() => {
    if (!enabled) return
    let running = false
    const tick = async () => {
      if (running || document.hidden) return
      running = true
      try { await callbackRef.current() } finally { running = false }
    }
    void tick()
    const timer = window.setInterval(() => void tick(), intervalMs)
    const onVisibilityChange = () => void tick()
    document.addEventListener("visibilitychange", onVisibilityChange)
    return () => {
      window.clearInterval(timer)
      document.removeEventListener("visibilitychange", onVisibilityChange)
    }
  }, [intervalMs, enabled])
}
