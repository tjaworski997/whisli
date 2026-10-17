import { useEffect, useRef } from "react"

import { useI18n } from "@/i18n"
import { cn } from "@/lib/utils"
import type { RgbwColor } from "@/types"

const hex = (value: number) => value.toString(16).padStart(2, "0")

/** The colour the user sees: the white channel adds equally to red, green and blue. */
export function toHex({ red, green, blue, white }: RgbwColor) {
  return `#${[red, green, blue].map(channel => hex(Math.min(255, channel + white))).join("")}`
}

/** With a white channel, the common part of R, G and B goes to W (white extraction), so whites use the white LED. */
function fromHex(color: string, hasWhite: boolean): RgbwColor {
  const [red, green, blue] = [1, 3, 5].map(start => parseInt(color.slice(start, start + 2), 16))
  const white = hasWhite ? Math.min(red, green, blue) : 0
  return { red: red - white, green: green - white, blue: blue - white, white }
}

/** Colour picker for RGB and RGBW lights. */
export function RgbwPicker({ value, hasWhite, disabled, hideLabel = false, className, onChange, onCommit }: {
  value: RgbwColor
  hasWhite: boolean
  disabled?: boolean
  /** Without the "Colour" caption, where the row already says what it is. */
  hideLabel?: boolean
  className?: string
  onChange: (value: RgbwColor) => void
  onCommit: (value: RgbwColor) => void
}) {
  const { t } = useI18n()
  const color = toHex(value)
  // The native colour dialog reports every movement; send the colour once it settles.
  const commitTimer = useRef<number | undefined>(undefined)
  useEffect(() => () => window.clearTimeout(commitTimer.current), [])
  function pick(next: string) {
    const rgbw = fromHex(next, hasWhite)
    onChange(rgbw)
    window.clearTimeout(commitTimer.current)
    commitTimer.current = window.setTimeout(() => onCommit(rgbw), 300)
  }

  return <div className={cn("grid gap-2", className)}>
    {!hideLabel && <span className="text-xs font-medium">{t("lightColor")}</span>}
    <label className={cn("whisli-line flex h-9 w-fit items-center gap-3 rounded-md border pl-1.5 pr-3 transition-colors", disabled ? "opacity-50" : "cursor-pointer hover:border-[var(--ink)]")} title={t("lightPickColor")}>
      <span className="whisli-line size-6 shrink-0 rounded border" style={{ backgroundColor: color }} aria-hidden />
      <span className="whisli-muted font-mono text-xs uppercase">{color}</span>
      <input className="sr-only" type="color" disabled={disabled} aria-label={t("lightPickColor")} value={color} onChange={event => pick(event.currentTarget.value)} />
    </label>
  </div>
}
