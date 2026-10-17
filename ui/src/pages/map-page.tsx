import "@xyflow/react/dist/style.css"

import { Background, Controls, Handle, Position, ReactFlow, type Edge, type Node, type NodeChange, type NodeProps } from "@xyflow/react"
import { ChevronsDown, ChevronsUp, Globe, Hand, Send, Palette, Lightbulb, LightbulbOff, MapPin, MousePointerClick, SunMedium, Power, PowerOff, Repeat2, Timer, ToggleLeft, ToggleRight, type LucideIcon } from "lucide-react"
import { useEffect, useMemo, useRef, useState, type ReactNode } from "react"

import { kelvinSwatch, mixToKelvin } from "@/components/color-temperature-picker"
import { ErrorText, Muted, PageHeader } from "@/components/page"
import { toHex } from "@/components/rgbw-picker"
import { subscribeInputEvents, useLiveRefresh, type LiveInputEvent } from "@/hooks/use-live-refresh"
import { usePresets } from "@/hooks/use-presets"
import { groupByZone, useZones, type ZonesApi } from "@/hooks/use-zones"
import { useI18n, type TranslationKey } from "@/i18n"
import { getJson, post } from "@/lib/api"
import { buttonSortKey, buttonSorts, compareButtons, gestureOrder, naturalCompare, useRememberedSort, type ButtonSort } from "@/lib/sort"
import { commandText, triggerLabels } from "@/lib/labels"
import type { AppRoute } from "@/lib/route"
import { cn } from "@/lib/utils"
import type { ActionPanelData, Light, LightPreset, PanelInput } from "@/types"

// Columns of the map: what a button press travels through, left to right.
const columns = { input: 0, gesture: 1, action: 2, target: 3 } as const
type Column = keyof typeof columns
const columnX: Record<Column, number> = { input: 0, gesture: 270, action: 520, target: 760 }
const rowHeight = 76
const highlightMs = 1600

// Each kind has its own shape, so the map reads without the labels.
type NodeKind = "input" | "gesture" | "action" | "light" | "relay" | "zone"
type MapNodeData = {
  kind: NodeKind
  column: Column
  title: string
  subtitle?: string
  icon: LucideIcon
  /** Short mark on the icon, e.g. "2×" for a double click. */
  badge?: string
  /** Live state: pressed, lit, switched on. */
  on?: boolean
  /** Lights: the colour they shine with. */
  glow?: string
  /** Buttons: held down right now (live press, before a gesture is recognised). */
  held?: boolean
  active: boolean
  dimmed?: boolean
  /** Where a click opens its editor (zone headings open nothing). */
  open?: AppRoute
  /** Lights and relays: the power button in the corner switches them. */
  toggle?: () => unknown
}
type MapNode = Node<MapNodeData, "map">

function MapNodeView({ data }: NodeProps<MapNode>) {
  const Icon = data.icon
  const handles = <>
    {data.column !== "input" && <Handle type="target" position={Position.Left} className="!invisible" />}
    {data.column !== "target" && <Handle type="source" position={Position.Right} className="!invisible" />}
  </>
  const power = data.toggle && <button
    type="button"
    className={cn("whisli-map-power nodrag nopan absolute -right-2 -top-2 flex size-6 items-center justify-center rounded-full border", data.on && "whisli-map-power-on")}
    aria-label={data.on ? "OFF" : "ON"}
    onClick={event => { event.stopPropagation(); void data.toggle?.() }}
  ><Power className="size-3.5" /></button>
  const text = <span className="min-w-0 flex-1">
    <b className="block truncate text-xs font-medium">{data.title}</b>
    {data.subtitle && <small className="whisli-muted block truncate text-[11px]">{data.subtitle}</small>}
  </span>
  const frame = (shape: string, children: ReactNode) => <div className={cn(
    "whisli-map-node relative flex items-center gap-2.5 border text-left transition-[box-shadow,border-color,opacity] duration-300",
    shape, data.active && "whisli-map-node-active", data.dimmed && "opacity-45",
  )}>{handles}{children}</div>

  switch (data.kind) {
    case "zone":  // a heading above the buttons of one zone
      return <div className="whisli-muted flex w-[210px] items-center gap-1.5 px-1 text-[11px] font-medium uppercase tracking-wide">
        <MapPin className="size-3.5 shrink-0" /><span className="truncate">{data.title}</span>
      </div>
    case "input":  // a wall button: card with a round key that lights up while pressed
      return frame("w-[210px] rounded-2xl px-2.5 py-2", <>
        <span className={cn("whisli-map-key flex size-9 shrink-0 items-center justify-center rounded-full border-2", (data.on || data.held) && "whisli-map-key-pressed")}>
          <span className="size-3 rounded-full bg-current opacity-70" />
        </span>
        {text}
      </>)
    case "gesture":  // a compact pill
      return frame("w-[180px] rounded-full px-3 py-1.5", <>
        <span className="relative shrink-0"><Icon className="size-4" />{data.badge && <b className="absolute -right-2.5 -top-2 text-[9px] leading-none">{data.badge}</b>}</span>
        {text}
      </>)
    case "action":  // a flowchart hexagon holding the command; lines meet its pointed ends
      return <div className={cn("relative flex h-11 w-[150px] items-center justify-center gap-2 px-5 transition-opacity", data.dimmed && "opacity-45")}>
        {handles}
        <svg className={cn("whisli-map-hexagon absolute inset-0 size-full overflow-visible", data.active && "whisli-map-hexagon-active")} viewBox="0 0 150 44" preserveAspectRatio="none" aria-hidden>
          <polygon points="14,1 136,1 149,22 136,43 14,43 1,22" vectorEffect="non-scaling-stroke" />
        </svg>
        <Icon className="relative size-4 shrink-0" />
        <span className="relative min-w-0 truncate text-xs font-medium">{data.title}{data.subtitle && <small className="whisli-muted block truncate text-[10px] font-normal">{data.subtitle}</small>}</span>
      </div>
    case "light":  // a bulb that shines with the light's real colour
      return frame("w-[220px] rounded-xl px-2.5 py-2", <>
        <span className="flex size-9 shrink-0 items-center justify-center rounded-full transition-[background,box-shadow] duration-300" style={data.on && data.glow ? { background: data.glow, boxShadow: `0 0 14px ${data.glow}`, color: "#3f2d00" } : undefined}>
          <Icon className={cn("size-5", !data.on && "whisli-muted")} />
        </span>
        {text}
        {power}
      </>)
    case "relay":  // square corners and a switch
      return frame("w-[220px] rounded-sm px-2.5 py-2", <>
        <span className={cn("flex size-9 shrink-0 items-center justify-center rounded-sm", data.on ? "whisli-map-relay-on" : "whisli-raised")}>
          <Icon className="size-5" />
        </span>
        {text}
        {power}
      </>)
  }
}

const nodeTypes = { map: MapNodeView }

type MapData = { panel: ActionPanelData; lights: Light[] }

const gestureIcons: Record<string, { icon: LucideIcon; badge?: string }> = {
  click: { icon: MousePointerClick },
  doubleClick: { icon: MousePointerClick, badge: "2×" },
  tripleClick: { icon: MousePointerClick, badge: "3×" },
  longClick: { icon: Timer },
  hold: { icon: Hand },
}
const commandIcons: Record<string, LucideIcon> = {
  toggle: Repeat2, on: Power, off: PowerOff, setLevel: SunMedium, stepUp: ChevronsUp, stepDown: ChevronsDown, preset: Palette, run: Send,
}

/** "controller.output-3" or "relay-1.output-3" → the device channel API call that flips it. */
function relayToggle(targetId: string, on: boolean) {
  const match = targetId.match(/^(controller|relay-\d+)\.output-(\d+)$/)
  if (!match) return undefined
  return () => post("/api/v1/devices/channel", { id: match[1], channel: Number(match[2]), state: on ? "off" : "on" })
}

function lightSwatch(light: Light) {
  if (light.kind === "cct") return kelvinSwatch(mixToKelvin(light.colorTemperature))
  if ((light.kind === "rgb" || light.kind === "rgbw") && light.color) return toHex(light.color)
  return "rgb(255 214 140)"
}

/** Map view narrowed to one zone: null = everything, 0 = things without a zone. */
type ZoneFilter = number | null

/**
 * Builds the graph. Rows follow the path of each button, so connected nodes line up. Buttons are
 * grouped by zone under a heading. With a zone filter a path stays when its button or its target is
 * in that zone, so a hallway switch for the living room still shows under "living room".
 */
function buildGraph(data: MapData, t: (key: TranslationKey) => string, zones: ZonesApi, filter: ZoneFilter, sort: ButtonSort, presets: LightPreset[]) {
  const nodes: MapNode[] = []
  const edges: Edge[] = []
  const rows: Record<Column, number> = { input: 0, gesture: 0, action: 0, target: 0 }
  const placed = new Set<string>()
  const relays = new Map(data.panel.targets.filter(item => item.group === "relay").map(item => [item.id, item]))

  const place = (id: string, column: Column, node: Omit<MapNodeData, "column" | "active">, minRow = 0) => {
    if (placed.has(id)) return
    placed.add(id)
    rows[column] = Math.max(rows[column], minRow)
    nodes.push({ id, type: "map", position: { x: columnX[column], y: rows[column] * rowHeight }, data: { ...node, column, active: false }, draggable: false })
    rows[column] += node.kind === "zone" ? 0.5 : 1
  }
  const link = (source: string, target: string) => {
    const id = `${source}->${target}`
    if (!edges.some(edge => edge.id === id)) edges.push({ id, source, target, type: "default" })  // curves: each line keeps its own path
  }
  const placeLight = (light: Light, row: number) => place(`tg:light-${light.id}`, "target", {
    kind: "light",
    title: light.name,
    subtitle: light.level > 0 ? `${Math.round(light.level * 100 / 255)}%` : t("outputOff"),
    icon: light.level > 0 ? Lightbulb : LightbulbOff, on: light.level > 0, glow: lightSwatch(light), open: { page: "lights", lightId: light.id },
    toggle: () => post("/api/v1/lights/level", { id: light.id, level: light.level > 0 ? 0 : "on" }),
  }, row)
  const placeRelay = (targetId: string, row: number) => {
    // "Module · channel" → channel as the title, module as the subtitle.
    const relay = relays.get(targetId)
    const [module, channel] = (relay?.name ?? targetId).split(" · ")
    place(`tg:${targetId}`, "target", {
      kind: "relay", title: channel ?? module, subtitle: channel ? module : undefined,
      icon: relay?.state ? ToggleRight : ToggleLeft, on: relay?.state, open: { page: "device", deviceId: targetId.split(".")[0] }, toggle: relayToggle(targetId, Boolean(relay?.state)),
    }, row)
  }

  const inZone = (target: string) => filter === null || zones.zoneOf(target) === filter
  const actionShown = (input: string, targetId: string) => inZone(input) || (Boolean(targetId) && inZone(targetId))
  const inputs = data.panel.inputs.filter(input => data.panel.actions.some(action => action.inputId === input.id && actionShown(input.id, action.targetId)) || inZone(input.id))
    .sort(compareButtons(sort))
  // Buttons follow the Actions page's sort; only "zone" groups them under headings.
  const showHeadings = sort === "zone" && zones.zones.length > 0
  const placeInput = (input: PanelInput) => {
    const inputRow = Math.max(rows.input, rows.gesture, rows.action)
    place(`in:${input.id}`, "input", { kind: "input", title: input.name, subtitle: input.hardwareName, icon: Hand, on: !input.high, open: { page: "actions", filter: `input:${input.id}` } }, inputRow)
    const events = data.panel.events.filter(item => item.inputId === input.id &&
      (inZone(input.id) || data.panel.actions.some(action => action.inputId === input.id && action.trigger === item.trigger && actionShown(input.id, action.targetId))))
      .sort(gestureOrder(data.panel.triggers))
    for (const event of events) {
      const gestureId = `ev:${event.id}`
      place(gestureId, "gesture", { kind: "gesture", title: t(triggerLabels[event.trigger] ?? "inputEventNone"), ...gestureIcons[event.trigger], open: { page: "actions", filter: `input:${input.id}`, highlight: `event-${event.id}` } }, Math.max(inputRow, rows.action))
      link(`in:${input.id}`, gestureId)
      for (const action of data.panel.actions.filter(item => item.inputId === input.id && item.trigger === event.trigger && actionShown(input.id, item.targetId))) {
        const actionId = `ac:${action.id}`
        place(actionId, "action", {
          kind: "action",
          title: action.command ? commandText(t, action.command, action.value, presets.find(preset => preset.id === action.value)?.name) : t("mapActionIncomplete"),
          subtitle: action.enabled ? undefined : t("mapActionDisabled"),
          icon: commandIcons[action.command] ?? Repeat2, dimmed: !action.enabled, open: { page: "actions", filter: `input:${input.id}`, highlight: `action-${action.id}` },
        }, rows.gesture - 1)
        link(gestureId, actionId)
        if (!action.targetId) continue
        const light = data.lights.find(item => `light-${item.id}` === action.targetId)
        const hook = data.panel.targets.find(item => item.id === action.targetId && item.group === "hook")
        if (light) placeLight(light, rows.action - 1)
        else if (hook) place(`tg:${hook.id}`, "target", { kind: "relay", title: hook.name, subtitle: "Webhook", icon: Globe, open: { page: "webhooks" } }, rows.action - 1)
        else placeRelay(action.targetId, rows.action - 1)
        link(actionId, `tg:${action.targetId}`)
      }
    }
  }
  for (const group of sort === "zone" ? groupByZone(inputs, input => zones.zoneOf(input.id), zones) : [{ zone: -1, name: "", items: inputs }]) {
    if (showHeadings) place(`zone:${group.zone}`, "input", { kind: "zone", title: group.name || t("zoneNone"), icon: MapPin }, Math.max(rows.input, rows.gesture, rows.action))
    for (const input of group.items) placeInput(input)
  }
  // Lights no button controls yet still belong on the map, in zone order.
  const loose = data.lights.filter(light => light.kind !== "none" && inZone(`light-${light.id}`)).sort((left, right) => naturalCompare(left.name, right.name))
  for (const group of groupByZone(loose, light => zones.zoneOf(`light-${light.id}`), zones)) for (const light of group.items) placeLight(light, rows.target)
  return { nodes, edges: edges.filter(edge => placed.has(edge.source) && placed.has(edge.target)) }
}

/** Node IDs along the path of one gesture: input → gesture → enabled actions → targets. */
function gesturePath(data: MapData, gesture: LiveInputEvent) {
  const ids = new Set<string>([`in:${gesture.input}`])
  const event = data.panel.events.find(item => item.inputId === gesture.input && item.trigger === gesture.trigger)
  if (event) ids.add(`ev:${event.id}`)
  for (const action of data.panel.actions) {
    if (!event || action.inputId !== gesture.input || action.trigger !== gesture.trigger || !action.enabled) continue
    ids.add(`ac:${action.id}`)
    if (action.targetId) ids.add(`tg:${action.targetId}`)
  }
  return ids
}

const zoneFilterKey = "whisli-map-zone"

export function MapPage({ theme, onOpen }: { theme: "light" | "dark"; onOpen: (route: AppRoute) => void }) {
  const { t } = useI18n()
  const [data, setData] = useState<MapData | null>(null)
  const [error, setError] = useState(false)
  // Node IDs that light up, with the time they fade out.
  const [highlight, setHighlight] = useState<Map<string, number>>(new Map())
  // Buttons held down right now: they glow from press to release, before any gesture is recognised.
  const [held, setHeld] = useState<Set<string>>(new Set())
  // Sizes React Flow measured. Nodes are rebuilt on every highlight and refresh; without their size
  // React Flow hides them until measured again, which rapid button presses never let happen.
  const [measured, setMeasured] = useState<Map<string, { width: number; height: number }>>(new Map())
  function onNodesChange(changes: NodeChange<MapNode>[]) {
    const sizes = changes.filter(change => change.type === "dimensions" && change.dimensions)
    if (sizes.length === 0) return
    setMeasured(current => {
      const next = new Map(current)
      for (const change of sizes) if (change.type === "dimensions" && change.dimensions) next.set(change.id, change.dimensions)
      return next
    })
  }
  const dataRef = useRef<MapData | null>(null)
  const zones = useZones()
  const { presets } = usePresets()
  const [buttonSort] = useRememberedSort<ButtonSort>(buttonSortKey, buttonSorts, "zone")
  const [zoneFilter, setZoneFilter] = useState<ZoneFilter>(() => {
    try { const saved = localStorage.getItem(zoneFilterKey); return saved === null ? null : Number(saved) } catch { return null }
  })
  const chooseZone = (next: ZoneFilter) => {
    setZoneFilter(next)
    try { if (next === null) localStorage.removeItem(zoneFilterKey); else localStorage.setItem(zoneFilterKey, String(next)) } catch { /* not remembered */ }
  }
  // A remembered zone that was removed since: show everything.
  const activeFilter = zoneFilter !== null && zoneFilter !== 0 && !zones.zones.some(zone => zone.id === zoneFilter) ? null : zoneFilter

  async function refresh() {
    try {
      const [panel, { lights }] = await Promise.all([getJson<ActionPanelData>("/api/v1/action-panel"), getJson<{ lights: Light[] }>("/api/v1/lights")])
      const previous = dataRef.current
      const next = { panel, lights }
      dataRef.current = next
      setData(next)
      setError(false)
      // Lights changed from elsewhere (panel, another button): flash them and their output.
      if (previous) {
        const changed = lights.filter(light => {
          const before = previous.lights.find(item => item.id === light.id)
          return before && (before.level !== light.level || before.colorTemperature !== light.colorTemperature || JSON.stringify(before.color) !== JSON.stringify(light.color))
        })
        if (changed.length) flash(changed.map(light => `tg:light-${light.id}`))
      }
    } catch {
      setError(true)
    }
  }
  useLiveRefresh(refresh, ["devices", "lights", "actions"])

  // `zones` is a new object every render; rebuild only when its data changes.
  const graph = useMemo(() => data ? buildGraph(data, t, zones, activeFilter, buttonSort, presets) : { nodes: [], edges: [] }, [data, t, zones.zones, zones.assignments, activeFilter, buttonSort, presets])

  function flash(ids: Iterable<string>) {
    const until = Date.now() + highlightMs
    setHighlight(current => {
      const next = new Map(current)
      for (const id of ids) next.set(id, until)
      return next
    })
  }

  useEffect(() => subscribeInputEvents(event => {
    const inputNode = `in:${event.input}`
    if (event.trigger === "press" || event.trigger === "release") {
      setHeld(current => {
        const next = new Set(current)
        if (event.trigger === "press") next.add(inputNode)
        else next.delete(inputNode)
        return next
      })
      if (event.trigger === "release") flash([inputNode])  // fade out instead of switching off
      return
    }
    if (dataRef.current) flash(gesturePath(dataRef.current, event))
  }), [])

  // Fade highlights out.
  useEffect(() => {
    if (highlight.size === 0) return
    const timer = window.setTimeout(() => {
      const now = Date.now()
      setHighlight(current => new Map([...current].filter(([, until]) => until > now)))
    }, 200)
    return () => window.clearTimeout(timer)
  }, [highlight])

  const nodes = useMemo(() => graph.nodes.map(node => {
    const toggle = node.data.toggle
    return { ...node, measured: measured.get(node.id), data: {
      ...node.data, active: highlight.has(node.id) || held.has(node.id), held: held.has(node.id),
      toggle: toggle && (() => { flash([node.id]); return toggle() }),
    } }
  }), [graph, highlight, held, measured])
  const edges = useMemo(() => graph.edges.map(edge => {
    const active = highlight.has(edge.source) && highlight.has(edge.target)
    return { ...edge, animated: active, className: active ? "whisli-map-edge-active" : undefined }
  }), [graph, highlight])

  return <>
    <PageHeader eyebrow={t("automation")} title={t("navMap")} description={t("mapDescription")} />
    {error && <ErrorText>{t("mapLoadError")}</ErrorText>}
    {zones.zones.length > 0 && <div className="mt-4 flex flex-wrap gap-1.5" role="group" aria-label={t("zoneFilter")}>
      {[{ id: null, name: t("zoneAll") }, ...zones.zones, { id: 0, name: t("zoneNone") }].map(zone => <button key={String(zone.id)} type="button" aria-pressed={activeFilter === zone.id}
        className={cn("whisli-line rounded-full border px-3 py-1 text-xs transition-colors", activeFilter === zone.id ? "border-[var(--ink)] bg-[var(--ink)] text-[var(--page)]" : "hover:border-[var(--ink)]")}
        onClick={() => chooseZone(zone.id)}>{zone.name}</button>)}
    </div>}

    <div className="whisli-line mt-5 h-[calc(100vh-15rem)] min-h-[420px] overflow-hidden rounded-lg border">
      {!data ? <Muted className="p-4">{t("connecting")}</Muted> : <ReactFlow
        key={String(activeFilter)}  // a new zone filter fits the view again
        nodes={nodes}
        edges={edges}
        onNodesChange={onNodesChange}
        nodeTypes={nodeTypes}
        colorMode={theme}
        nodesConnectable={false}
        nodesDraggable={false}
        elementsSelectable={false}
        fitView
        fitViewOptions={{ padding: 0.15 }}
        minZoom={0.3}
        proOptions={{ hideAttribution: true }}
        onNodeClick={(_, node) => { if (node.data.open) onOpen(node.data.open) }}
      >
        <Background gap={20} size={1} />
        <Controls showInteractive={false} />
      </ReactFlow>}
    </div>
    <Muted className="mt-2 text-xs">{t("mapHint")}</Muted>
  </>
}
