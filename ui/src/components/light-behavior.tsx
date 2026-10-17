import { RotateCcw } from "lucide-react"
import type { ReactNode } from "react"

import { Checkbox } from "@/components/ui/checkbox"
import { useI18n, type TranslationKey } from "@/i18n"
import { cn } from "@/lib/utils"
import type { LightBehavior } from "@/types"

/** Groups that are saved and overridden together (they match the firmware's override fields). */
export type BehaviorGroup = "fadeIn" | "fadeOut" | "onLevel" | "maxLevel" | "restore"

/** API parameters for one group of `behavior`. */
export function groupParams(group: BehaviorGroup, behavior: LightBehavior): Record<string, string | number | boolean> {
  switch (group) {
    case "fadeIn": return { fadeIn: behavior.fadeIn, fadeInMs: behavior.fadeInMs }
    case "fadeOut": return { fadeOut: behavior.fadeOut, fadeOutMs: behavior.fadeOutMs }
    case "onLevel": return { onLevel: behavior.onLevel }
    case "maxLevel": return { maxLevel: behavior.maxLevel }
    case "restore": return { restore: behavior.restore }
  }
}

const groupKeys: Record<BehaviorGroup, (keyof LightBehavior)[]> = {
  fadeIn: ["fadeIn", "fadeInMs"], fadeOut: ["fadeOut", "fadeOutMs"], onLevel: ["onLevel"], maxLevel: ["maxLevel"], restore: ["restore"],
}

/** Whether a light's own settings override `group`. */
export const isOverridden = (own: Partial<LightBehavior>, group: BehaviorGroup) => groupKeys[group].some(key => key in own)

/**
 * Light behaviour editor. Without `own` it edits the defaults. With `own` (a light's overrides) every
 * group shows the default until changed: inherited values are muted, overridden ones get a button
 * that brings the default back. Relay lights only have the after-restart setting.
 */
export function BehaviorFields({ defaults, own, relay = false, disabled, onChange, onReset }: {
  defaults: LightBehavior
  own?: Partial<LightBehavior>
  relay?: boolean
  disabled?: boolean
  onChange: (group: BehaviorGroup, value: LightBehavior) => void
  onReset?: (group: BehaviorGroup) => void
}) {
  const { t } = useI18n()
  const value: LightBehavior = { ...defaults, ...own }
  const change = (group: BehaviorGroup, patch: Partial<LightBehavior>) => onChange(group, { ...value, ...patch })

  const row = (group: BehaviorGroup, label: TranslationKey, hint: TranslationKey, control: ReactNode) => {
    const inherited = own !== undefined && !isOverridden(own, group)
    return <div className="grid items-center gap-x-4 gap-y-1.5 py-2.5 sm:grid-cols-[13rem_minmax(0,1fr)_7rem]">
      <div className="min-w-0">
        <p className="text-sm font-medium">{t(label)}</p>
        <p className="whisli-muted text-xs">{t(hint)}</p>
      </div>
      <div className={cn("flex flex-wrap items-center gap-2 transition-opacity", inherited && "opacity-55 focus-within:opacity-100 hover:opacity-100")}>{control}</div>
      <div className="flex justify-end">
        {own !== undefined && !inherited && <button type="button" disabled={disabled} className="whisli-muted inline-flex items-center gap-1 text-xs hover:text-[var(--ink)] disabled:opacity-50" title={t("behaviorReset")} onClick={() => onReset?.(group)}>
          <RotateCcw className="size-3.5" />{t("behaviorReset")}
        </button>}
      </div>
    </div>
  }

  const milliseconds = (current: number, commit: (ms: number) => void, enabled: boolean) => <label className={cn("whisli-input flex h-9 w-28 items-center gap-1 py-0 focus-within:border-[var(--ink)]", !enabled && "opacity-50")}>
    <input key={current} className="min-w-0 flex-1 bg-transparent text-right tabular-nums outline-none" type="number" min={0} max={10000} step={100} disabled={disabled || !enabled} defaultValue={current}
      onBlur={event => { const next = Math.min(10000, Math.max(0, Math.round(Number(event.currentTarget.value) || 0))); if (next !== current) commit(next) }}
      onKeyDown={event => { if (event.key === "Enter") event.currentTarget.blur() }} />
    <span className="whisli-muted text-xs">ms</span>
  </label>
  const percent = (current: number, low: number, commit: (value: number) => void) => <label className="whisli-input flex h-9 w-24 items-center gap-1 py-0 focus-within:border-[var(--ink)]">
    <input key={current} className="min-w-0 flex-1 bg-transparent text-right tabular-nums outline-none" type="number" min={low} max={100} disabled={disabled} defaultValue={current}
      onBlur={event => { const next = Math.min(100, Math.max(low, Math.round(Number(event.currentTarget.value) || low))); if (next !== current) commit(next) }}
      onKeyDown={event => { if (event.key === "Enter") event.currentTarget.blur() }} />
    <span className="whisli-muted text-xs">%</span>
  </label>
  const toggle = (checked: boolean, commit: (checked: boolean) => void, label: TranslationKey) => <label className="inline-flex items-center gap-2 text-sm">
    <Checkbox checked={checked} disabled={disabled} onCheckedChange={next => commit(next === true)} />{t(label)}
  </label>

  return <div className="whisli-line divide-y divide-[var(--line)]">
    {!relay && <>
      {row("fadeIn", "behaviorFadeIn", "behaviorFadeInHint", <>
        {toggle(value.fadeIn, fadeIn => change("fadeIn", { fadeIn }), "behaviorSmooth")}
        {milliseconds(value.fadeInMs, fadeInMs => change("fadeIn", { fadeInMs }), value.fadeIn)}
      </>)}
      {row("fadeOut", "behaviorFadeOut", "behaviorFadeOutHint", <>
        {toggle(value.fadeOut, fadeOut => change("fadeOut", { fadeOut }), "behaviorSmooth")}
        {milliseconds(value.fadeOutMs, fadeOutMs => change("fadeOut", { fadeOutMs }), value.fadeOut)}
      </>)}
      {row("onLevel", "behaviorOnLevel", "behaviorOnLevelHint", <>
        <select className="whisli-select h-9" disabled={disabled} value={value.onLevel === "last" ? "last" : "fixed"}
          onChange={event => change("onLevel", { onLevel: event.currentTarget.value === "last" ? "last" : 100 })}>
          <option value="last">{t("behaviorOnLevelLast")}</option>
          <option value="fixed">{t("behaviorOnLevelFixed")}</option>
        </select>
        {value.onLevel !== "last" && percent(value.onLevel, 1, onLevel => change("onLevel", { onLevel }))}
      </>)}
      {row("maxLevel", "behaviorMaxLevel", "behaviorMaxLevelHint", percent(value.maxLevel, 10, maxLevel => change("maxLevel", { maxLevel })))}
    </>}
    {row("restore", "behaviorRestore", "behaviorRestoreHint", <select className="whisli-select h-9" disabled={disabled} value={value.restore ? "restore" : "off"}
      onChange={event => change("restore", { restore: event.currentTarget.value === "restore" })}>
      <option value="off">{t("behaviorRestoreOff")}</option>
      <option value="restore">{t("behaviorRestoreOn")}</option>
    </select>)}
  </div>
}
