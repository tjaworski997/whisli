import { Lightbulb, Plus, Power, PowerOff, Search, Settings, Trash2, X } from "lucide-react"
import { useEffect, useMemo, useRef, useState, type ReactNode } from "react"

import { BehaviorFields, groupParams, type BehaviorGroup } from "@/components/light-behavior"
import { defaultColor, dmxRange, isDmxLight, LightControlDialog, lightKindLabel, LightSwatch } from "@/components/light-control"
import { ErrorText, Muted, PageHeader } from "@/components/page"
import { SortMenu } from "@/components/sort-menu"
import { useCollapsedZones, ZoneHeading, ZonePicker } from "@/components/zone-picker"
import { Button } from "@/components/ui/button"
import { Combobox } from "@/components/ui/combobox"
import { MessageBox } from "@/components/ui/message-box"
import { Slider } from "@/components/ui/slider"
import { useLiveRefresh } from "@/hooks/use-live-refresh"
import { groupByZone, useZones, type ZonesApi } from "@/hooks/use-zones"
import { useI18n, type TranslationKey } from "@/i18n"
import { getJson, post } from "@/lib/api"
import { naturalCompare, useRememberedSort, useStableOrder } from "@/lib/sort"
import { cn } from "@/lib/utils"
import type { DimCurve, Device, Dimming, Light, LightBehavior, LightsResponse } from "@/types"

/** A used output a relay light can switch: "Module · channel name", with its code (CH01, DO03) apart. */
type Output = { id: string; device: string; channel: string; code: string }
const outputLabel = (output: Output) => `${output.device} · ${output.channel}`

function relayOutputs(devices: Device[]): Output[] {
  return devices.flatMap(device => {
    if (device.kind === "relay" && !device.active) return []
    const prefix = device.kind === "controller" ? "DO" : "CH"
    const base = device.kind === "controller" ? "controller" : `relay-${device.id.replace("relay-", "")}`
    return device.channels.flatMap((_, index) => {
      if (!device.channelUsed[index]) return []
      const code = `${prefix}${String(index + 1).padStart(2, "0")}`
      return [{ id: `${base}.output-${index + 1}`, device: device.name, channel: device.channelNames[index] || code, code }]
    })
  })
}

const lightSorts = ["zone", "name", "wiring", "added"] as const
type LightSort = typeof lightSorts[number]
const lightSortOptions: { value: LightSort; label: TranslationKey }[] = [
  { value: "zone", label: "sortZoneName" }, { value: "name", label: "sortName" }, { value: "wiring", label: "sortWiring" }, { value: "added", label: "sortAdded" },
]

/** With `lightId` (opened from the map) only that light is shown, already expanded. */
export function LightsPage({ devices, lightId, onClearFilter }: { devices: Device[]; lightId?: number; onClearFilter: () => void }) {
  const { t } = useI18n()
  const [lights, setLights] = useState<Light[] | null>(null)
  const [defaults, setDefaults] = useState<LightBehavior | null>(null)
  const [error, setError] = useState("")
  const [search, setSearch] = useState("")
  const [addedId, setAddedId] = useState<number | null>(null)
  const zones = useZones()
  const folded = useCollapsedZones("whisli-lights-collapsed")
  const outputs = useMemo(() => relayOutputs(devices), [devices])
  const [sort, setSort] = useRememberedSort<LightSort>("whisli-lights-sort", lightSorts, "zone")
  // Wiring: DMX by channel, then relays by output name, then lights not set up yet.
  const wiringRank = (light: Light) => isDmxLight(light.kind) ? 0 : light.kind === "relay" ? 1 : 2
  const outputName = (light: Light) => { const output = outputs.find(item => item.id === light.relayTarget); return output ? outputLabel(output) : light.relayTarget }
  const ordered = useStableOrder(lights ?? [], light => light.id, (left, right) => {
    if (sort === "added") return left.id - right.id
    if (sort === "wiring") return wiringRank(left) - wiringRank(right) || left.dmxChannel - right.dmxChannel || naturalCompare(outputName(left), outputName(right)) || left.id - right.id
    return naturalCompare(left.name, right.name) || left.id - right.id
  }, sort)
  const query = search.trim().toLocaleLowerCase()
  const filteredLights = ordered.filter(light => (!lightId || light.id === lightId) && light.name.toLocaleLowerCase().includes(query))
  // Only "zone · name" groups the list under zone headings.
  const groups = sort === "zone" ? groupByZone(filteredLights, light => zones.zoneOf(`light-${light.id}`), zones) : [{ zone: -1, name: "", items: filteredLights }]
  const focused = lightId ? lights?.find(light => light.id === lightId) : undefined

  async function refresh() {
    try {
      const response = await getJson<LightsResponse>("/api/v1/lights")
      setLights(response.lights)
      setDefaults(response.defaults ?? null)
      setError("")
    }
    catch { setError(t("lightsLoadError")) }
  }
  useLiveRefresh(refresh, ["lights"])

  return <>
    <PageHeader eyebrow={t("automation")} title={t("lights")} description={t("lightsDescription")} actions={<>
      <label className="relative min-w-0 flex-1 sm:w-72 sm:flex-none"><Search className="whisli-muted pointer-events-none absolute left-3 top-1/2 size-4 -translate-y-1/2" /><input className="whisli-input whisli-input-with-icon h-10 w-full" type="search" value={search} onChange={event => setSearch(event.target.value)} placeholder={t("searchLightsPlaceholder")} aria-label={t("searchLights")} /></label>
      <Button variant="outline" onClick={() => void post("/api/v1/lights").then(result => {
        if (!result.ok) { setError(result.message || t("lightSaveError")); return }
        setSearch("")
        setAddedId(Number(result.data.id) || null)
        void refresh()
      })}><Plus className="size-4" />{t("addLight")}</Button>
    </>} />
    {/* Above the list: the active filter on the left, the sort on the right. */}
    <div className="mt-5 flex min-h-8 items-center gap-1 text-sm">
      {lightId && <>
        <span className="whisli-muted">{t("lightsFilter")}</span>
        <button className="whisli-line inline-flex items-center gap-1.5 rounded-full border px-3 py-1 font-medium hover:border-[var(--ink)]" title={t("clearFilter")} onClick={onClearFilter}>
          {focused?.name ?? `#${lightId}`}<X className="size-3.5" />
        </button>
      </>}
      <span className="flex-1" />
      <SortMenu value={sort} options={lightSortOptions} onChange={setSort} />
    </div>
    {error && <ErrorText>{error}</ErrorText>}
    {lights === null ? <Muted className="whisli-line mt-3 border-t py-4">{t("connecting")}</Muted>
      : lights.length === 0 ? <Muted className="whisli-line mt-3 border-t py-4">{t("noLights")}</Muted>
      : filteredLights.length === 0 ? <Muted className="whisli-line mt-3 border-t py-4">{t("noLightsMatch")}</Muted>
      : <div className="whisli-line mt-3 border-t">
        <div className={`${columns} whisli-muted whisli-line hidden border-b py-2 text-xs lg:grid`}><span className="pl-10">{t("lightName")}</span><span>{t("lightWiring")}</span><span className="px-3">{t("catalogActionColumn")}</span></div>
        {/* Grouped by zone in the user's order; without any zones it is one plain list. */}
        {groups.map(group => <div key={group.zone}>
          {group.zone >= 0 && zones.zones.length > 0 && <ZoneHeading name={group.name || t("zoneNone")} count={group.items.length} collapsed={folded.isCollapsed(group.zone)} onToggle={() => folded.toggle(group.zone)} />}
          {/* Under a zone heading the rows step in, so the group reads at a glance. A search or a light opened from the map shows everything. */}
          {(group.zone < 0 || !folded.isCollapsed(group.zone) || query || lightId) && <div className={cn(group.zone >= 0 && zones.zones.length > 0 && "pl-4 sm:pl-6")}>
            {group.items.map(light => <LightRow key={`${light.id}${light.id === lightId ? "-focused" : ""}`} light={light} others={(lights ?? []).filter(other => other.id !== light.id)} defaults={defaults} outputs={outputs} zones={zones} focused={light.id === lightId} added={light.id === addedId} onRefresh={refresh} />)}
          </div>}
        </div>)}
      </div>}
  </>
}

// Name (with what it is wired to) | state | controls. Phone: name and controls on one line, the summary below.
const columns = "grid grid-cols-[minmax(0,1fr)_auto] items-center gap-x-3 gap-y-1 lg:grid-cols-[minmax(0,1.3fr)_minmax(0,1fr)_auto]"

function LightRow({ light, others, defaults, outputs, zones, focused, added, onRefresh }: { light: Light; others: Light[]; defaults: LightBehavior | null; outputs: Output[]; zones: ZonesApi; focused: boolean; added: boolean; onRefresh: () => void }) {
  const { t } = useI18n()
  const [busy, setBusy] = useState(false)
  // A light without a type still needs setting up, so its settings start open.
  const [settingsOpen, setSettingsOpen] = useState(focused || added || light.kind === "none")
  const nameInput = useRef<HTMLInputElement>(null)
  // A light just added: bring it into view with its name selected, ready to type over.
  useEffect(() => {
    if (!added) return
    nameInput.current?.scrollIntoView({ behavior: "smooth", block: "center" })
    nameInput.current?.select()
  }, [added])
  const [controlOpen, setControlOpen] = useState(false)
  const [removing, setRemoving] = useState(false)
  const [own, setOwn] = useState<Partial<LightBehavior>>(light.behavior ?? {})
  const [dimming, setDimming] = useState<Dimming>(light.dimming ?? { minimum: 0, curve: "linear" })
  const [name, setName] = useState(light.name)
  const [kind, setKind] = useState<Light["kind"]>(light.kind)
  const [relayTarget, setRelayTarget] = useState(light.relayTarget)
  const [dmxChannel, setDmxChannel] = useState(light.dmxChannel ? String(light.dmxChannel) : "")
  const [error, setError] = useState("")
  const [showValidation, setShowValidation] = useState(false)
  const usesDmx = isDmxLight(kind)
  const dmxChannels = { rgbw: 4, rgb: 3, cct: 2 }[kind as string] ?? 1
  const maxChannel = 513 - dmxChannels
  const channelValid = Number(dmxChannel) >= 1 && Number(dmxChannel) <= maxChannel
  // One output and one set of DMX channels per light: say who has them before anything is saved.
  const outputOwner = (outputId: string) => others.find(other => other.kind === "relay" && other.relayTarget === outputId)
  const lastChannel = Number(dmxChannel) + dmxChannels - 1
  const dmxOwner = usesDmx && channelValid ? others.find(other => isDmxLight(other.kind) &&
    Number(dmxChannel) <= other.dmxChannel + ({ rgbw: 4, rgb: 3, cct: 2 }[other.kind as string] ?? 1) - 1 && other.dmxChannel <= lastChannel) : undefined
  const conflict = kind === "relay" ? outputOwner(relayTarget) : dmxOwner
  const configured = kind !== "none" && Boolean(name) && !conflict && (kind === "relay" ? Boolean(relayTarget) : channelValid)
  const invalid = (bad: boolean) => showValidation && bad ? "animate-pulse border-red-500" : ""
  const saveBehavior = async (params: Record<string, string | number | boolean>) => { const result = await post("/api/v1/lights/behavior", { id: light.id, ...params }); setError(result.ok ? "" : result.message || t("lightSaveError")); onRefresh() }
  const changeBehavior = (group: BehaviorGroup, next: LightBehavior) => { const params = groupParams(group, next); setOwn(current => ({ ...current, ...params })); void saveBehavior(params) }
  const resetBehavior = (group: BehaviorGroup) => {
    setOwn(current => { const next = { ...current }; for (const key of Object.keys(groupParams(group, { ...defaults!, ...current }))) delete next[key as keyof LightBehavior]; return next })
    void saveBehavior({ [group]: "inherit" })
  }
  const saveSettings = async () => { const result = await post("/api/v1/lights/settings", { id: light.id, name, kind, relayTarget, dmxChannel: Number(dmxChannel) }); if (!result.ok) { setError(result.message || t("lightSaveError")); return false } setError(""); onRefresh(); return true }
  // Autosave edits, but not the values just loaded from the controller.
  const unchanged = name === light.name && kind === light.kind && relayTarget === light.relayTarget && Number(dmxChannel) === light.dmxChannel
  useEffect(() => {
    if (!configured || unchanged) return
    const timer = window.setTimeout(() => void saveSettings(), 450)
    return () => window.clearTimeout(timer)
  }, [configured, unchanged, dmxChannel, kind, light.id, name, relayTarget])
  const saveDimming = async (next: Dimming) => { setDimming(next); const result = await post("/api/v1/lights/dimming", { id: light.id, ...next }); setError(result.ok ? "" : result.message || t("lightSaveError")); onRefresh() }
  const on = light.level > 0
  // An incomplete light cannot be switched yet: show what is missing.
  async function toggle() {
    if (!configured) { setShowValidation(true); setSettingsOpen(true); return }
    setBusy(true)
    if (unchanged || await saveSettings()) await post("/api/v1/lights/level", { id: light.id, level: on ? 0 : "on" })
    setBusy(false)
    onRefresh()
  }
  async function remove() {
    setBusy(true)
    const result = await post("/api/v1/lights/remove", { id: light.id })
    setBusy(false)
    setRemoving(false)
    if (!result.ok) setError(result.message || t("lightSaveError"))
    onRefresh()
  }

  const output = outputs.find(item => item.id === light.relayTarget)
  const summary = light.kind === "none" ? t("lightNotConfigured")
    : light.kind === "relay" ? <span className="flex min-w-0 items-baseline gap-1.5">
      <span className="truncate">{output ? outputLabel(output) : `${t(lightKindLabel.relay)} · ${light.relayTarget || "—"}`}</span>
      {output && output.channel !== output.code && <span className="shrink-0 font-mono text-[10px]">{output.code}</span>}
    </span>
    : `${t(lightKindLabel[light.kind])} · ${dmxRange(light)}`

  return <section className="whisli-line border-b py-3">
    <div className={columns}>
      <div className="flex min-w-0 items-center gap-3">
        <Lightbulb className={cn("size-5 shrink-0", on ? "text-amber-500" : "whisli-muted")} />
        <input ref={nameInput} className={cn("whisli-input whisli-input-inline h-9 min-w-0 flex-1 font-medium", invalid(!name))} aria-label={t("lightName")} placeholder={t("lightName")} value={name} onChange={event => setName(event.currentTarget.value)} />
      </div>
      <div className="flex items-center justify-end gap-1 lg:order-last">
        <Button variant={on ? "outline" : "ghost"} className="h-9 w-[6.5rem] justify-start" disabled={busy} onClick={() => void toggle()}>
          {on ? <PowerOff className="size-4 text-amber-500" /> : <Power className="size-4" />}{t(on ? "actionTurnOff" : "actionTurnOn")}
        </Button>
        {usesDmx && light.kind === kind && configured
          ? <Button variant="ghost" className="h-9 w-[4.75rem] justify-start gap-2 px-2.5" aria-label={t("lightControl")} title={t("lightControl")} onClick={() => setControlOpen(true)}>
            <LightSwatch kind={light.kind} level={light.level} temperature={light.colorTemperature ?? 50} color={light.color ?? defaultColor(light.kind)} />
            <span className="tabular-nums text-xs">{on ? `${Math.round(light.level * 100 / 255)}%` : t("lightOffShort")}</span>
          </Button>
          : <span className="block w-[4.75rem] shrink-0" aria-hidden />}
        <Button variant="ghost" size="icon" aria-expanded={settingsOpen} aria-label={t("lightSettings")} title={t("lightSettings")} onClick={() => setSettingsOpen(value => !value)}>
          <Settings className={cn("size-4 transition-transform", settingsOpen && "rotate-45 text-[var(--ink)]")} />
        </Button>
        <Button variant="dangerGhost" size="icon" aria-label={t("removeLight")} title={t("removeLight")} onClick={() => setRemoving(true)}><Trash2 className="size-4" /></Button>
      </div>
      <p className={cn("col-span-2 truncate pl-10 text-xs lg:col-span-1 lg:pl-0", light.kind === "none" ? "whisli-status-error" : "whisli-muted")}>{summary}</p>
    </div>

    {settingsOpen && <div className="whisli-raised mt-3 grid gap-5 rounded-md p-4 lg:ml-8">
      <SettingsGroup title={t("lightConnection")}>
        <div className="grid gap-3 sm:grid-cols-2 xl:grid-cols-3">
          <select className={cn("whisli-select w-full", invalid(kind === "none"))} aria-label={t("lightType")} value={kind} onChange={event => setKind(event.currentTarget.value as Light["kind"])}>
            <option value="none">{t("chooseLightType")}</option>
            <optgroup label={t("lightTypeGroupRelay")}><option value="relay">{t("relayLight")}</option></optgroup>
            <optgroup label={t("lightTypeGroupDmx")}><option value="dmx">{t("dmxWhiteLed")}</option><option value="cct">{t("dmxCctLed")}</option><option value="rgb">{t("dmxRgbLed")}</option><option value="rgbw">{t("dmxRgbwLed")}</option></optgroup>
          </select>
          {kind === "relay" && <Combobox className={cn(invalid(!relayTarget), conflict && "border-[var(--status-error)]")} aria-label={t("relayOutput")} value={relayTarget} placeholder={t("chooseRelayOutput")} searchPlaceholder={t("searchRelayOutput")} emptyText={t("noActionTargetMatch")}
            groups={[...new Set(outputs.map(item => item.device))].map(device => ({ label: device, items: outputs.filter(item => item.device === device)
              .map(item => {
                const owner = outputOwner(item.id)
                return { id: item.id, name: outputLabel(item), label: item.channel, hint: item.channel !== item.code ? item.code : undefined,
                  badge: owner ? t("lightOutputTaken", { name: owner.name }) : undefined, disabled: Boolean(owner) }
              })
              .sort((left, right) => Number(left.disabled) - Number(right.disabled)) }))}
            onValueChange={setRelayTarget} />}
          {usesDmx && <label className={cn("whisli-input flex h-9 items-center gap-2 py-0 focus-within:border-[var(--ink)]", invalid(!channelValid), conflict && "border-[var(--status-error)]")}>
            <span className="whisli-muted shrink-0 text-xs">DMX</span>
            <input className="min-w-0 flex-1 bg-transparent outline-none" type="number" min="1" max={maxChannel} aria-label={t(kind === "dmx" ? "dmxChannel" : "dmxFirstChannel", { max: maxChannel })} placeholder={`1–${maxChannel}`} value={dmxChannel} onChange={event => setDmxChannel(event.currentTarget.value)} />
            {dmxChannels > 1 && channelValid && <span className="whisli-muted shrink-0 text-xs tabular-nums" title={t(kind === "rgbw" ? "dmxRgbwChannels" : kind === "rgb" ? "dmxRgbChannels" : "dmxCctChannels", { first: Number(dmxChannel), last: Number(dmxChannel) + dmxChannels - 1 })}>→ {Number(dmxChannel) + dmxChannels - 1}</span>}
          </label>}
          <ZonePicker target={`light-${light.id}`} zones={zones} className="sm:col-span-2 xl:col-span-1" />
        </div>
        {conflict && <p role="alert" className="whisli-status-error mt-2 text-xs">{t(kind === "relay" ? "lightOutputConflict" : "lightDmxConflict", { name: conflict.name })}</p>}
        {showValidation && !configured && !conflict && <Muted className="mt-2 text-xs">{t("completeLightBeforeTest")}</Muted>}
      </SettingsGroup>
      {usesDmx && <SettingsGroup title={t("lightDimming")}>
        <DimmingSettings value={dimming} disabled={!configured} onChange={setDimming} onCommit={next => void saveDimming(next)} />
      </SettingsGroup>}
      {kind !== "none" && defaults && <SettingsGroup title={t("lightBehavior")}>
        <BehaviorFields relay={kind === "relay"} defaults={defaults} own={own} disabled={!configured} onChange={changeBehavior} onReset={resetBehavior} />
      </SettingsGroup>}
    </div>}
    {error && <ErrorText>{error}</ErrorText>}

    {controlOpen && <LightControlDialog light={light} onClose={() => setControlOpen(false)} onChanged={onRefresh} />}
    <MessageBox open={removing} type="danger" title={t("removeLightTitle")} message={t("removeLightConfirm")} detail={light.name || `#${light.id}`}
      confirmLabel={t("remove")} cancelLabel={t("cancel")} busy={busy} onConfirm={() => void remove()} onClose={() => setRemoving(false)} />
  </section>
}

function SettingsGroup({ title, children }: { title: string; children: ReactNode }) {
  return <div><h3 className="whisli-muted mb-2 text-xs font-medium uppercase tracking-wide">{title}</h3>{children}</div>
}

const curves: { value: DimCurve; label: TranslationKey }[] = [
  { value: "linear", label: "dimCurveLinear" },
  { value: "perceptual", label: "dimCurvePerceptual" },
  { value: "square", label: "dimCurveSquare" },
]

/** Per-lamp dimming: where the lowest level starts and how the slider maps to output. */
function DimmingSettings({ value, disabled, onChange, onCommit }: { value: Dimming; disabled: boolean; onChange: (value: Dimming) => void; onCommit: (value: Dimming) => void }) {
  const { t } = useI18n()
  return <div className="grid items-end gap-5 sm:grid-cols-2">
    <label className="grid gap-2 text-xs">
      <span className="font-medium">{t("dimCurve")}</span>
      <select className="whisli-select w-full text-sm" disabled={disabled} value={value.curve} onChange={event => onCommit({ ...value, curve: event.currentTarget.value as DimCurve })}>
        {curves.map(curve => <option key={curve.value} value={curve.value}>{t(curve.label)}</option>)}
      </select>
    </label>
    <Slider value={[value.minimum]} min={0} max={50} step={1} disabled={disabled} label={t("dimMinimum")} valueLabel={`${value.minimum}%`}
      onValueChange={next => onChange({ ...value, minimum: next[0] ?? 0 })} onValueCommitted={next => onCommit({ ...value, minimum: next[0] ?? 0 })} />
  </div>
}
