import { useState } from "react"

import { useLiveRefresh } from "@/hooks/use-live-refresh"
import { getJson } from "@/lib/api"
import type { Light, LightPreset } from "@/types"

/** Colour and white presets, refreshed when the controller reports a change. */
export function usePresets() {
  const [presets, setPresets] = useState<LightPreset[]>([])
  async function refresh() {
    try { setPresets((await getJson<{ presets: LightPreset[] }>("/api/v1/presets")).presets) } catch { /* keep what we have */ }
  }
  useLiveRefresh(refresh, ["lights"])
  return { presets, refresh }
}

/** Which presets suit a light: whites for CCT lights, colours for RGB and RGBW ones. */
export function presetKindFor(kind: Light["kind"] | undefined): LightPreset["kind"] | null {
  if (kind === "cct") return "temperature"
  if (kind === "rgb" || kind === "rgbw") return "color"
  return null
}
