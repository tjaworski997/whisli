import { Combobox as ComboboxPrimitive } from "@base-ui/react/combobox"
import { Check, ChevronDown, ChevronRight, MapPin, Plus } from "lucide-react"
import { useState } from "react"

import type { ZonesApi } from "@/hooks/use-zones"
import { useI18n } from "@/i18n"
import { cn } from "@/lib/utils"

type Option = { id: number; name: string; create?: string }

/**
 * Zone of one thing (a light, a button). Searchable; typing a name that does not exist offers to
 * create it right here, so nobody has to visit the Zones page first.
 */
export function ZonePicker({ target, zones, disabled, className }: { target: string; zones: ZonesApi; disabled?: boolean; className?: string }) {
  const { t } = useI18n()
  const [query, setQuery] = useState("")
  const typed = query.trim()
  const exists = zones.zones.some(zone => zone.name.toLocaleLowerCase() === typed.toLocaleLowerCase())
  const options: Option[] = [
    { id: 0, name: t("zoneNone") },
    ...zones.zones,
    ...(typed && !exists ? [{ id: -1, name: t("zoneCreate", { name: typed }), create: typed }] : []),
  ]
  const current = zones.zoneOf(target)
  const selected = options.find(option => option.id === current) ?? options[0]

  async function choose(option: Option | null) {
    if (!option) return
    const zone = option.create ? await zones.create(option.create) : option.id
    if (zone >= 0) await zones.assign(target, zone)
    setQuery("")
  }

  return <ComboboxPrimitive.Root
    items={options}
    value={selected}
    disabled={disabled}
    inputValue={query}
    onInputValueChange={setQuery}
    autoHighlight  // Enter takes the first match, including "Create …"
    itemToStringLabel={(option: Option) => option.name}
    isItemEqualToValue={(option: Option, value: Option) => option.id === value.id}
    onValueChange={(option: Option | null) => void choose(option)}
  >
    <ComboboxPrimitive.Trigger aria-label={t("zone")} className={cn("whisli-select flex w-full items-center gap-2 text-left data-disabled:opacity-50", className)}>
      <MapPin className="whisli-muted size-4 shrink-0" />
      <ComboboxPrimitive.Value>{(option: Option | null) => <span className={cn("min-w-0 flex-1 truncate", !option?.id && "whisli-muted")}>{option?.name ?? t("zoneNone")}</span>}</ComboboxPrimitive.Value>
      <ChevronDown className="whisli-muted size-4 shrink-0" />
    </ComboboxPrimitive.Trigger>
    <ComboboxPrimitive.Portal>
      <ComboboxPrimitive.Positioner sideOffset={4} align="start" className="z-50 outline-none">
        <ComboboxPrimitive.Popup className="whisli-popover w-[var(--anchor-width)] min-w-56 overflow-hidden">
          <div className="whisli-line border-b p-1">
            <ComboboxPrimitive.Input placeholder={t("zoneSearch")} className="h-8 w-full bg-transparent px-2 text-sm outline-none" />
          </div>
          <ComboboxPrimitive.Empty className="whisli-muted px-3 py-4 text-center text-sm empty:hidden">{t("zoneEmpty")}</ComboboxPrimitive.Empty>
          <ComboboxPrimitive.List className="max-h-72 overflow-y-auto p-1 empty:p-0">
            {(option: Option) => <ComboboxPrimitive.Item key={option.id} value={option}
              className={cn("flex cursor-default items-center gap-2 rounded-sm px-2 py-1.5 text-sm outline-none select-none data-highlighted:bg-[var(--raised)]", option.create && "font-medium", option.id === 0 && "whisli-muted")}>
              {option.create && <Plus className="size-4 shrink-0" />}
              <span className="min-w-0 flex-1 truncate">{option.name}</span>
              <ComboboxPrimitive.ItemIndicator><Check className="size-4" /></ComboboxPrimitive.ItemIndicator>
            </ComboboxPrimitive.Item>}
          </ComboboxPrimitive.List>
        </ComboboxPrimitive.Popup>
      </ComboboxPrimitive.Positioner>
    </ComboboxPrimitive.Portal>
  </ComboboxPrimitive.Root>
}

/** Heading of a zone's group in lists: a light grey band that folds the group away and back. */
export function ZoneHeading({ name, count, collapsed, onToggle }: { name: string; count: number; collapsed: boolean; onToggle: () => void }) {
  return <h2 className="mt-4">
    <button type="button" aria-expanded={!collapsed} onClick={onToggle}
      className="whisli-raised whisli-line flex w-full items-center gap-2 rounded-md border-b px-3 py-2 text-left text-sm font-semibold uppercase tracking-wide text-[var(--ink)]">
      <ChevronRight className={cn("size-4 shrink-0 transition-transform", !collapsed && "rotate-90")} />
      <MapPin className="size-4 shrink-0" />
      <span className="min-w-0 flex-1 truncate">{name}</span>
      <span className="whisli-muted text-xs font-normal tabular-nums normal-case">{count}</span>
    </button>
  </h2>
}

/** Zones folded away on one page, remembered in this browser. Everything starts unfolded. */
export function useCollapsedZones(storageKey: string) {
  const [collapsed, setCollapsed] = useState<Set<number>>(() => {
    try { return new Set(JSON.parse(localStorage.getItem(storageKey) ?? "[]") as number[]) } catch { return new Set() }
  })
  const toggle = (zone: number) => setCollapsed(current => {
    const next = new Set(current)
    if (next.has(zone)) next.delete(zone); else next.add(zone)
    try { localStorage.setItem(storageKey, JSON.stringify([...next])) } catch { /* not remembered */ }
    return next
  })
  return { isCollapsed: (zone: number) => collapsed.has(zone), toggle }
}
