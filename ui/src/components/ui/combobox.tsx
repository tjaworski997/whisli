import { Combobox as ComboboxPrimitive } from "@base-ui/react/combobox"
import { Check, ChevronDown } from "lucide-react"

import { cn } from "@/lib/utils"

/** `name` is searched and shown when chosen; `label` replaces it in the list (e.g. without the group's name); `hint` is a small code on the right. */
export type ComboboxItem = { id: string; name: string; label?: string; hint?: string; badge?: string; disabled?: boolean }
export type ComboboxGroup = { label: string; items: ComboboxItem[] }

/** Searchable single select with grouped options (shadcn "Combobox" on Base UI). */
export function Combobox({ groups, value, placeholder, searchPlaceholder, emptyText, disabled, className, "aria-label": ariaLabel, onValueChange }: {
  groups: ComboboxGroup[]
  value: string
  placeholder: string
  searchPlaceholder: string
  emptyText: string
  disabled?: boolean
  className?: string
  "aria-label"?: string
  onValueChange: (id: string) => void
}) {
  const items = groups.flatMap(group => group.items)
  const selected = items.find(item => item.id === value) ?? null
  const visibleGroups = groups.filter(group => group.items.length > 0)

  return <ComboboxPrimitive.Root
    items={visibleGroups}
    value={selected}
    disabled={disabled}
    itemToStringLabel={(item: ComboboxItem) => item.name}
    isItemEqualToValue={(item: ComboboxItem, current: ComboboxItem) => item.id === current.id}
    onValueChange={(item: ComboboxItem | null) => { if (item) onValueChange(item.id) }}
  >
    <ComboboxPrimitive.Trigger aria-label={ariaLabel} className={cn("whisli-select flex w-full items-center justify-between gap-2 text-left data-disabled:opacity-50", className)}>
      <ComboboxPrimitive.Value>{(item: ComboboxItem | null) => <span className={cn("flex min-w-0 items-baseline gap-2", !item && "whisli-muted")}>
        <span className="truncate">{item?.name ?? placeholder}</span>
        {item?.hint && <span className="whisli-muted shrink-0 font-mono text-[10px]">{item.hint}</span>}
      </span>}</ComboboxPrimitive.Value>
      <ChevronDown className="whisli-muted size-4 shrink-0" />
    </ComboboxPrimitive.Trigger>
    <ComboboxPrimitive.Portal>
      <ComboboxPrimitive.Positioner sideOffset={4} align="start" className="z-50 outline-none">
        <ComboboxPrimitive.Popup className="whisli-popover w-[var(--anchor-width)] min-w-56 overflow-hidden">
          <div className="whisli-line border-b p-1">
            <ComboboxPrimitive.Input placeholder={searchPlaceholder} className="h-8 w-full bg-transparent px-2 text-sm outline-none" />
          </div>
          <ComboboxPrimitive.Empty className="whisli-muted px-3 py-4 text-center text-sm empty:hidden">{emptyText}</ComboboxPrimitive.Empty>
          <ComboboxPrimitive.List className="max-h-72 overflow-y-auto p-1 empty:p-0">
            {(group: ComboboxGroup, index: number) => <ComboboxPrimitive.Group key={group.label} items={group.items}>
              {index > 0 && <ComboboxPrimitive.Separator className="whisli-line -mx-1 my-1 border-t" />}
              <ComboboxPrimitive.GroupLabel className="whisli-muted px-2 py-1.5 text-xs">{group.label}</ComboboxPrimitive.GroupLabel>
              <ComboboxPrimitive.Collection>
                {(item: ComboboxItem) => <ComboboxPrimitive.Item key={item.id} value={item} disabled={item.disabled} className="flex cursor-default items-center gap-2 rounded-sm px-2 py-1.5 text-sm outline-none select-none data-highlighted:bg-[var(--raised)] data-disabled:opacity-45">
                  <span className="min-w-0 flex-1 truncate">{item.label ?? item.name}</span>
                  {item.badge && <span className="whisli-line shrink-0 rounded-full border px-1.5 py-px text-[10px]">{item.badge}</span>}
                  {item.hint && <span className="whisli-muted shrink-0 font-mono text-[10px]">{item.hint}</span>}
                  <ComboboxPrimitive.ItemIndicator><Check className="size-4" /></ComboboxPrimitive.ItemIndicator>
                </ComboboxPrimitive.Item>}
              </ComboboxPrimitive.Collection>
            </ComboboxPrimitive.Group>}
          </ComboboxPrimitive.List>
        </ComboboxPrimitive.Popup>
      </ComboboxPrimitive.Positioner>
    </ComboboxPrimitive.Portal>
  </ComboboxPrimitive.Root>
}
