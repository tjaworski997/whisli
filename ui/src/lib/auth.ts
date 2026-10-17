import { useSyncExternalStore } from "react"

/** "unknown" until the first check; the panel shows the login screen only when signed out. */
export type AuthState = "unknown" | "signedIn" | "signedOut"

let state: AuthState = "unknown"
const listeners = new Set<() => void>()

function publish(next: AuthState) {
  if (state === next) return
  state = next
  listeners.forEach(listener => listener())
}

export const signedIn = () => publish("signedIn")
export const signedOut = () => publish("signedOut")

export function useAuth() {
  return useSyncExternalStore(listener => { listeners.add(listener); return () => listeners.delete(listener) }, () => state)
}

/** Asks the controller whether this browser is logged in. Offline counts as signed in: the app shows its own "unreachable" state. */
export async function checkAuth() {
  try {
    const response = await fetch("/api/v1/auth", { cache: "no-store", headers: { Authorization: "Session" } })
    publish(response.status === 401 ? "signedOut" : "signedIn")
  } catch {
    publish("signedIn")
  }
}
