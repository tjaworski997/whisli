import { Menu } from "@base-ui/react/menu"
import { Check, type LucideIcon } from "lucide-react"
import type { ReactNode } from "react"

import { cn } from "@/lib/utils"

const itemClass = "flex cursor-default items-center gap-2 rounded-sm px-2 py-1.5 text-sm outline-none select-none data-highlighted:bg-[var(--raised)]"

/** Icon button that opens a small menu (shadcn "Dropdown Menu" on Base UI). */
export function IconMenu({ icon: Icon, label, active = false, children }: { icon: LucideIcon; label: string; active?: boolean; children: ReactNode }) {
  return <Menu.Root>
    <Menu.Trigger aria-label={label} title={label}
      className={cn("whisli-muted inline-flex size-8 shrink-0 items-center justify-center rounded-md transition-colors hover:bg-[var(--raised)] hover:text-[var(--ink)] data-popup-open:bg-[var(--raised)] data-popup-open:text-[var(--ink)]", active && "text-[var(--status-ok)]")}>
      <Icon className="size-4" />
    </Menu.Trigger>
    <Menu.Portal>
      <Menu.Positioner sideOffset={4} align="end" className="z-50 outline-none">
        <Menu.Popup className="whisli-popover min-w-52 p-1 outline-none">{children}</Menu.Popup>
      </Menu.Positioner>
    </Menu.Portal>
  </Menu.Root>
}

/** A titled list of choices, one of them ticked. */
export function IconMenuChoice<T extends string>({ title, value, options, onChange }: { title: string; value: T; options: { value: T; label: string }[]; onChange: (value: T) => void }) {
  return <Menu.Group>
    <Menu.GroupLabel className="whisli-muted px-2 py-1.5 text-xs">{title}</Menu.GroupLabel>
    <Menu.RadioGroup value={value} onValueChange={next => onChange(next as T)}>
      {options.map(option => <Menu.RadioItem key={option.value} value={option.value} closeOnClick className={itemClass}>
        <span className="min-w-0 flex-1 truncate">{option.label}</span>
        <Menu.RadioItemIndicator><Check className="size-4" /></Menu.RadioItemIndicator>
      </Menu.RadioItem>)}
    </Menu.RadioGroup>
  </Menu.Group>
}

export function IconMenuItem({ icon: Icon, children, onClick }: { icon?: LucideIcon; children: ReactNode; onClick: () => void }) {
  return <Menu.Item className={itemClass} onClick={onClick}>{Icon && <Icon className="whisli-muted size-4" />}<span className="flex-1">{children}</span></Menu.Item>
}
