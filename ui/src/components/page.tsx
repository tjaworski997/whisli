import { CircleCheck, CircleX, type LucideIcon } from "lucide-react"
import type { InputHTMLAttributes, ReactNode } from "react"

import { useI18n } from "@/i18n"
import { cn } from "@/lib/utils"

export function PageHeader({ eyebrow, title, description, icon, actions, back }: {
  eyebrow?: string
  title: string
  description?: string
  icon?: LucideIcon
  actions?: ReactNode
  back?: { label: string; onClick: () => void }
}) {
  return <header>
    {back && <button className="whisli-link mb-3" onClick={back.onClick}>← {back.label}</button>}
    <div className="flex flex-wrap items-start gap-x-4 gap-y-3">
      {icon && <IconTile icon={icon} className="self-center" />}
      <div className="min-w-0 flex-1">
        {eyebrow && <p className="whisli-muted mb-1">{eyebrow}</p>}
        <h1 className="text-xl font-semibold">{title}</h1>
        {description && <p className="whisli-muted mt-1">{description}</p>}
      </div>
      {actions && <div className="flex w-full flex-wrap items-center gap-2 sm:w-auto">{actions}</div>}
    </div>
  </header>
}

export function Section({ title, description, actions, bordered = false, className, children }: {
  title: string
  description?: string
  actions?: ReactNode
  bordered?: boolean
  className?: string
  children: ReactNode
}) {
  return <section className={cn("mt-8", bordered && "whisli-line border-y py-4", className)}>
    <div className="flex flex-wrap items-end justify-between gap-3">
      <div className="min-w-0"><h2 className="font-medium">{title}</h2>{description && <p className="whisli-muted mt-1">{description}</p>}</div>
      {actions && <div className="flex flex-wrap gap-2">{actions}</div>}
    </div>
    {children}
  </section>
}

export function Field({ label, className, children }: { label: string; className?: string; children: ReactNode }) {
  return <label className={cn("grid gap-1.5", className)}><span className="whisli-muted text-xs">{label}</span>{children}</label>
}

/**
 * Uncontrolled input that calls `onSave` on blur when the trimmed value differs from `value`.
 * Re-mounts when `value` changes on the server, so polling refreshes stay visible.
 */
export function AutoSaveInput({ value, onSave, inline = false, className, ...props }: Omit<InputHTMLAttributes<HTMLInputElement>, "value" | "defaultValue" | "onBlur"> & {
  value: string | number
  onSave: (value: string) => void
  inline?: boolean
}) {
  return <input
    key={String(value)}
    className={cn("whisli-input", inline && "whisli-input-inline", className)}
    defaultValue={value}
    onBlur={event => {
      const next = event.currentTarget.value.trim()
      if (next !== String(value)) onSave(next)
    }}
    {...props}
  />
}

export function IconTile({ icon: Icon, className }: { icon: LucideIcon; className?: string }) {
  return <span className={cn("whisli-raised flex size-9 shrink-0 items-center justify-center rounded-md", className)}><Icon className="size-4" /></span>
}

export function Status({ online }: { online: boolean }) {
  const { t } = useI18n()
  const Icon = online ? CircleCheck : CircleX
  return <span className={cn("inline-flex shrink-0 items-center gap-1.5 text-xs", online ? "whisli-status-ok" : "whisli-status-error")}><Icon className="size-4" />{t(online ? "statusOnline" : "statusOffline")}</span>
}

export function ErrorText({ children }: { children: ReactNode }) {
  return <p role="alert" className="whisli-status-error mt-4 text-sm">{children}</p>
}

export function Muted({ className, children }: { className?: string; children: ReactNode }) {
  return <p className={cn("whisli-muted", className)}>{children}</p>
}

export function SaveStatus({ saving, message, children }: { saving: boolean; message: string; children?: ReactNode }) {
  const { t } = useI18n()
  const status = saving ? t("saving") : message
  return <p className="whisli-muted mt-3 text-xs">{children}{children && status ? " · " : ""}{status}</p>
}
