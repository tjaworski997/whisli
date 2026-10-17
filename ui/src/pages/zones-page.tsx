import { ArrowDown, ArrowUp, MapPin, Plus, Trash2 } from "lucide-react"
import { useState } from "react"

import { AutoSaveInput, ErrorText, Muted, PageHeader } from "@/components/page"
import { Button } from "@/components/ui/button"
import { MessageBox } from "@/components/ui/message-box"
import { useZones } from "@/hooks/use-zones"
import { useI18n } from "@/i18n"
import { post } from "@/lib/api"
import type { Zone } from "@/types"

/** The list of zones: add, rename, order (which lists and the map follow) and remove. */
export function ZonesPage() {
  const { t } = useI18n()
  const zones = useZones()
  const [addedId, setAddedId] = useState<number | null>(null)
  const [error, setError] = useState("")
  const [removing, setRemoving] = useState<Zone | null>(null)
  const [busy, setBusy] = useState(false)

  // What each zone holds, counted from the assignments ("light-3", "controller.input-2").
  const members = (zone: number) => {
    const targets = Object.entries(zones.assignments).filter(([, id]) => id === zone).map(([target]) => target)
    return { lights: targets.filter(target => target.startsWith("light-")).length, buttons: targets.filter(target => target.includes(".input-")).length }
  }

  async function run(request: () => Promise<{ ok: boolean; message: string }>) {
    setBusy(true)
    const result = await request()
    setBusy(false)
    setError(result.ok ? "" : result.message || t("zoneSaveError"))
    await zones.refresh()
    return result.ok
  }

  // Like a new light: a record with a default name, its name selected to type over.
  async function add() {
    const base = t("zoneDefaultName")
    const taken = new Set(zones.zones.map(zone => zone.name.toLocaleLowerCase()))
    let name = base
    for (let number = 2; taken.has(name.toLocaleLowerCase()); number += 1) name = `${base} ${number}`
    let id = 0
    await run(async () => { const result = await post("/api/v1/zones", { name }); id = Number(result.data.id) || 0; return result })
    setAddedId(id || null)
  }

  const move = (index: number, by: number) => {
    const ids = zones.zones.map(zone => zone.id)
    ;[ids[index], ids[index + by]] = [ids[index + by], ids[index]]
    void run(() => post("/api/v1/zones/order", { ids: ids.join(",") }))
  }

  return <>
    <PageHeader eyebrow={t("automation")} title={t("zonesTitle")} description={t("zonesDescription")} actions={
      <Button variant="outline" disabled={busy} onClick={() => void add()}><Plus className="size-4" />{t("zoneAdd")}</Button>
    } />
    {error && <ErrorText>{error}</ErrorText>}
    {zones.zones.length === 0
      ? <Muted className="whisli-line mt-5 border-t py-4">{t("zonesEmpty")}</Muted>
      : <div className="whisli-line mt-5 border-t">
        {zones.zones.map((zone, index) => {
          const count = members(zone.id)
          return <div key={zone.id} className="whisli-line grid grid-cols-[auto_minmax(0,1fr)_auto] items-center gap-3 border-b py-2.5 sm:grid-cols-[auto_minmax(0,1fr)_auto_auto]">
            <div className="flex">
              <Button variant="ghost" size="icon" disabled={busy || index === 0} aria-label={t("zoneMoveUp")} title={t("zoneMoveUp")} onClick={() => move(index, -1)}><ArrowUp className="size-4" /></Button>
              <Button variant="ghost" size="icon" disabled={busy || index === zones.zones.length - 1} aria-label={t("zoneMoveDown")} title={t("zoneMoveDown")} onClick={() => move(index, 1)}><ArrowDown className="size-4" /></Button>
            </div>
            <div className="flex min-w-0 items-center gap-2">
              <MapPin className="whisli-muted size-4 shrink-0" />
              <AutoSaveInput inline className="font-medium" value={zone.name} maxLength={32} aria-label={t("zoneName")}
                autoFocus={zone.id === addedId} onFocus={event => { if (zone.id === addedId) { event.currentTarget.select(); setAddedId(null) } }}
                onSave={next => { if (next) void run(() => post("/api/v1/zones/rename", { id: zone.id, name: next })) }} />
            </div>
            <span className="whisli-muted col-span-3 pl-[4.5rem] text-xs sm:col-span-1 sm:pl-0">{t("zoneMembers", count)}</span>
            <Button variant="dangerGhost" size="icon" className="row-start-1 col-start-3 sm:col-start-4" disabled={busy} aria-label={t("zoneRemove")} title={t("zoneRemove")} onClick={() => setRemoving(zone)}><Trash2 className="size-4" /></Button>
          </div>
        })}
      </div>}
    <MessageBox
      open={removing !== null}
      type="danger"
      title={t("zoneRemoveTitle")}
      message={t("zoneRemoveMessage")}
      detail={removing?.name}
      confirmLabel={t("remove")}
      cancelLabel={t("cancel")}
      busy={busy}
      onConfirm={() => { if (removing) void run(() => post("/api/v1/zones/remove", { id: removing.id })).then(() => setRemoving(null)) }}
      onClose={() => setRemoving(null)}
    />
  </>
}
