import { cva, type VariantProps } from "class-variance-authority"
import type * as React from "react"

import { cn } from "@/lib/utils"

const buttonVariants = cva(
  // Every variant has a 1px border (transparent when not drawn), so switching variants never changes the size.
  "inline-flex items-center justify-center gap-2 whitespace-nowrap rounded-md border text-sm font-medium transition-colors disabled:pointer-events-none disabled:opacity-50",
  {
    variants: {
      variant: {
        default: "whisli-button-primary",
        outline: "whisli-button-outline",
        ghost: "whisli-button-ghost",
        danger: "whisli-button-danger",
        dangerGhost: "whisli-button-danger-ghost",
      },
      size: {
        default: "px-4 py-2",
        sm: "h-8 px-2 text-xs",
        icon: "size-8 p-0",
      },
    },
    defaultVariants: { variant: "default", size: "default" },
  },
)

export interface ButtonProps extends React.ButtonHTMLAttributes<HTMLButtonElement>, VariantProps<typeof buttonVariants> {}

function Button({ className, variant, size, type = "button", ...props }: ButtonProps) {
  return <button className={cn(buttonVariants({ variant, size }), className)} type={type} {...props} />
}

export { Button, buttonVariants }
