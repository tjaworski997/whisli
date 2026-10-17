import { CircleHelp, Info, Trash2 } from "lucide-react"
import { useId, type ReactNode } from "react"

import { Button } from "@/components/ui/button"
import { Dialog } from "@/components/ui/dialog"
import { ErrorText } from "@/components/page"
import { cn } from "@/lib/utils"

export type MessageBoxType = "question" | "information" | "danger"

type MessageBoxProps = {
  open: boolean
  type: MessageBoxType
  title: string
  message: string
  /** Extra content under the message, e.g. the name of the affected item. */
  detail?: ReactNode
  error?: string
  confirmLabel?: string
  cancelLabel: string
  busy?: boolean
  onConfirm?: () => void
  onClose: () => void
}

const icons = { question: CircleHelp, information: Info, danger: Trash2 }

/** Confirmation / information dialog. `onConfirm` does not close the box — the caller decides (e.g. after a request succeeds). */
export function MessageBox({ open, type, title, message, detail, error, confirmLabel, cancelLabel, busy = false, onConfirm, onClose }: MessageBoxProps) {
  const id = useId()
  const Icon = icons[type]
  const asks = type !== "information"

  return <Dialog open={open} busy={busy} onClose={onClose} labelledBy={`${id}-title`} describedBy={`${id}-message`}>
    <div className="flex items-start gap-3">
      <span className={cn("flex size-9 shrink-0 items-center justify-center rounded-full", type === "danger" ? "whisli-status-error bg-[var(--status-error-bg)]" : "whisli-raised")}><Icon className="size-4" /></span>
      <div className="min-w-0">
        <h2 id={`${id}-title`} className="font-semibold">{title}</h2>
        <p id={`${id}-message`} className="whisli-muted mt-2 text-sm">{message}</p>
        {detail && <div className="mt-2 break-words text-sm font-medium">{detail}</div>}
      </div>
    </div>
    {error && <ErrorText>{error}</ErrorText>}
    <div className="mt-6 flex justify-end gap-2">
      <Button variant="outline" disabled={busy} onClick={onClose}>{cancelLabel}</Button>
      {asks && <Button variant={type === "danger" ? "danger" : "outline"} disabled={busy} onClick={onConfirm}>{confirmLabel}</Button>}
    </div>
  </Dialog>
}
