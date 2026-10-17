import * as React from "react"
import { Checkbox as CheckboxPrimitive } from "radix-ui"
import { Check } from "lucide-react"

import { cn } from "@/lib/utils"

function Checkbox({ className, ...props }: React.ComponentProps<typeof CheckboxPrimitive.Root>) {
  return <CheckboxPrimitive.Root
    data-slot="checkbox"
    className={cn(
      "peer inline-flex size-4 shrink-0 items-center justify-center rounded border border-[var(--line)] bg-transparent text-[var(--ink)] shadow-sm transition-colors outline-none focus-visible:ring-2 focus-visible:ring-[var(--ink)]/30 disabled:cursor-not-allowed disabled:opacity-50 data-[state=checked]:border-[var(--ink)] data-[state=checked]:bg-[var(--ink)] data-[state=checked]:text-[var(--page)]",
      className,
    )}
    {...props}
  >
    <CheckboxPrimitive.Indicator className="grid place-content-center">
      <Check className="size-3" />
    </CheckboxPrimitive.Indicator>
  </CheckboxPrimitive.Root>
}

export { Checkbox }
