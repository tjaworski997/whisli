import { useEffect, useRef, useSyncExternalStore } from "react"

import { usePolling } from "@/hooks/use-polling"
import { getJson } from "@/lib/api"

/** What the controller reports as changed; pages subscribe to the topics they display. */
export type LiveTopic = "devices" | "lights" | "actions" | "zones"

/**
 * One shared WebSocket to /api/v1/ws. The client first sends the token from /api/v1/ws/token (an
 * authenticated request), then the controller pushes `{"type":"changed","topics":[...]}`
 * whenever something changes, plus a heartbeat every 20 s. Each topic has a local counter that moves
 * on every push, so a page reloads only the data it shows. The socket opens while anything is
 * subscribed and reconnects with a growing delay when it drops.
 */
type LiveState = { connected: boolean; counters: Record<LiveTopic, number> }
const topics: LiveTopic[] = ["devices", "lights", "actions", "zones"]
const silenceLimitMs = 45000  // > heartbeat interval: no message this long means the link is dead

let state: LiveState = { connected: false, counters: { devices: 0, lights: 0, actions: 0, zones: 0 } }
const listeners = new Set<() => void>()

/** A button gesture as it happens (`{"type":"input"}` frames), for live views such as the map. */
export type LiveInputEvent = { input: string; trigger: string }
const inputListeners = new Set<(event: LiveInputEvent) => void>()

/** Calls `listener` for every gesture pushed by the controller. Keep a useLiveRefresh subscriber alive meanwhile. */
export function subscribeInputEvents(listener: (event: LiveInputEvent) => void) {
  inputListeners.add(listener)
  return () => { inputListeners.delete(listener) }
}
let socket: WebSocket | null = null
let retryDelayMs = 1000
let retryTimer: number | undefined
let silenceTimer: number | undefined

function publish(next: LiveState) {
  state = next
  listeners.forEach(listener => listener())
}

function bump(changed: LiveTopic[]) {
  const counters = { ...state.counters }
  changed.forEach(topic => { counters[topic] += 1 })
  publish({ ...state, counters })
}

function watchSilence() {
  window.clearTimeout(silenceTimer)
  silenceTimer = window.setTimeout(() => socket?.close(), silenceLimitMs)
}

function scheduleReconnect() {
  if (listeners.size === 0) return
  retryTimer = window.setTimeout(() => void connect(), retryDelayMs)
  retryDelayMs = Math.min(retryDelayMs * 2, 15000)
}

async function connect() {
  let token: string
  try {
    token = (await getJson<{ token: string }>("/api/v1/ws/token")).token
  } catch {
    scheduleReconnect()  // offline, or firmware without push: callers keep polling meanwhile
    return
  }
  if (listeners.size === 0 || socket) return
  const protocol = location.protocol === "https:" ? "wss" : "ws"
  const current = new WebSocket(`${protocol}://${location.host}/api/v1/ws`)
  socket = current
  current.onopen = () => {
    current.send(token)
    retryDelayMs = 1000
    watchSilence()
    // Switching from fallback polling to push restarts usePolling, which reloads everything once.
    publish({ ...state, connected: true })
  }
  current.onmessage = event => {
    watchSilence()
    try {
      const message = JSON.parse(String(event.data)) as { type?: string; topics?: string[]; input?: string; trigger?: string }
      if (message.type === "input" && message.input && message.trigger) {
        const gesture = { input: message.input, trigger: message.trigger }
        inputListeners.forEach(listener => listener(gesture))
        return
      }
      if (message.type !== "changed") return
      const changed = (message.topics ?? []).filter((topic): topic is LiveTopic => topics.includes(topic as LiveTopic))
      if (changed.length) bump(changed)
    } catch {
      // Ignore malformed frames.
    }
  }
  current.onclose = () => {
    window.clearTimeout(silenceTimer)
    if (socket !== current) return
    socket = null
    if (state.connected) publish({ ...state, connected: false })
    scheduleReconnect()
  }
}

function subscribe(listener: () => void) {
  listeners.add(listener)
  if (listeners.size === 1 && !socket) void connect()
  return () => {
    listeners.delete(listener)
    if (listeners.size > 0) return
    window.clearTimeout(retryTimer)
    const current = socket
    socket = null
    current?.close()
  }
}

const getSnapshot = () => state

/**
 * Runs `callback` on mount and whenever the controller pushes a change for one of `watched`.
 * While the WebSocket is down (or the firmware has none) it falls back to polling every 1.5 s;
 * while it is up, a slow 30 s poll is the only safety net.
 */
export function useLiveRefresh(callback: () => unknown, watched: LiveTopic[], enabled = true) {
  const { connected, counters } = useSyncExternalStore(enabled ? subscribe : noopSubscribe, getSnapshot)
  const callbackRef = useRef(callback)
  useEffect(() => { callbackRef.current = callback })

  // One number per subscriber: the sum of the counters it watches.
  const version = watched.reduce((sum, topic) => sum + counters[topic], 0)
  const seen = useRef(version)
  const running = useRef(false)
  const pending = useRef(false)
  useEffect(() => {
    if (!enabled || version === seen.current) return
    seen.current = version
    // Re-run once more if a change arrives while a refresh is still in flight.
    const run = async () => {
      if (running.current) { pending.current = true; return }
      running.current = true
      try { await callbackRef.current() } finally { running.current = false }
      if (pending.current) { pending.current = false; void run() }
    }
    void run()
  }, [version, enabled])

  usePolling(callback, connected ? 30000 : 1500, enabled)
}

const noopSubscribe = () => () => {}
