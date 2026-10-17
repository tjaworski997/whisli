import { Slider as SliderPrimitive } from "@base-ui/react/slider"
import type { SliderRootProps } from "@base-ui/react/slider"
import type { ReactNode } from "react"

import { cn } from "@/lib/utils"

type SliderProps = Omit<SliderRootProps<number[]>, "value" | "onValueChange" | "onValueCommitted"> & {
  value: number[]
  label: string
  valueLabel: ReactNode
  onValueChange: (value: number[]) => void
  onValueCommitted?: (value: number[]) => void
}

export function Slider({ className, value, label, valueLabel, onValueChange, onValueCommitted, ...props }: SliderProps) {
  return <SliderPrimitive.Root
    className={cn("grid w-full gap-2 data-disabled:opacity-50", className)}
    value={value}
    onValueChange={onValueChange}
    onValueCommitted={onValueCommitted}
    {...props}
  >
    <div className="flex items-center justify-between gap-3 text-xs">
      <SliderPrimitive.Label className="font-medium text-[var(--ink)]">{label}</SliderPrimitive.Label>
      <SliderPrimitive.Value className="whisli-muted inline-flex items-center gap-2 tabular-nums">{() => valueLabel}</SliderPrimitive.Value>
    </div>
    <SliderPrimitive.Control className="relative flex w-full items-center py-2">
      <SliderPrimitive.Track className="relative h-1.5 w-full grow overflow-hidden rounded-full bg-[var(--line)]">
        <SliderPrimitive.Indicator className="absolute h-full rounded-full bg-[var(--ink)]" />
      </SliderPrimitive.Track>
      <SliderPrimitive.Thumb
        getAriaLabel={() => label}
        className="block size-4 shrink-0 rounded-full border-2 border-[var(--ink)] bg-[var(--page)] shadow-sm outline-none transition-shadow focus-visible:ring-4 focus-visible:ring-[var(--ink)]/20 data-dragging:ring-4 data-dragging:ring-[var(--ink)]/20"
      />
    </SliderPrimitive.Control>
  </SliderPrimitive.Root>
}
