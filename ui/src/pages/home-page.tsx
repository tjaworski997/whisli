import { ArrowUpRight, Boxes, Lightbulb, LightbulbOff, MapPin, Network, Palette, Power, SlidersHorizontal, Zap, type LucideIcon } from "lucide-react"
import { useState } from "react"

import { defaultColor, isDmxLight, lampColor, LightControlDialog } from "@/components/light-control"
import { Muted } from "@/components/page"
import { Button } from "@/components/ui/button"
import { MessageBox } from "@/components/ui/message-box"
import { useLiveRefresh } from "@/hooks/use-live-refresh"
import { usePolling } from "@/hooks/use-polling"
import { usePresets } from "@/hooks/use-presets"
import { groupByZone, useZones } from "@/hooks/use-zones"
import { useI18n } from "@/i18n"
import { getJson, post } from "@/lib/api"
import { formatUptime } from "@/lib/format"
import type { AppRoute } from "@/lib/route"
import { naturalCompare } from "@/lib/sort"
import { cn } from "@/lib/utils"
import type { ActionPanelData, Device, DeviceLogResponse, Light, LightsResponse, SystemInfo } from "@/types"

/** Start: what is on right now, with shortcuts to every section. The first page anyone sees. */
export function HomePage({ devices, onOpen }: { devices: Device[]; onOpen: (route: AppRoute) => void }) {
  const { t } = useI18n()
  const [lights, setLights] = useState<Light[] | null>(null)
  const [panel, setPanel] = useState<ActionPanelData | null>(null)
  const [system, setSystem] = useState<SystemInfo | null>(null)
  const [warnings, setWarnings] = useState(0)
  const [controlled, setControlled] = useState<Light | null>(null)
  // Lights to switch together (all, or one zone), waiting for confirmation.
  const [switching, setSwitching] = useState<{ lights: Light[]; on: boolean; all: boolean } | null>(null)
  const zones = useZones()
  const { presets } = usePresets()

  async function refreshLights() {
    try { setLights((await getJson<LightsResponse>("/api/v1/lights")).lights) } catch { /* keep the last known */ }
  }
  useLiveRefresh(refreshLights, ["lights"])
  useLiveRefresh(async () => { try { setPanel(await getJson<ActionPanelData>("/api/v1/action-panel")) } catch { /* keep */ } }, ["actions"])
  usePolling(async () => {
    try {
      setSystem(await getJson<SystemInfo>("/api/v1/system"))
      const log = await getJson<DeviceLogResponse>("/api/v1/logs?limit=100")
      setWarnings(log.entries.filter(entry => entry.level !== "info").length)
    } catch { /* keep the last known */ }
  }, 15000)

  const configured = (lights ?? []).filter(light => light.kind !== "none")
  const lit = configured.filter(light => light.level > 0)
  const relays = devices.filter(device => device.kind === "relay")
  const offline = relays.filter(device => !device.online).length
  const actions = panel?.actions ?? []
  const activeActions = actions.filter(action => action.enabled && action.command && action.targetId).length
  const controlledNow = controlled && lights?.find(light => light.id === controlled.id)

  // "on" lets each light come on at its own on-level (last brightness or the fixed one).
  async function switchAll(targets: Light[], on: boolean) {
    setSwitching(null)
    for (const light of targets) await post("/api/v1/lights/level", { id: light.id, level: on ? "on" : 0 })
    void refreshLights()
  }
  const dark = configured.filter(light => light.level === 0)
  const toggle = async (light: Light) => { await post("/api/v1/lights/level", { id: light.id, level: light.level > 0 ? 0 : "on" }); void refreshLights() }

  return <>
    {/* What is on: the one number that matters most, and the way to switch everything off. */}
    <section className="whisli-raised relative overflow-hidden rounded-xl p-5 sm:p-6">
      <div className="flex flex-wrap items-end justify-between gap-4">
        <div>
          <p className="whisli-muted text-sm">{t("homeLightsOn")}</p>
          <p className="mt-1 text-4xl font-semibold tabular-nums tracking-tight">
            {lights === null ? "–" : lit.length}<span className="whisli-muted text-2xl font-normal"> / {configured.length}</span>
          </p>
          <p className="whisli-muted mt-1 text-sm">{lights === null ? t("connecting") : lit.length === 0 ? t("homeAllOff") : lit.map(light => light.name).join(", ")}</p>
        </div>
        <div className="flex flex-wrap gap-2">
          <Button variant="outline" disabled={dark.length === 0} onClick={() => setSwitching({ lights: dark, on: true, all: true })}><Lightbulb className="size-4" />{t("homeAllOnButton")}</Button>
          <Button variant="outline" disabled={lit.length === 0} onClick={() => setSwitching({ lights: lit, on: false, all: true })}><Power className="size-4" />{t("homeAllOffButton")}</Button>
        </div>
      </div>
      <div className="mt-5 h-1.5 overflow-hidden rounded-full bg-[var(--line)]">
        <div className="h-full rounded-full bg-[var(--status-ok)] transition-[width] duration-500" style={{ width: `${configured.length ? lit.length * 100 / configured.length : 0}%` }} />
      </div>
    </section>

    {/* Shortcuts: each tile opens its section. */}
    <div className="mt-6 grid grid-cols-2 gap-3 xl:grid-cols-3">
      <Tile icon={Lightbulb} label={t("lights")} value={configured.length} onClick={() => onOpen({ page: "lights" })}
        detail={t("homeLightsDetail", { on: lit.length, dmx: configured.filter(light => isDmxLight(light.kind)).length, relay: configured.filter(light => light.kind === "relay").length })} />
      <Tile icon={Zap} label={t("homeButtons")} value={panel?.inputs.length ?? "–"} onClick={() => onOpen({ page: "actions" })}
        detail={t("homeButtonsDetail", { actions: actions.length, active: activeActions })} />
      <Tile icon={Boxes} label={t("devices")} value={devices.length} onClick={() => onOpen({ page: "devices" })}
        detail={relays.length === 0 ? t("homeDevicesOnlyController") : offline ? t("homeDevicesOffline", { count: offline }) : t("homeDevicesOnline", { count: relays.length })}
        alert={offline > 0} />
      <Tile icon={MapPin} label={t("zonesTitle")} value={zones.zones.length} onClick={() => onOpen({ page: "zones" })}
        detail={zones.zones.length ? zones.zones.map(zone => zone.name).join(" · ") : t("homeZonesNone")} />
      <Tile icon={Palette} label={t("navLightPresets")} value={presets.length} onClick={() => onOpen({ page: "lightPresets" })}
        detail={t("homePresetsDetail", { whites: presets.filter(preset => preset.kind === "temperature").length, colors: presets.filter(preset => preset.kind === "color").length })} />
      <Tile icon={Network} label={t("networkTitle")} value={system ? (system.network.ethernetIp ? "Ethernet" : system.network.wifiConnected ? "Wi-Fi" : "–") : "–"} onClick={() => onOpen({ page: "network" })}
        detail={system ? [system.network.ethernetIp, system.network.wifiConnected ? `${system.network.ssid} · ${system.network.wifiIp}` : ""].filter(Boolean).join(" · ") || t("notConnected") : t("connecting")} />
    </div>

    {/* Every light by zone: tap to switch, the slider icon for brightness and colour. */}
    <div className="mt-10 flex items-baseline justify-between gap-3">
      <h2 className="text-lg font-semibold">{t("homeLightsNow")}</h2>
      <button type="button" className="whisli-link text-sm" onClick={() => onOpen({ page: "lights" })}>{t("homeAllLights")}</button>
    </div>
    {lights === null ? <Muted className="mt-3">{t("connecting")}</Muted>
      : configured.length === 0 ? <Muted className="mt-3">{t("noLights")}</Muted>
      : <div className="mt-3 grid gap-3 lg:grid-cols-2">
        {groupByZone([...configured].sort((left, right) => naturalCompare(left.name, right.name)), light => zones.zoneOf(`light-${light.id}`), zones).map(group => {
          const groupLit = group.items.filter(light => light.level > 0)
          const groupDark = group.items.filter(light => light.level === 0)
          return <section key={group.zone} className="whisli-line rounded-xl border p-4">
            <header className="flex items-center gap-2">
              <MapPin className="whisli-muted size-4" />
              <h3 className="min-w-0 flex-1 truncate font-medium">{group.name || t(zones.zones.length ? "zoneNone" : "homeAllLightsGroup")}</h3>
              <span className="whisli-muted text-xs tabular-nums">{t("homeZoneOn", { on: groupLit.length, total: group.items.length })}</span>
              <Button variant="ghost" size="sm" className="h-7 px-2 text-xs" disabled={groupDark.length === 0} onClick={() => setSwitching({ lights: groupDark, on: true, all: false })}><Lightbulb className="size-3.5" />{t("homeZoneOn2")}</Button>
              <Button variant="ghost" size="sm" className="h-7 px-2 text-xs" disabled={groupLit.length === 0} onClick={() => setSwitching({ lights: groupLit, on: false, all: false })}><Power className="size-3.5" />{t("homeZoneOff")}</Button>
            </header>
            <div className="mt-3 grid gap-2 sm:grid-cols-2">
              {group.items.map(light => <LightTile key={light.id} light={light} onToggle={() => void toggle(light)} onAdjust={() => setControlled(light)} />)}
            </div>
          </section>
        })}
      </div>}

    {/* The controller itself, briefly. */}
    <button type="button" onClick={() => onOpen({ page: "system" })}
      className="whisli-line group mt-10 grid w-full gap-x-8 gap-y-3 rounded-xl border p-4 text-left transition-colors hover:border-[var(--ink)] sm:grid-cols-2 lg:grid-cols-4">
      <Datum label={t("firmwareVersion")} value={system?.firmware.version ?? "–"} />
      <Datum label={t("systemUptime")} value={system ? formatUptime(system.uptimeSeconds) : "–"} />
      <Datum label={t("homeMemory")} value={system ? t("homeMemoryValue", { heap: system.hardware.freeHeapKb, files: system.storage.filesTotalKb ? `${system.storage.filesUsedKb} / ${system.storage.filesTotalKb}` : "–" }) : "–"} />
      <Datum label={t("systemLogs")} value={t(warnings ? "homeWarnings" : "homeNoWarnings", { count: warnings })} alert={warnings > 0} />
    </button>

    {controlledNow && <LightControlDialog light={controlledNow} onClose={() => setControlled(null)} onChanged={() => void refreshLights()} />}
    <MessageBox
      open={switching !== null}
      type="question"
      title={t(switching?.on ? (switching.all ? "homeAllOnTitle" : "homeZoneOnTitle") : (switching?.all ? "homeAllOffTitle" : "homeZoneOffTitle"))}
      message={(switching?.lights ?? []).map(light => light.name).join(", ")}
      confirmLabel={t(switching?.on ? "actionTurnOn" : "actionTurnOff")}
      cancelLabel={t("cancel")}
      onConfirm={() => { if (switching) void switchAll(switching.lights, switching.on) }}
      onClose={() => setSwitching(null)}
    />
  </>
}

function Tile({ icon: Icon, label, value, detail, alert = false, onClick }: { icon: LucideIcon; label: string; value: number | string; detail: string; alert?: boolean; onClick: () => void }) {
  return <button type="button" onClick={onClick}
    className="whisli-line group relative flex min-w-0 flex-col gap-2 rounded-xl border p-3 text-left transition-[border-color,transform] hover:-translate-y-px hover:border-[var(--ink)] sm:flex-row sm:items-start sm:gap-3 sm:p-4">
    <span className="whisli-raised flex size-9 shrink-0 items-center justify-center rounded-lg sm:size-10"><Icon className="size-[18px] sm:size-5" /></span>
    <span className="min-w-0 flex-1">
      <span className="whisli-muted block text-sm">{label}</span>
      <span className="mt-0.5 block truncate text-xl font-semibold tabular-nums tracking-tight sm:text-2xl">{value}</span>
      <span className={cn("mt-1 block truncate text-xs", alert ? "whisli-status-error" : "whisli-muted")}>{detail}</span>
    </span>
    <ArrowUpRight className="whisli-muted absolute right-3 top-3 size-4 opacity-0 transition-opacity group-hover:opacity-100" />
  </button>
}

function LightTile({ light, onToggle, onAdjust }: { light: Light; onToggle: () => void; onAdjust: () => void }) {
  const { t } = useI18n()
  const on = light.level > 0
  const glow = lampColor(light.kind, light.colorTemperature ?? 50, light.color ?? defaultColor(light.kind))
  const dimmable = isDmxLight(light.kind)
  return <div className={cn("relative flex items-center rounded-lg border transition-colors", on ? "border-[var(--ink)]/40 bg-[var(--raised)]" : "whisli-line")}>
    <button type="button" onClick={onToggle} aria-pressed={on} title={t(on ? "actionTurnOff" : "actionTurnOn")}
      className="flex min-w-0 flex-1 items-center gap-3 px-3 py-2.5 text-left">
      <span className="flex size-8 shrink-0 items-center justify-center rounded-full transition-[background,box-shadow] duration-300"
        style={on ? { background: glow, boxShadow: `0 0 14px ${glow}`, color: "#3f2d00", opacity: dimmable ? 0.45 + 0.55 * light.level / 255 : 1 } : undefined}>
        {on ? <Lightbulb className="size-4" /> : <LightbulbOff className="whisli-muted size-4" />}
      </span>
      <span className="min-w-0 flex-1">
        <span className="block truncate text-sm font-medium">{light.name}</span>
        <span className="whisli-muted block text-xs tabular-nums">{on ? (dimmable ? `${Math.round(light.level * 100 / 255)}%` : t("outputOn")) : t("outputOff")}</span>
      </span>
    </button>
    {dimmable && <button type="button" onClick={onAdjust} aria-label={t("lightControl")} title={t("lightControl")}
      className="whisli-muted mr-1.5 flex size-8 shrink-0 items-center justify-center rounded-md hover:bg-[var(--raised)] hover:text-[var(--ink)]">
      <SlidersHorizontal className="size-4" />
    </button>}
  </div>
}

function Datum({ label, value, alert = false }: { label: string; value: string; alert?: boolean }) {
  return <div className="min-w-0">
    <span className="whisli-muted block text-xs">{label}</span>
    <span className={cn("mt-0.5 block truncate text-sm", alert && "whisli-status-error")}>{value}</span>
  </div>
}

