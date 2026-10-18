import { ChevronRight, ChevronsDownUp, ChevronsUpDown, Play, Plus, Power, PowerOff, Search, Trash2, X, Zap } from "lucide-react"
import { createContext, useContext, useEffect, useId, useMemo, useRef, useState } from "react"

import { ErrorText, Muted, PageHeader } from "@/components/page"
import { SortMenu } from "@/components/sort-menu"
import { IconMenu, IconMenuItem } from "@/components/ui/icon-menu"
import { useCollapsedZones, ZoneHeading, ZonePicker } from "@/components/zone-picker"
import { Button } from "@/components/ui/button"
import { Checkbox } from "@/components/ui/checkbox"
import { Combobox } from "@/components/ui/combobox"
import { Dialog } from "@/components/ui/dialog"
import { MessageBox } from "@/components/ui/message-box"
import { useLiveRefresh } from "@/hooks/use-live-refresh"
import { presetKindFor, usePresets } from "@/hooks/use-presets"
import { groupByZone, useZones, type ZonesApi } from "@/hooks/use-zones"
import { useI18n, type TranslationKey } from "@/i18n"
import { getJson, post, type ApiParams } from "@/lib/api"
import { buttonSortKey, buttonSorts, compareButtons, gestureOrder, useRememberedSort, type ButtonSort } from "@/lib/sort"
import { actionCountKey } from "@/lib/format"
import { commandLabels, hookCommand, lightCommands, needsDimmable, presetLightKinds, switchCommands, triggerDescriptions, triggerLabels, valueCommands } from "@/lib/labels"
import type { ActionsFilter, ActionsHighlight } from "@/lib/route"
import { cn } from "@/lib/utils"
import type { ActionPanelData, LightPreset, PanelAction, PanelEvent, PanelInput, PanelTarget } from "@/types"

type Mutations = {
  busy: boolean
  addAction: (inputId: string, trigger: string) => void
  updateAction: (action: PanelAction, change: Partial<Pick<PanelAction, "command" | "targetId" | "value">>) => void
  setActionEnabled: (id: number, enabled: boolean) => void
  testAction: (id: number) => void
  removeAction: (id: number) => void
  removeEvent: (event: PanelEvent, hasActions: boolean) => void
}

/** Keeps only one button, or only the events and actions that drive one target. */
function narrow(data: ActionPanelData, filter: ActionsFilter | undefined): ActionPanelData {
  if (!filter) return data
  if (filter.startsWith("input:")) {
    const inputId = filter.slice(6)
    return { ...data, inputs: data.inputs.filter(input => input.id === inputId) }
  }
  const targetId = filter.slice(7)
  const actions = data.actions.filter(action => action.targetId === targetId)
  const events = data.events.filter(event => actions.some(action => action.inputId === event.inputId && action.trigger === event.trigger))
  return { ...data, actions, events, inputs: data.inputs.filter(input => events.some(event => event.inputId === input.id)) }
}

/** Scrolls a pointed-out row into view once, when it first appears. */
function useScrollIntoView<T extends HTMLElement>(active: boolean) {
  const ref = useRef<T>(null)
  useEffect(() => { if (active) ref.current?.scrollIntoView({ block: "center", behavior: "smooth" }) }, [active])
  return ref
}

export function ActionsPage({ filter, highlight, onClearFilter, onConfigureInputs }: { filter?: ActionsFilter; highlight?: ActionsHighlight; onClearFilter: () => void; onConfigureInputs: () => void }) {
  const { t } = useI18n()
  const [data, setData] = useState<ActionPanelData | null>(null)
  const [loadError, setLoadError] = useState(false)
  const [actionError, setActionError] = useState<TranslationKey | null>(null)
  const [busy, setBusy] = useState(false)
  const [search, setSearch] = useState("")
  const [addingEventsFor, setAddingEventsFor] = useState<PanelInput | null>(null)
  const [eventToRemove, setEventToRemove] = useState<PanelEvent | null>(null)
  // Optimistic edits bump the revision so a poll started before the edit cannot overwrite it.
  const pausedRef = useRef(false)
  const revisionRef = useRef(0)
  const zones = useZones()
  const folded = useCollapsedZones("whisli-actions-collapsed")
  const { presets } = usePresets()
  const [sort, setSort] = useRememberedSort<ButtonSort>(buttonSortKey, buttonSorts, "zone")
  const view = useMemo(() => data && narrow(data, filter), [data, filter])
  // Search also finds buttons by their zone's name.
  const filteredInputs = (view?.inputs ?? []).filter(input =>
    `${input.name} ${input.hardwareName} ${zones.zoneName(zones.zoneOf(input.id))}`.toLocaleLowerCase().includes(search.trim().toLocaleLowerCase()))
    .sort(compareButtons(sort))
  const groups = sort === "zone" ? groupByZone(filteredInputs, input => zones.zoneOf(input.id), zones) : [{ zone: -1, name: "", items: filteredInputs }]
  // Buttons start collapsed to a one-line summary. Opening from the map, searching or a pointed-out
  // row shows the details; what the user opened is remembered in this browser.
  const [expanded, setExpanded] = useState<Set<string>>(() => {
    try { return new Set(JSON.parse(localStorage.getItem(expandedKey) ?? "[]") as string[]) } catch { return new Set() }
  })
  const setExpandedAndRemember = (next: Set<string>) => {
    setExpanded(next)
    try { localStorage.setItem(expandedKey, JSON.stringify([...next])) } catch { /* private mode: not remembered */ }
  }
  const toggleInput = (id: string) => { const next = new Set(expanded); if (next.has(id)) next.delete(id); else next.add(id); setExpandedAndRemember(next) }
  const forceExpanded = Boolean(filter || highlight || search.trim())
  const isExpanded = (id: string) => forceExpanded || expanded.has(id)
  const allExpanded = filteredInputs.length > 0 && filteredInputs.every(input => isExpanded(input.id))
  const filterTitle = !filter || !data ? "" : filter.startsWith("input:")
    ? data.inputs.find(input => input.id === filter.slice(6))?.name ?? filter.slice(6)
    : data.targets.find(target => target.id === filter.slice(7))?.name ?? filter.slice(7)

  async function refresh() {
    if (pausedRef.current) return
    const revision = revisionRef.current
    try {
      const next = await getJson<ActionPanelData>("/api/v1/action-panel")
      if (revision !== revisionRef.current) return
      setData(next)
      setLoadError(false)
    } catch {
      setLoadError(true)
    }
  }
  useLiveRefresh(refresh, ["actions"])

  async function mutate(errorKey: TranslationKey, request: () => Promise<boolean>, optimistic?: (data: ActionPanelData) => ActionPanelData) {
    setBusy(true)
    if (optimistic) {
      pausedRef.current = true
      revisionRef.current += 1
      setData(current => current && optimistic(current))
    }
    try {
      const ok = await request()
      pausedRef.current = false
      await refresh()
      setActionError(ok ? null : errorKey)
      return ok
    } finally {
      pausedRef.current = false
      setBusy(false)
    }
  }

  const call = (path: string, params: ApiParams) => async () => (await post(path, params)).ok

  const mutations: Mutations = {
    busy,
    addAction: (inputId, trigger) => void mutate("actionSaveError", call("/api/v1/actions", { inputId, trigger, command: "", targetId: "", enabled: true })),
    updateAction: (action, change) => {
      const next = { ...action, ...change }
      // A new command starts with its own default percentage (the controller picks it).
      const params: ApiParams = { id: next.id, command: next.command, targetId: next.targetId }
      if (change.value !== undefined || (!change.command && valueCommands.includes(next.command))) params.value = next.value
      void mutate("actionUpdateError", call("/api/v1/actions/update", params),
        current => ({ ...current, actions: current.actions.map(item => item.id === next.id ? next : item) }))
    },
    setActionEnabled: (id, enabled) => void mutate("actionSaveError", call("/api/v1/actions/enabled", { id, enabled })),
    testAction: id => void mutate("actionTestError", call("/api/v1/actions/test", { id })),
    removeAction: id => void mutate("actionRemoveError", call("/api/v1/actions/remove", { id })),
    removeEvent: (event, hasActions) => {
      if (hasActions) setEventToRemove(event)
      else void mutate("eventRemoveError", call("/api/v1/events/remove", { id: event.id }))
    },
  }

  async function addEvents(input: PanelInput, triggers: string[]) {
    const added = await mutate("eventSaveError", async () => {
      for (const trigger of triggers) {
        if (!(await post("/api/v1/events", { inputId: input.id, trigger })).ok) return false
      }
      return true
    })
    if (added) { setAddingEventsFor(null); setExpandedAndRemember(new Set([...expanded, input.id])) }
  }

  async function confirmRemoveEvent() {
    if (!eventToRemove) return
    await mutate("eventRemoveError", call("/api/v1/events/remove", { id: eventToRemove.id }))
    setEventToRemove(null)
  }

  return <PresetsContext.Provider value={presets}>
    <PageHeader eyebrow={t("automation")} title={t("buttonActionsTitle")} description={t("buttonActionsDescription")} actions={<>
      <label className="relative min-w-0 flex-1 sm:w-72 sm:flex-none"><Search className="whisli-muted pointer-events-none absolute left-3 top-1/2 size-4 -translate-y-1/2" /><input className="whisli-input whisli-input-with-icon h-10 w-full" type="search" value={search} onChange={event => setSearch(event.target.value)} placeholder={t("searchButtonsPlaceholder")} aria-label={t("searchButtons")} /></label>
    </>} />
    {/* Above the list: the active filter on the left, how the list is shown on the right. */}
    <div className="mt-5 flex min-h-8 items-center gap-1 text-sm">
      {filter && <>
        <span className="whisli-muted">{t(filter.startsWith("input:") ? "actionsFilterInput" : "actionsFilterTarget")}</span>
        <button className="whisli-line inline-flex items-center gap-1.5 rounded-full border px-3 py-1 font-medium hover:border-[var(--ink)]" title={t("clearFilter")} onClick={onClearFilter}>
          {filterTitle}<X className="size-3.5" />
        </button>
      </>}
      <span className="flex-1" />
      <SortMenu value={sort} options={buttonSortOptions} onChange={setSort} />
      {!forceExpanded && filteredInputs.length > 1 && <IconMenu icon={allExpanded ? ChevronsDownUp : ChevronsUpDown} label={t("actionsExpandMenu")}>
        <IconMenuItem icon={ChevronsUpDown} onClick={() => setExpandedAndRemember(new Set(filteredInputs.map(input => input.id)))}>{t("actionsExpandAll")}</IconMenuItem>
        <IconMenuItem icon={ChevronsDownUp} onClick={() => setExpandedAndRemember(new Set())}>{t("actionsCollapseAll")}</IconMenuItem>
      </IconMenu>}
    </div>
    {loadError && <ErrorText>{t("actionPanelLoadError")}</ErrorText>}
    {actionError && <ErrorText>{t(actionError)}</ErrorText>}
    {!data ? <Muted className="mt-6">{t("actionPanelLoading")}</Muted>
      : data.inputs.length === 0 ? <div className="whisli-line mt-3 border-y py-5"><p>{t("noActiveInputs")}</p><Button className="mt-3" variant="outline" onClick={onConfigureInputs}>{t("configureInputs")}</Button></div>
      : filteredInputs.length === 0 ? <Muted className="whisli-line mt-3 border-y py-5">{t(filter?.startsWith("target:") ? "actionsFilterTargetEmpty" : "noButtonsMatch")}</Muted>
      : <div className="whisli-line mt-3 border-t">{groups.map(group => <div key={group.zone}>
        {group.zone >= 0 && zones.zones.length > 0 && <ZoneHeading name={group.name || t("zoneNone")} count={group.items.length} collapsed={folded.isCollapsed(group.zone)} onToggle={() => folded.toggle(group.zone)} />}
        {(group.zone < 0 || !folded.isCollapsed(group.zone) || forceExpanded) && <div className={cn(group.zone >= 0 && zones.zones.length > 0 && "pl-4 sm:pl-6")}>
          {group.items.map(input => <InputSection key={input.id} input={input} data={view ?? data} zones={zones} highlight={highlight} expanded={isExpanded(input.id)} onToggle={forceExpanded ? undefined : () => toggleInput(input.id)} mutations={mutations} onAddEvents={() => setAddingEventsFor(input)} />)}
        </div>}
      </div>)}</div>}

    {addingEventsFor && data && <AddEventsDialog
      inputName={addingEventsFor.name}
      triggers={data.triggers.filter(trigger => !data.events.some(event => event.inputId === addingEventsFor.id && event.trigger === trigger))}
      busy={busy}
      onClose={() => setAddingEventsFor(null)}
      onConfirm={triggers => void addEvents(addingEventsFor, triggers)}
    />}
    <MessageBox
      open={eventToRemove !== null}
      type="danger"
      title={t("removeEventWithActionsTitle")}
      message={t("removeEventWithActionsMessage")}
      confirmLabel={t("removeEventAndActions")}
      cancelLabel={t("cancel")}
      busy={busy}
      onConfirm={() => void confirmRemoveEvent()}
      onClose={() => setEventToRemove(null)}
    />
  </PresetsContext.Provider>
}

/** Presets for the action rows, loaded once by the page. */
const PresetsContext = createContext<LightPreset[]>([])

const expandedKey = "whisli-actions-expanded"
const buttonSortOptions: { value: ButtonSort; label: TranslationKey }[] = [
  { value: "zone", label: "sortZoneInput" }, { value: "name", label: "sortName" }, { value: "input", label: "sortInput" },
]

function InputSection({ input, data, zones, highlight, expanded, onToggle, mutations, onAddEvents }: { input: PanelInput; data: ActionPanelData; zones: ZonesApi; highlight?: ActionsHighlight; expanded: boolean; onToggle?: () => void; mutations: Mutations; onAddEvents: () => void }) {
  const { t } = useI18n()
  const events = data.events.filter(event => event.inputId === input.id).sort(gestureOrder(data.triggers))
  const actions = data.actions.filter(action => action.inputId === input.id)
  const pressed = !input.high
  const targetName = (id: string) => data.targets.find(target => target.id === id)?.name ?? (id ? t("actionTargetUnavailable") : "—")
  return <section className="whisli-line border-b">
    <div className={cn("flex flex-wrap items-center gap-3 py-3", onToggle && "cursor-pointer")} onClick={onToggle}>
      {onToggle && <ChevronRight className={cn("whisli-muted size-4 shrink-0 transition-transform", expanded && "rotate-90")} />}
      <div className="min-w-0 flex-1">
        <h2 className="flex items-center gap-2 font-medium">
          <span className={cn("size-2 shrink-0 rounded-full transition-colors", pressed ? "bg-[var(--status-ok)] shadow-[0_0_8px_var(--status-ok)]" : "bg-[var(--line)]")} title={pressed ? t("inputLow") : t("inputHigh")} />
          {input.name}
          <span className="whisli-raised whisli-line inline-flex items-center rounded-md border px-1.5 py-0.5 font-mono text-[10px] font-normal leading-none">{input.hardwareName}</span>
        </h2>
        {/* Collapsed: what each gesture does, in one glance. */}
        {!expanded && <div className="whisli-muted mt-1 grid gap-0.5 text-xs">
          {events.length === 0 ? <span>{t("noButtonEvents")}</span> : events.map(event => {
            const eventActions = actions.filter(action => action.trigger === event.trigger)
            return <span key={event.id} className="truncate">
              <span className="text-[var(--ink)]">{t(triggerLabels[event.trigger] ?? "inputEventNone")}</span>
              {" → "}{eventActions.length ? eventActions.map(action => targetName(action.targetId)).join(", ") : t("noEventActionsShort")}
            </span>
          })}
        </div>}
        {expanded && <p className="whisli-muted mt-1 text-xs">{input.high ? t("inputHigh") : t("inputLow")} · {t("lastEvent")}: {t(triggerLabels[input.lastEvent] ?? "inputEventNone")}</p>}
      </div>
      {!expanded && <span className="whisli-muted text-xs tabular-nums">{t(actionCountKey(actions.length), { count: actions.length })}</span>}
      {expanded && <div className="w-full sm:w-56" onClick={event => event.stopPropagation()}><ZonePicker target={input.id} zones={zones} className="h-9" /></div>}
      {expanded && <Button variant="outline" disabled={mutations.busy} onClick={event => { event.stopPropagation(); onAddEvents() }}>{t("addEvent")}</Button>}
    </div>
    {expanded && (events.length > 0
      ? <div className="whisli-line border-t pb-2 sm:pl-7">{events.map(event => <EventSection key={event.id} input={input} event={event} data={data} highlight={highlight} mutations={mutations} />)}</div>
      : <Muted className="whisli-line border-t py-3 text-xs sm:pl-7">{t("noButtonEvents")}</Muted>)}
  </section>
}

function EventSection({ input, event, data, highlight, mutations }: { input: PanelInput; event: PanelEvent; data: ActionPanelData; highlight?: ActionsHighlight; mutations: Mutations }) {
  const highlighted = highlight === `event-${event.id}`
  const ref = useScrollIntoView<HTMLElement>(highlighted)
  const { t } = useI18n()
  const actions = data.actions.filter(action => action.inputId === input.id && action.trigger === event.trigger)
  const addAction = () => mutations.addAction(input.id, event.trigger)
  return <section ref={ref} className={cn("whisli-line border-b py-3 last:border-b-0", highlighted && "whisli-highlight -mx-3 border-transparent px-3")}>
    <div className="flex flex-wrap items-center gap-3">
      <h3 className="min-w-0 flex-1 text-sm font-medium">{t(triggerLabels[event.trigger] ?? "inputEventNone")}</h3>
      <Button variant="ghost" size="sm" disabled={mutations.busy} onClick={addAction}><Plus className="size-3.5" />{t("addAction")}</Button>
      <Button variant="ghost" size="icon" disabled={mutations.busy} title={t("removeEvent")} aria-label={t("removeEvent")} onClick={() => mutations.removeEvent(event, actions.length > 0)}><Trash2 className="size-4" /></Button>
    </div>
    {actions.length === 0
      ? <button className="whisli-link w-full py-5 text-center text-sm disabled:opacity-50" disabled={mutations.busy} onClick={addAction}>{t("noEventActions")}</button>
      : <div className="whisli-line mt-1 divide-y divide-[var(--line)] border-l pl-3">{actions.map(action => <ActionRow key={action.id} action={action} data={data} highlighted={highlight === `action-${action.id}`} mutations={mutations} />)}</div>}
  </section>
}

function ActionRow({ action, data, highlighted, mutations }: { action: PanelAction; data: ActionPanelData; highlighted: boolean; mutations: Mutations }) {
  const ref = useScrollIntoView<HTMLDivElement>(highlighted)
  const { t } = useI18n()
  const target = data.targets.find(item => item.id === action.targetId)
  const sendsHook = action.command === hookCommand
  const { busy } = mutations
  // A preset suits a light of its kind; with no light chosen yet, all are offered.
  const presets = useContext(PresetsContext)
  const presetKind = presetKindFor(target?.kind)
  const presetOptions = presets.filter(preset => !target || preset.kind === presetKind)
  const chosenPreset = presets.find(preset => preset.id === action.value)
  // Phone: "active" and remove share the first line, the rest stacks below. Desktop: one row.
  return <div ref={ref} className={cn("grid grid-cols-[1fr_auto] items-start gap-2 py-2 sm:grid-cols-[auto_minmax(9rem,0.8fr)_minmax(12rem,1.6fr)_auto_auto]", highlighted && "whisli-highlight -ml-3 pl-3")}>
    <label className="whisli-muted inline-flex items-center gap-1.5 self-center text-xs sm:mt-2.5 sm:self-start">
      <Checkbox checked={action.enabled} disabled={busy} aria-label={t("actionEnabled")} onCheckedChange={checked => mutations.setActionEnabled(action.id, checked === true)} />
      <span>{t("actionActive")}</span>
    </label>
    <Button variant="ghost" size="icon" className="sm:order-last sm:mt-0.5" disabled={busy} aria-label={t("removeAction")} title={t("removeAction")} onClick={() => mutations.removeAction(action.id)}><Trash2 className="size-4" /></Button>
    {/* Brightness commands only for DMX lights. */}
    <div className="col-span-2 flex min-w-0 gap-2 sm:col-span-1">
      <select className="whisli-select min-w-0 flex-1" aria-label={t("actionCommand")} value={action.command} disabled={busy} onChange={event => mutations.updateAction(action, { command: event.currentTarget.value })}>
        <option value="">{t("chooseActionCommand")}</option>
        {commandGroups(target).map(group => <optgroup key={group.label} label={t(group.label)}>
          {group.commands.filter(command => data.commands.includes(command)).map(command => <option key={command} value={command}>{t(commandLabels[command] ?? "actionToggle")}</option>)}
        </optgroup>)}
      </select>
      {action.command === "preset" && <select className="whisli-select w-36 shrink-0" aria-label={t("actionPresetChoose")} value={action.value || ""} disabled={busy}
        onChange={event => mutations.updateAction(action, { value: Number(event.currentTarget.value) })}>
        <option value="">{t("actionPresetChoose")}</option>
        {presetOptions.map(preset => <option key={preset.id} value={preset.id}>{preset.name}</option>)}
      </select>}
      {valueCommands.includes(action.command) && <label className="whisli-input flex h-9 w-20 shrink-0 items-center gap-1 py-0 focus-within:border-[var(--ink)]">
        <input key={action.value} className="min-w-0 flex-1 bg-transparent text-right tabular-nums outline-none" type="number" min={1} max={100} disabled={busy} defaultValue={action.value} aria-label={t("actionPercent")}
          onBlur={event => { const value = Math.min(100, Math.max(1, Math.round(Number(event.currentTarget.value) || action.value))); if (value !== action.value) mutations.updateAction(action, { value }) }}
          onKeyDown={event => { if (event.key === "Enter") event.currentTarget.blur() }} />
        <span className="whisli-muted text-xs">%</span>
      </label>}
    </div>
    <div className="col-span-2 min-w-0 sm:col-span-1">
      <Combobox
        aria-label={t("actionTarget")}
        disabled={busy}
        value={action.targetId}
        placeholder={action.targetId && !target ? t("actionTargetUnavailable") : t(sendsHook ? "chooseActionHook" : "chooseActionTarget")}
        searchPlaceholder={t("searchActionTarget")}
        emptyText={t("noActionTargetMatch")}
        groups={sendsHook ? [{ label: t("actionTargetHooks"), items: data.targets.filter(item => item.group === "hook") }] : [
          { label: t("lights"), items: data.targets.filter(item => item.group === "light" && (!needsDimmable(action.command) || item.dimmable) &&
            (action.command !== "preset" || (chosenPreset ? presetKindFor(item.kind) === chosenPreset.kind : presetLightKinds.includes(item.kind ?? "")))) },
          { label: t("actionTargetRelays"), items: needsDimmable(action.command) ? [] : data.targets.filter(item => item.group === "relay") },
          { label: t("actionTargetHooks"), items: needsDimmable(action.command) ? [] : data.targets.filter(item => item.group === "hook") },
        ]}
        onValueChange={targetId => {
          // A webhook is only called, and "call" only goes to one: switch the command with the target.
          const toHook = data.targets.find(item => item.id === targetId)?.group === "hook"
          if (toHook && action.command !== hookCommand) mutations.updateAction(action, { targetId, command: hookCommand })
          else if (!toHook && action.command === hookCommand) mutations.updateAction(action, { targetId, command: "toggle" })
          else mutations.updateAction(action, { targetId })
        }}
      />
      {target && target.group !== "hook" && <span className="whisli-muted mt-1 inline-flex items-center gap-1.5 text-[10px] leading-4">
        {target.state ? <Power className="size-3" /> : <PowerOff className="size-3" />}
        {t("relayState")}: {target.state ? t("outputOn") : t("outputOff")}
        {!target.online && <> · {t("relayOffline")}</>}
      </span>}
    </div>
    <Button variant="outline" className="col-span-2 sm:col-span-1" disabled={busy || !action.command || !target?.online} onClick={() => mutations.testAction(action.id)}><Play className="size-3.5" />{t("testAction")}</Button>
  </div>
}

/** The commands one action may use, given its target: light commands for DMX lights, presets for colour or white ones. */
function commandGroups(target: PanelTarget | undefined): { label: TranslationKey; commands: string[] }[] {
  if (target?.group === "hook") return [{ label: "actionGroupHttp", commands: [hookCommand] }]
  const light = !target || target.dimmable
  const commands = lightCommands.filter(command => command !== "preset" || !target || presetLightKinds.includes(target.kind ?? ""))
  return [
    { label: "actionGroupSwitch", commands: switchCommands },
    ...(light ? [{ label: "actionGroupBrightness" as TranslationKey, commands }] : []),
    ...(target ? [] : [{ label: "actionGroupHttp" as TranslationKey, commands: [hookCommand] }]),
  ]
}

function AddEventsDialog({ inputName, triggers, busy, onClose, onConfirm }: { inputName: string; triggers: string[]; busy: boolean; onClose: () => void; onConfirm: (triggers: string[]) => void }) {
  const { t } = useI18n()
  const id = useId()
  const [selected, setSelected] = useState<string[]>([])
  const toggle = (trigger: string) => setSelected(current => current.includes(trigger) ? current.filter(item => item !== trigger) : [...current, trigger])

  return <Dialog open busy={busy} onClose={onClose} labelledBy={`${id}-title`}>
    <div className="flex items-start gap-3">
      <span className="whisli-raised flex size-9 shrink-0 items-center justify-center rounded-full"><Zap className="size-4" /></span>
      <div className="min-w-0">
        <h2 id={`${id}-title`} className="font-semibold">{t("addEventsTitle")}</h2>
        <Muted className="mt-1 text-sm">{t("addEventsDescription")}</Muted>
        <p className="mt-2 text-sm font-medium">{inputName}</p>
      </div>
    </div>
    <div className="mt-5 max-h-80 space-y-2 overflow-y-auto">
      {triggers.length === 0 ? <Muted className="whisli-line rounded-md border p-3 text-sm">{t("noAvailableEvents")}</Muted> : triggers.map(trigger => {
        const checked = selected.includes(trigger)
        return <label key={trigger} className={`whisli-raised flex cursor-pointer items-start gap-3 rounded-md border p-3 transition-colors ${checked ? "border-[var(--ink)]" : "border-[var(--line)]"}`}>
          <Checkbox className="mt-0.5" checked={checked} disabled={busy} onCheckedChange={() => toggle(trigger)} />
          <span className="min-w-0"><span className="block text-sm font-medium">{t(triggerLabels[trigger] ?? "inputEventNone")}</span>{triggerDescriptions[trigger] && <span className="whisli-muted mt-0.5 block text-xs">{t(triggerDescriptions[trigger])}</span>}</span>
        </label>
      })}
    </div>
    <div className="mt-5 flex justify-end gap-2">
      <Button variant="outline" disabled={busy} onClick={onClose}>{t("cancel")}</Button>
      <Button variant="outline" disabled={busy || selected.length === 0} onClick={() => onConfirm(selected)}>{busy ? t("saving") : t("addSelectedEvents")}</Button>
    </div>
  </Dialog>
}
