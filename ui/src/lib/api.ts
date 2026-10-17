import { signedOut } from "@/lib/auth"

export type ApiParams = Record<string, string | number | boolean>
export type ApiResult = { ok: boolean; status: number; message: string; data: Record<string, unknown> }

/**
 * fetch() for the controller API. The session cookie goes along by itself; the explicit
 * Authorization header stops the browser adding an HTTP Basic login it may have cached, which
 * would keep the panel signed in after logging out. A 401 shows the login screen.
 */
export async function apiFetch(path: string, init: RequestInit = {}) {
  const headers = new Headers(init.headers)
  headers.set("Authorization", "Session")
  const response = await fetch(path, { ...init, headers })
  if (response.status === 401 && !path.startsWith("/api/v1/auth/login")) signedOut()
  return response
}

export async function getJson<T>(path: string): Promise<T> {
  const response = await apiFetch(path, { cache: "no-store" })
  if (!response.ok) throw new Error(`${path}: HTTP ${response.status}`)
  return await response.json() as T
}

/** Sends a form-encoded POST. Never throws: network and parse errors resolve to `ok: false`. */
export async function post(path: string, params: ApiParams = {}): Promise<ApiResult> {
  const body = new URLSearchParams(Object.entries(params).map(([key, value]) => [key, String(value)]))
  try {
    const response = await apiFetch(path, { method: "POST", body })
    const data = await response.json().catch(() => ({})) as Record<string, unknown>
    return { ok: response.ok, status: response.status, message: typeof data.message === "string" ? data.message : "", data }
  } catch {
    return { ok: false, status: 0, message: "", data: {} }
  }
}
