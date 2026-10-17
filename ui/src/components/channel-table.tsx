import { Power, PowerOff } from "lucide-react"
import type { ReactNode } from "react"

import { AutoSaveInput } from "@/components/page"
import { Button } from "@/components/ui/button"
import { Checkbox } from "@/components/ui/checkbox"
import { useI18n } from "@/i18n"
import { channelLabel } from "@/lib/format"

export type ChannelRow = { name: string; used: boolean; hint?: string; state: ReactNode }

const columns = "grid grid-cols-[58px_minmax(0,1fr)_76px_72px] items-center gap-3 border-b sm:grid-cols-[72px_minmax(0,1fr)_92px_100px] sm:gap-4"

/** Physical channels (relay CH, controller DI/DO): fixed label, editable name, "used" flag and a state cell. */
export function ChannelTable({ prefix, rows, stateLabel, disabled, onRename, onUsedChange }: {
  prefix: string
  rows: ChannelRow[]
  stateLabel: string
  disabled: boolean
  onRename: (index: number, name: string) => void
  onUsedChange: (index: number, used: boolean) => void
}) {
  const { t } = useI18n()
  return <div className="whisli-line mt-3 border-t">
    <div className={`${columns} whisli-muted whisli-line py-2 text-[11px]`}><span>{prefix}</span><span>{t("channelName")}</span><span className="text-center">{t("relayUsed")}</span><span className="text-right">{stateLabel}</span></div>
    {rows.map((row, index) => {
      const label = channelLabel(prefix, index)
      return <div key={index} className={`${columns} whisli-line py-2.5`}>
        <span className="font-medium tabular-nums">{label}</span>
        <div className="min-w-0">
          <AutoSaveInput inline value={row.name} placeholder={t("channelNamePlaceholder")} aria-label={`${t("channelName")} ${label}`} onSave={name => onRename(index, name)} />
          {row.hint && <span className="whisli-muted block truncate px-2 text-[10px]">{row.hint}</span>}
        </div>
        <Checkbox className="justify-self-center" checked={row.used} disabled={disabled} aria-label={`${t("relayUsed")} ${label}`} onCheckedChange={checked => onUsedChange(index, checked === true)} />
        <div className="justify-self-end">{row.state}</div>
      </div>
    })}
  </div>
}

export function OutputToggle({ on, busy, disabled, label, onClick }: { on: boolean; busy: boolean; disabled: boolean; label: string; onClick: () => void }) {
  return <button onClick={onClick} disabled={busy || disabled} aria-label={label} className="whisli-raised whisli-muted inline-flex min-w-16 items-center justify-center gap-1.5 rounded-md px-2.5 py-1 text-xs disabled:opacity-50">
    {busy ? "…" : on ? <><Power className="size-3.5" />ON</> : <><PowerOff className="size-3.5" />OFF</>}
  </button>
}

export function AllOutputsButtons({ busy, disabled, onSetAll }: { busy: boolean; disabled: boolean; onSetAll: (on: boolean) => void }) {
  const { t } = useI18n()
  return <>
    <Button variant="outline" disabled={busy || disabled} onClick={() => onSetAll(true)}><Power className="size-4" />{t("turnAllOn")}</Button>
    <Button variant="outline" disabled={busy || disabled} onClick={() => onSetAll(false)}><PowerOff className="size-4" />{t("turnAllOff")}</Button>
  </>
}
