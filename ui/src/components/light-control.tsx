import { Power, X } from "lucide-react"
import { useEffect, useState } from "react"

import { ColorTemperaturePicker, kelvinSwatch, kelvinToMix, mixToKelvin } from "@/components/color-temperature-picker"
import { RgbwPicker, toHex } from "@/components/rgbw-picker"
import { Button } from "@/components/ui/button"
import { Dialog } from "@/components/ui/dialog"
import { Slider } from "@/components/ui/slider"
import { presetKindFor, usePresets } from "@/hooks/use-presets"
import { useI18n, type TranslationKey } from "@/i18n"
import { post } from "@/lib/api"
import { cn } from "@/lib/utils"
import type { Light, LightPreset, RgbwColor } from "@/types"

export const isDmxLight = (kind: Light["kind"]) => kind === "dmx" || kind === "cct" || kind === "rgb" || kind === "rgbw"

export const lightKindLabel: Record<Light["kind"], TranslationKey> = {
  none: "lightKindNone", relay: "lightKindRelay", dmx: "lightKindDmx", cct: "lightKindCct", rgb: "lightKindRgb", rgbw: "lightKindRgbw",
}

/** Matches the firmware: a new colour light starts white. */
export const defaultColor = (kind: Light["kind"]): RgbwColor => kind === "rgb" ? { red: 255, green: 255, blue: 255, white: 0 } : { red: 0, green: 0, blue: 0, white: 255 }

/** DMX slots a light uses, e.g. "DMX 10–13". */
export function dmxRange(light: Pick<Light, "kind" | "dmxChannel">) {
  const last = light.dmxChannel + ({ rgbw: 4, rgb: 3, cct: 2 }[light.kind as string] ?? 1) - 1
  return `DMX ${light.dmxChannel}${last > light.dmxChannel ? `–${last}` : ""}`
}

const levelPercent = (level: number) => Math.round(level * 100 / 255)

/** Colour the lamp gives off at full brightness, as a CSS colour. */
export function lampColor(kind: Light["kind"], temperature: number, color: RgbwColor) {
  if (kind === "cct") return kelvinSwatch(mixToKelvin(temperature))
  if (kind === "rgb" || kind === "rgbw") return toHex(color)
  return kelvinSwatch(3000)
}

/** Round indicator of a DMX light: its colour, dimmed with its brightness; an empty ring when off. */
export function LightSwatch({ kind, level, temperature, color, className }: { kind: Light["kind"]; level: number; temperature: number; color: RgbwColor; className?: string }) {
  return <span aria-hidden className={cn("whisli-line inline-block size-4 shrink-0 rounded-full border", className)}
    style={level > 0 ? { background: lampColor(kind, temperature, color), opacity: 0.35 + 0.65 * level / 255, borderColor: "transparent" } : undefined} />
}

/**
 * Everyday control of one DMX light: on/off, brightness and colour, applied live.
 * Self-contained (talks to the API itself), so any page can open it.
 */
export function LightControlDialog({ light, onClose, onChanged }: { light: Light; onClose: () => void; onChanged: () => void }) {
  const { t } = useI18n()
  const [level, setLevel] = useState(light.level)
  const [temperature, setTemperature] = useState(light.colorTemperature ?? 50)
  const [color, setColor] = useState<RgbwColor>(light.color ?? defaultColor(light.kind))
  // Follow the controller (other panels, buttons, "on" picking the level) when it reports a change.
  useEffect(() => setLevel(light.level), [light.level])
  useEffect(() => setTemperature(light.colorTemperature ?? 50), [light.colorTemperature])
  useEffect(() => { if (light.color) setColor(light.color) }, [light.color])

  const usesColor = light.kind === "rgb" || light.kind === "rgbw"
  const on = level > 0
  const { presets } = usePresets()
  const presetKind = presetKindFor(light.kind)
  const suitable = presets.filter(preset => preset.kind === presetKind)
  // A preset only changes the white or colour: brightness (and off) stays.
  async function applyPreset(preset: LightPreset) {
    if (preset.kind === "temperature") setTemperature(kelvinToMix(preset.kelvin))
    else setColor(preset.color)
    await post("/api/v1/presets/apply", { id: preset.id, light: light.id })
    onChanged()
  }
  async function apply(next: number | "on", nextTemperature = temperature, nextColor = color) {
    if (next !== "on") setLevel(next)
    await post("/api/v1/lights/level", {
      id: light.id, level: next,
      ...(next !== "on" && light.kind === "cct" ? { temperature: nextTemperature } : {}),
      ...(next !== "on" && usesColor ? nextColor : {}),
    })
    onChanged()
  }

  return <Dialog open onClose={onClose} labelledBy="light-control-title" className="w-[min(30rem,calc(100%-2rem))]">
    <div className="flex items-center gap-3">
      <span className="whisli-raised flex size-10 shrink-0 items-center justify-center rounded-full">
        <LightSwatch kind={light.kind} level={level} temperature={temperature} color={color} className="size-5" />
      </span>
      <div className="min-w-0 flex-1">
        <h2 id="light-control-title" className="truncate font-semibold">{light.name}</h2>
        <p className="whisli-muted text-xs">{t(lightKindLabel[light.kind])} · {dmxRange(light)}</p>
      </div>
      <Button variant="ghost" size="icon" aria-label={t("close")} title={t("close")} onClick={onClose}><X className="size-4" /></Button>
    </div>
    <div className="whisli-raised mt-5 flex items-center justify-between gap-3 rounded-md p-3">
      <span className="text-sm">{on ? t("lightStateOnLevel", { percent: levelPercent(level) }) : t("lightStateOff")}</span>
      <Button variant={on ? "outline" : "default"} onClick={() => void apply(on ? 0 : "on")}>
        <Power className={cn("size-4", on && "text-amber-500")} />{t(on ? "actionTurnOff" : "actionTurnOn")}
      </Button>
    </div>
    <div className="mt-5 grid gap-5">
      <Slider value={[level]} min={0} max={255} step={1} label={t("lightBrightness")} valueLabel={`${levelPercent(level)}%`}
        onValueChange={value => setLevel(value[0] ?? 0)} onValueCommitted={value => void apply(value[0] ?? 0)} />
      {light.kind === "cct" && <ColorTemperaturePicker value={temperature} onChange={setTemperature} onCommit={mix => void apply(level, mix)} />}
      {usesColor && <RgbwPicker value={color} hasWhite={light.kind === "rgbw"} onChange={setColor} onCommit={next => void apply(level, temperature, next)} />}
      {suitable.length > 0 && <div className="flex flex-wrap gap-1.5" role="group" aria-label={t("presetsTitle")}>
        {suitable.map(preset => <button key={preset.id} type="button" onClick={() => void applyPreset(preset)}
          className="whisli-line inline-flex items-center gap-1.5 rounded-full border px-2.5 py-1 text-xs transition-colors hover:border-[var(--ink)]">
          <span className="whisli-line size-3 rounded-full border" style={{ backgroundColor: preset.kind === "temperature" ? kelvinSwatch(preset.kelvin) : toHex(preset.color) }} aria-hidden />
          {preset.name}
        </button>)}
      </div>}
    </div>
  </Dialog>
}
