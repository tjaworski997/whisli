import { useEffect, useState } from "react"

export type Page = "home" | "devices" | "deviceTypes" | "device" | "lights" | "lightSettings" | "lightPresets" | "actions" | "zones" | "map" | "network" | "mqtt" | "test" | "system" | "logs"
/** Actions page narrowed to one button (`input:<id>`) or to what drives one target (`target:<id>`). */
export type ActionsFilter = `input:${string}` | `target:${string}`
/** A row to point out on the Actions page: `event-<id>` or `action-<id>`. */
export type ActionsHighlight = `event-${number}` | `action-${number}`
export type AppRoute = { page: Page; deviceId?: string; filter?: ActionsFilter; highlight?: ActionsHighlight; lightId?: number }

// "logs" stays here so old #/logs links still open the log (now at #/system/logs).
const simplePages: Page[] = ["home", "devices", "lights", "actions", "zones", "map", "logs", "system"]

function parseRoute(hash = window.location.hash): AppRoute {
  const [section, detail] = hash.replace(/^#\/?/, "").split("/")
  if (section === "devices" && detail === "add") return { page: "deviceTypes" }
  if (section === "lights" && detail === "settings") return { page: "lightSettings" }
  if (section === "lights" && detail === "presets") return { page: "lightPresets" }
  if (section === "system" && detail === "network") return { page: "network" }
  if (section === "system" && detail === "mqtt") return { page: "mqtt" }
  if (section === "system" && detail === "logs") return { page: "logs" }
  if (section === "system" && detail === "test") return { page: "test" }
  if (section === "lights" && /^\d+$/.test(detail ?? "")) return { page: "lights", lightId: Number(detail) }
  if (section === "actions" && detail) {
    const target = hash.match(/^#\/?actions\/target\/(.+)$/)?.[1]
    if (target) return { page: "actions", filter: `target:${decodeURIComponent(target)}` }
    const highlight = hash.split("/")[3]
    return { page: "actions", filter: `input:${decodeURIComponent(detail)}`, highlight: /^(event|action)-\d+$/.test(highlight ?? "") ? highlight as ActionsHighlight : undefined }
  }
  if (section === "device" && (detail === "controller" || /^relay-\d+$/.test(detail ?? ""))) return { page: "device", deviceId: detail }
  if (simplePages.includes(section as Page)) return { page: section as Page }
  return { page: "home" }
}

/** The `#/…` address of a route, for links that can open in a new tab. */
export function routeHash(route: AppRoute) {
  if (route.page === "deviceTypes") return "#/devices/add"
  if (route.page === "device" && route.deviceId) return `#/device/${route.deviceId}`
  if (route.page === "lights" && route.lightId) return `#/lights/${route.lightId}`
  if (route.page === "lightSettings") return "#/lights/settings"
  if (route.page === "lightPresets") return "#/lights/presets"
  if (route.page === "network") return "#/system/network"
  if (route.page === "mqtt") return "#/system/mqtt"
  if (route.page === "logs") return "#/system/logs"
  if (route.page === "test") return "#/system/test"
  if (route.page === "actions" && route.filter?.startsWith("input:")) return `#/actions/${encodeURIComponent(route.filter.slice(6))}${route.highlight ? `/${route.highlight}` : ""}`
  if (route.page === "actions" && route.filter?.startsWith("target:")) return `#/actions/target/${encodeURIComponent(route.filter.slice(7))}`
  return `#/${route.page}`
}

export function useRoute() {
  const [route, setRoute] = useState<AppRoute>(() => parseRoute())

  function navigate(next: AppRoute, replace = false) {
    if (!replace && route.page === next.page && route.deviceId === next.deviceId && route.filter === next.filter && route.highlight === next.highlight && route.lightId === next.lightId) return
    const url = `${window.location.pathname}${window.location.search}${routeHash(next)}`
    if (replace) window.history.replaceState({ whisliRoute: true }, "", url)
    else window.history.pushState({ whisliRoute: true }, "", url)
    setRoute(next)
  }

  function goBack(fallback: AppRoute) {
    if (window.history.state?.whisliRoute && window.history.length > 1) window.history.back()
    else navigate(fallback)
  }

  useEffect(() => {
    const sync = () => setRoute(parseRoute())
    window.addEventListener("popstate", sync)
    window.addEventListener("hashchange", sync)
    if (!window.location.hash) navigate({ page: "home" }, true)
    return () => {
      window.removeEventListener("popstate", sync)
      window.removeEventListener("hashchange", sync)
    }
  }, [])

  return { route, navigate, goBack }
}
