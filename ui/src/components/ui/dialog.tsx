import { useEffect, useRef, type ReactNode } from "react"

import { cn } from "@/lib/utils"

type DialogProps = {
  open: boolean
  busy?: boolean
  onClose: () => void
  labelledBy?: string
  describedBy?: string
  /** Extra classes, e.g. a wider width for forms. */
  className?: string
  children: ReactNode
}

/** Native modal `<dialog>`. Escape and backdrop clicks call `onClose` unless `busy`. */
export function Dialog({ open, busy = false, onClose, labelledBy, describedBy, className, children }: DialogProps) {
  const dialogRef = useRef<HTMLDialogElement>(null)

  useEffect(() => {
    const dialog = dialogRef.current
    if (!dialog) return
    if (open && !dialog.open) dialog.showModal()
    if (!open && dialog.open) dialog.close()
  }, [open])

  return <dialog
    ref={dialogRef}
    className={cn("whisli-dialog", className)}
    aria-labelledby={labelledBy}
    aria-describedby={describedBy}
    onCancel={event => { event.preventDefault(); if (!busy) onClose() }}
    onClick={event => { if (event.target === event.currentTarget && !busy) onClose() }}
  >{children}</dialog>
}
