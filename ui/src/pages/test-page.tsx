import { Lightbulb, LightbulbOff, Power, SlidersHorizontal, Sparkles } from "lucide-react"
import { useState } from "react"

import { defaultColor, isDmxLight, lampColor, LightControlDialog } from "@/components/light-control"
import { Muted, PageHeader, Section } from "@/components/page"
import { Button } from "@/components/ui/button"
import { useLiveRefresh } from "@/hooks/use-live-refresh"
import { usePolling } from "@/hooks/use-polling"
import { useI18n } from "@/i18n"
import { getJson, post } from "@/lib/api"
import { channelLabel, formatUptime } from "@/lib/format"
import { naturalCompare } from "@/lib/sort"
import { cn } from "@/lib/utils"
import type { Device, Light, LightsResponse, TestStatus } from "@/types"

/**
 * System › Installation test: every light and every output, used or not, to check the wiring one by
 * one, and chaos mode to load-test everything at once.
 */
export function TestPage({ devices, onRefreshDevices }: { devices: Device[]; onRefreshDevices: () => unknown }) {
  const { t } = useI18n()
  const [lights, setLights] = useState<Light[] | null>(null)
  const [status, setStatus] = useState<TestStatus | null>(null)
  const [controlled, setControlled] = useState<Light | null>(null)
  const chaos = status?.chaos

  async function refreshLights() {
    try { setLights((await getJson<LightsResponse>("/api/v1/lights")).lights) } catch { /* keep */ }
  }
  useLiveRefresh(refreshLights, ["lights"])
  usePolling(async () => { try { setStatus(await getJson<TestStatus>("/api/v1/test")) } catch { /* keep */ } }, 2000)

  async function toggleChaos() {
    const on = !chaos?.running
    await post("/api/v1/test/chaos", { on })
    setStatus(await getJson<TestStatus>("/api/v1/test").catch(() => status))
    void refreshLights()
  }
  const toggleLight = async (light: Light) => { await post("/api/v1/lights/level", { id: light.id, level: light.level > 0 ? 0 : "on" }); void refreshLights() }
  async function toggleOutput(device: Device, index: number) {
    await post("/api/v1/devices/channel", { id: device.id, channel: index + 1, state: device.channels[index] ? "off" : "on" })
    await onRefreshDevices()
    void refreshLights()  // relay lights follow their output
  }
  async function setAll(device: Device, on: boolean) {
    await post("/api/v1/devices/channels/all", { id: device.id, state: on ? "on" : "off" })
    await onRefreshDevices()
    void refreshLights()
  }
  const controlledNow = controlled && lights?.find(light => light.id === controlled.id)
  const sortedLights = [...(lights ?? [])].sort((left, right) => naturalCompare(left.name, right.name))

  return <>
    <PageHeader eyebrow={t("navSystem")} title={t("testTitle")} description={t("testDescription")} />

    {/* Chaos mode: everything at random until switched off, then back as it was. */}
    <section className="whisli-raised mt-6 rounded-xl p-5">
      <div className="flex flex-wrap items-center gap-4">
        <Sparkles className="size-6 shrink-0" />
        <div className="min-w-0 flex-1">
          <h2 className="font-semibold">Chaos mode</h2>
          <p className="whisli-muted mt-1 text-sm">{t("chaosDescription")}</p>
          {/* A fixed-height status line, so the card never changes size. */}
          <p className="mt-1 h-5 truncate text-sm text-[var(--status-ok)]">
            {chaos?.running ? t("chaosRunning", { time: formatUptime(chaos.elapsedSeconds), switches: chaos.switches }) : chaos?.restoring ? t("chaosRestoring") : ""}
          </p>
        </div>
        {/* Only the button shows that it is running. */}
        <Button variant="outline" className={cn(chaos?.running && "border-[var(--status-ok)] bg-[var(--status-ok)] text-white hover:bg-[var(--status-ok)] hover:text-white")} disabled={!status || chaos?.restoring} onClick={() => void toggleChaos()}>
          <Power className="size-4" />{t(chaos?.running ? "chaosStop" : "chaosStart")}
        </Button>
      </div>
    </section>

    <Section bordered className="mt-6 py-6" title={t("lights")} description={t("testLightsDescription")}>
      {lights === null ? <Muted className="mt-3">{t("connecting")}</Muted> : sortedLights.length === 0 ? <Muted className="mt-3">{t("noLights")}</Muted>
        : <div className="mt-4 grid gap-2 sm:grid-cols-2 lg:grid-cols-3">
          {sortedLights.map(light => {
            const on = light.level > 0
            const glow = lampColor(light.kind, light.colorTemperature ?? 50, light.color ?? defaultColor(light.kind))
            return <div key={light.id} className={cn("flex items-center rounded-lg border", on ? "border-[var(--ink)]/40 bg-[var(--raised)]" : "whisli-line", light.kind === "none" && "opacity-50")}>
              <button type="button" disabled={light.kind === "none"} onClick={() => void toggleLight(light)} className="flex min-w-0 flex-1 items-center gap-3 px-3 py-2 text-left">
                <span className="flex size-7 shrink-0 items-center justify-center rounded-full" style={on ? { background: glow, boxShadow: `0 0 10px ${glow}`, color: "#3f2d00" } : undefined}>
                  {on ? <Lightbulb className="size-4" /> : <LightbulbOff className="whisli-muted size-4" />}
                </span>
                <span className="min-w-0 flex-1">
                  <span className="block truncate text-sm font-medium">{light.name || `#${light.id}`}</span>
                  <span className="whisli-muted block text-xs tabular-nums">{light.kind === "none" ? t("lightNotConfigured") : on ? (isDmxLight(light.kind) ? `${Math.round(light.level * 100 / 255)}%` : t("outputOn")) : t("outputOff")}</span>
                </span>
              </button>
              {isDmxLight(light.kind) && <button type="button" onClick={() => setControlled(light)} aria-label={t("lightControl")} title={t("lightControl")}
                className="whisli-muted mr-1 flex size-8 shrink-0 items-center justify-center rounded-md hover:bg-[var(--raised)] hover:text-[var(--ink)]"><SlidersHorizontal className="size-4" /></button>}
            </div>
          })}
        </div>}
    </Section>

    {devices.map(device => {
      const inactive = device.kind === "relay" && !device.active
      const prefix = device.kind === "controller" ? "DO" : "CH"
      return <Section key={device.id} bordered className="mt-0 border-t-0 py-6" title={device.name}
        description={t(inactive ? "testModuleInactive" : !device.online ? "testModuleOffline" : "testOutputsDescription")}
        actions={<>
          <Button variant="outline" size="sm" disabled={inactive || !device.online} onClick={() => void setAll(device, true)}>{t("testAllOn")}</Button>
          <Button variant="outline" size="sm" disabled={inactive || !device.online} onClick={() => void setAll(device, false)}>{t("testAllOff")}</Button>
        </>}>
        <div className={cn("mt-4 grid grid-cols-2 gap-2 sm:grid-cols-4 lg:grid-cols-8", (inactive || !device.online) && "opacity-50")}>
          {device.channels.map((on, index) => <button key={index} type="button" disabled={inactive || !device.online} onClick={() => void toggleOutput(device, index)}
            className={cn("flex flex-col items-start rounded-lg border px-2.5 py-2 text-left transition-colors", on ? "border-[var(--status-ok)] bg-[var(--status-ok-bg)]" : "whisli-line hover:border-[var(--ink)]")}
            title={device.channelNames[index] || channelLabel(prefix, index)}>
            <span className="flex w-full items-center gap-1.5">
              <span className={cn("size-2 rounded-full", on ? "bg-[var(--status-ok)]" : "bg-[var(--line)]")} />
              <span className="font-mono text-xs">{channelLabel(prefix, index)}</span>
            </span>
            <span className={cn("mt-1 w-full truncate text-xs", device.channelUsed[index] ? "" : "whisli-muted italic")}>{device.channelNames[index] || (device.channelUsed[index] ? "—" : t("testUnused"))}</span>
          </button>)}
        </div>
      </Section>
    })}

    {controlledNow && <LightControlDialog light={controlledNow} onClose={() => setControlled(null)} onChanged={() => void refreshLights()} />}
  </>
}
