import { useState } from "react"

import { useLiveRefresh } from "@/hooks/use-live-refresh"
import { getJson, post } from "@/lib/api"
import type { Zone, ZonesResponse } from "@/types"

export type ZonesApi = {
  zones: Zone[]
  /** Thing (by API id) → zone id. */
  assignments: Record<string, number>
  /** Zone of a thing by its API id ("light-3", "controller.input-2"); 0 = none. */
  zoneOf: (target: string) => number
  zoneName: (id: number) => string
  /** Position in the user's order, for sorting; things without a zone go last. */
  rank: (id: number) => number
  assign: (target: string, zone: number) => Promise<void>
  /** Creates a zone and returns its id (0 on failure). */
  create: (name: string) => Promise<number>
  refresh: () => Promise<void>
}

/** Zones and assignments, kept fresh by the controller's "zones" push. Use once per page and pass down. */
export function useZones(): ZonesApi {
  const [data, setData] = useState<ZonesResponse>({ zones: [], assignments: {} })
  async function refresh() {
    try { setData(await getJson<ZonesResponse>("/api/v1/zones")) } catch { /* keep what we have */ }
  }
  useLiveRefresh(refresh, ["zones"])

  return {
    zones: data.zones,
    assignments: data.assignments,
    zoneOf: target => data.assignments[target] ?? 0,
    zoneName: id => data.zones.find(zone => zone.id === id)?.name ?? "",
    rank: id => { const index = data.zones.findIndex(zone => zone.id === id); return index < 0 ? data.zones.length : index },
    assign: async (target, zone) => {
      setData(current => {
        const assignments = { ...current.assignments }
        if (zone) assignments[target] = zone; else delete assignments[target]
        return { ...current, assignments }
      })
      await post("/api/v1/zones/assign", { target, zone })
      await refresh()
    },
    create: async name => {
      const result = await post("/api/v1/zones", { name })
      await refresh()
      return result.ok ? Number(result.data.id) || 0 : 0
    },
    refresh,
  }
}

/** Groups items by zone in the user's order; items without a zone come last (zone 0). */
export function groupByZone<T>(items: T[], zoneOfItem: (item: T) => number, zones: ZonesApi) {
  const groups = new Map<number, T[]>()
  for (const item of items) {
    const zone = zoneOfItem(item)
    const key = zone && zones.zoneName(zone) ? zone : 0
    groups.set(key, [...(groups.get(key) ?? []), item])
  }
  return [...groups.entries()].sort(([left], [right]) => (left ? zones.rank(left) : Infinity) - (right ? zones.rank(right) : Infinity))
    .map(([zone, members]) => ({ zone, name: zone ? zones.zoneName(zone) : "", items: members }))
}
