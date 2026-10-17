import { Cpu, Pencil, Plus, Radio, Trash2 } from "lucide-react"

import { IconTile, Muted, PageHeader, Status } from "@/components/page"
import { Button } from "@/components/ui/button"
import { useI18n, type TranslationKey } from "@/i18n"
import { modbusBus } from "@/lib/labels"
import type { Device, Relay } from "@/types"

// Device | status | actions (edit + remove slot). The remove slot is kept empty for built-in devices so every row lines up.
// Phone: the status moves under the device name and the actions become icon-only.
const columns = "grid grid-cols-[minmax(0,1fr)_auto] items-center gap-3 border-b sm:grid-cols-[minmax(0,1fr)_100px_200px] sm:gap-4"

export function DeviceListPage({ devices, onAdd, onOpen, onRemove }: { devices: Device[] | null; onAdd: () => void; onOpen: (device: Device) => void; onRemove: (device: Relay) => void }) {
  const { t } = useI18n()
  const summary = (device: Device) => device.kind === "controller"
    ? `${t("builtInDevice")} · ${t("digitalIoShort")}`
    : `${modbusBus} · Slave ${device.address} · ${device.channelCount} CH${device.active ? "" : ` · ${t("deviceInactive")}`}`

  return <>
    <PageHeader eyebrow={t("automation")} title={t("devices")} actions={<Button variant="outline" onClick={onAdd}><Plus className="size-4" />{t("addDevice")}</Button>} />
    {devices === null ? <Muted className="whisli-line mt-5 border-t py-4">{t("connecting")}</Muted>
      : !devices.length ? <Muted className="whisli-line mt-5 border-t py-4">{t("noDevices")}</Muted>
      : <div className="whisli-line mt-5 border-t">
        <div className={`${columns} whisli-muted whisli-line hidden py-2 text-xs sm:grid`}><span>{t("catalogDeviceColumn")}</span><span>{t("state")}</span><span className="px-3">{t("catalogActionColumn")}</span></div>
        {devices.map(device => <div key={device.id} className={`${columns} whisli-line py-2`}>
          <button onClick={() => onOpen(device)} className="flex min-w-0 items-center gap-4 rounded-md py-2 text-left">
            <IconTile icon={device.kind === "controller" ? Cpu : Radio} />
            <span className="min-w-0">
              <b className="block truncate font-medium">{device.name}</b>
              <small className="whisli-muted mt-1 block truncate">{t(device.profileNameKey as TranslationKey)} · {summary(device)}</small>
              <span className="mt-1 block sm:hidden"><Status online={device.online} /></span>
            </span>
          </button>
          <span className="hidden sm:block"><Status online={device.online} /></span>
          <div className="grid grid-cols-2 gap-1">
            <Button variant="ghost" className="justify-start" aria-label={`${t("edit")} ${device.name}`} title={t("edit")} onClick={() => onOpen(device)}><Pencil className="size-4" /><span className="hidden sm:inline">{t("edit")}</span></Button>
            {device.kind === "relay" && <Button variant="dangerGhost" className="justify-start" aria-label={`${t("remove")} ${device.name}`} title={t("remove")} onClick={() => onRemove(device)}><Trash2 className="size-4" /><span className="hidden sm:inline">{t("remove")}</span></Button>}
          </div>
        </div>)}
      </div>}
  </>
}
