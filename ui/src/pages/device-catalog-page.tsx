import { Lightbulb, Radio, Search, Zap, type LucideIcon } from "lucide-react"
import { useEffect, useState } from "react"

import { ErrorText, IconTile, Muted, PageHeader } from "@/components/page"
import { Button } from "@/components/ui/button"
import { useI18n, type TranslationKey } from "@/i18n"
import { getJson, post } from "@/lib/api"
import type { Device, DeviceCatalogData } from "@/types"

const icons: Record<string, LucideIcon> = { radio: Radio, lightbulb: Lightbulb }
const columns = "grid grid-cols-[minmax(260px,2fr)_minmax(140px,1fr)_minmax(210px,1.5fr)_120px] items-center gap-4 border-b px-3"
const maxModbusAddress = 247

type CatalogEntry = { id: string; classNameKey: string; nameKey: string; descriptionKey: string; communication: string; searchKeywords: string; defaultNameKey: string; available: boolean; icon: LucideIcon }

function catalogEntries(catalog: DeviceCatalogData): CatalogEntry[] {
  return catalog.classes.flatMap(deviceClass => {
    const base = { classNameKey: deviceClass.nameKey, icon: icons[deviceClass.icon] ?? Zap }
    const profiles = catalog.profiles.filter(profile => profile.deviceClass === deviceClass.id)
    // A class without profiles is still listed as "coming soon".
    if (!profiles.length) return [{ ...base, id: deviceClass.id, nameKey: deviceClass.nameKey, descriptionKey: deviceClass.descriptionKey, communication: "—", searchKeywords: "", defaultNameKey: "", available: false }]
    return profiles.map(profile => ({ ...base, id: profile.id, nameKey: profile.nameKey, descriptionKey: profile.descriptionKey, communication: profile.communication, searchKeywords: profile.searchKeywords, defaultNameKey: profile.defaultNameKey, available: deviceClass.available && profile.available }))
  })
}

export function DeviceCatalogPage({ devices, onBack, onRefresh, onAdded }: {
  devices: Device[]
  onBack: () => void
  onRefresh: () => Promise<Device[]>
  onAdded: (device: Device | null) => void
}) {
  const { t } = useI18n()
  const [search, setSearch] = useState("")
  const [catalog, setCatalog] = useState<DeviceCatalogData | null>(null)
  const [loadError, setLoadError] = useState(false)
  const [addingProfile, setAddingProfile] = useState<string | null>(null)
  const [error, setError] = useState("")

  useEffect(() => {
    let cancelled = false
    getJson<DeviceCatalogData>("/api/v1/device-profiles")
      .then(data => { if (!cancelled) setCatalog(data) })
      .catch(() => { if (!cancelled) setLoadError(true) })
    return () => { cancelled = true }
  }, [])

  async function addDevice(entry: CatalogEntry) {
    const used = new Set(devices.flatMap(device => device.kind === "relay" ? [device.address] : []))
    let address = 1
    while (address <= maxModbusAddress && used.has(address)) address++
    if (address > maxModbusAddress) { setError(t("relayNoFreeAddresses")); return }

    setAddingProfile(entry.id)
    setError("")
    try {
      const result = await post("/api/v1/devices", { profile: entry.id, name: t((entry.defaultNameKey || "relayDefaultNameCustom") as TranslationKey), address })
      if (!result.ok) { setError(result.message || t("operationFailed")); return }
      const refreshed = await onRefresh()
      onAdded(refreshed.find(device => device.kind === "relay" && device.address === address && device.profile === entry.id) ?? null)
    } finally {
      setAddingProfile(null)
    }
  }

  const query = search.trim().toLocaleLowerCase()
  const entries = catalog ? catalogEntries(catalog).filter(entry => [t(entry.nameKey as TranslationKey), t(entry.descriptionKey as TranslationKey), entry.communication, entry.searchKeywords].join(" ").toLocaleLowerCase().includes(query)) : []

  return <>
    <PageHeader
      back={{ label: t("devices"), onClick: onBack }}
      eyebrow={t("addDevice")}
      title={t("deviceCatalogTitle")}
      description={t("deviceCatalogDescription")}
      actions={<label className="relative w-full sm:w-80"><Search className="whisli-muted pointer-events-none absolute left-3 top-1/2 size-4 -translate-y-1/2" /><input className="whisli-input whisli-input-with-icon h-10 w-full" type="search" value={search} onChange={event => setSearch(event.target.value)} placeholder={t("searchDevicesPlaceholder")} aria-label={t("searchDevices")} /></label>}
    />
    {loadError && <ErrorText>{t("deviceCatalogLoadError")}</ErrorText>}
    {error && <ErrorText>{error}</ErrorText>}
    {!catalog && !loadError && <Muted className="mt-6">{t("deviceCatalogLoading")}</Muted>}
    {catalog && <div className="whisli-line mt-6 overflow-x-auto border-y">
      <div className="min-w-[760px]">
        <div className={`${columns} whisli-muted whisli-line py-2 text-xs`}><span>{t("catalogDeviceColumn")}</span><span>{t("catalogClassColumn")}</span><span>{t("catalogCommunicationColumn")}</span><span className="text-right">{t("catalogActionColumn")}</span></div>
        {entries.map(entry => <div key={entry.id} className={`${columns} whisli-line py-3 last:border-b-0`}>
          <div className="flex min-w-0 items-center gap-3"><IconTile icon={entry.icon} /><span className="min-w-0"><b className="block truncate font-medium">{t(entry.nameKey as TranslationKey)}</b><small className="whisli-muted mt-0.5 block truncate">{t(entry.descriptionKey as TranslationKey)}</small></span></div>
          <span>{t(entry.classNameKey as TranslationKey)}</span>
          <span className="whisli-muted">{entry.communication}</span>
          <div className="flex justify-end"><Button variant="outline" disabled={!entry.available || addingProfile !== null} onClick={() => void addDevice(entry)}>{addingProfile === entry.id ? t("adding") : entry.available ? t("add") : t("comingSoon")}</Button></div>
        </div>)}
        {!entries.length && <Muted className="px-3 py-8 text-center">{t("noCatalogMatches")}</Muted>}
      </div>
    </div>}
  </>
}
