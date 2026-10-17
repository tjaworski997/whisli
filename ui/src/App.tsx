import { Boxes, CircleX, House, Lightbulb, LogOut, MapPin, Moon, NotebookPen, Settings, Sun, Workflow, Zap, type LucideIcon } from "lucide-react"
import { Fragment, lazy, Suspense, useEffect, useState, type ReactNode } from "react"

import { Muted } from "@/components/page"
import { Button, buttonVariants } from "@/components/ui/button"
import { MessageBox } from "@/components/ui/message-box"
import { useLiveRefresh } from "@/hooks/use-live-refresh"
import { useTheme } from "@/hooks/use-theme"
import { useI18n, type Locale, type TranslationKey } from "@/i18n"
import { getJson, post } from "@/lib/api"
import { checkAuth, signedOut, useAuth } from "@/lib/auth"
import { routeHash, useRoute, type AppRoute, type Page } from "@/lib/route"
import { cn } from "@/lib/utils"
import { ActionsPage } from "@/pages/actions-page"
import { ControllerPage } from "@/pages/controller-page"
import { DeviceCatalogPage } from "@/pages/device-catalog-page"
import { DeviceListPage } from "@/pages/device-list-page"
import { HomePage } from "@/pages/home-page"
import { LoginPage } from "@/pages/login-page"
import { LogsPage } from "@/pages/logs-page"
import { NotesPage } from "@/pages/notes-page"
import { MqttPage, NetworkPage } from "@/pages/network-page"
import { LightPresetsPage } from "@/pages/light-presets-page"
import { LightSettingsPage } from "@/pages/light-settings-page"
import { LightsPage } from "@/pages/lights-page"
import { RelayPage } from "@/pages/relay-page"
import { SystemPage } from "@/pages/system-page"
import { TestPage } from "@/pages/test-page"
import { ZonesPage } from "@/pages/zones-page"
import type { Device, Relay } from "@/types"


// The map pulls in React Flow (~50 kB gzip); load it only when opened.
const MapPage = lazy(() => import("@/pages/map-page").then(module => ({ default: module.MapPage })))
// A section may have sub-pages; they show under it while one of the section's pages is open.
type NavigationItem = { page: Page; label: TranslationKey; icon: LucideIcon; children?: { page: Page; label: TranslationKey }[] }
const navigation: NavigationItem[] = [
  { page: "home", label: "navHome", icon: House },
  { page: "devices", label: "devices", icon: Boxes },
  { page: "lights", label: "lights", icon: Lightbulb, children: [{ page: "lights", label: "lightDefinitions" }, { page: "lightPresets", label: "navLightPresets" }, { page: "lightSettings", label: "navLightSettings" }] },
  { page: "actions", label: "buttons", icon: Zap },
  { page: "zones", label: "zonesTitle", icon: MapPin },
  { page: "map", label: "navMap", icon: Workflow },
  { page: "system", label: "navSystem", icon: Settings, children: [{ page: "system", label: "navSystemSettings" }, { page: "network", label: "networkTitle" }, { page: "mqtt", label: "mqttTitle" }, { page: "test", label: "testTitle" }, { page: "logs", label: "systemLogs" }] },
]

/** Pages that show the device list and therefore keep it polled. */
const devicePages: Page[] = ["home", "devices", "deviceTypes", "device", "lights", "test"]

export default function App() {
  const auth = useAuth()
  useEffect(() => { void checkAuth() }, [])
  if (auth === "unknown") return <div className="whisli-page min-h-screen" />
  return auth === "signedOut" ? <LoginPage /> : <Panel />
}

async function logout() {
  await post("/api/v1/auth/logout")
  signedOut()
}

function Panel() {
  const { t, locale, setLocale, labels } = useI18n()
  const { theme, toggleTheme } = useTheme()
  const { route, navigate, goBack } = useRoute()
  /** `null` until the first successful load. On errors the last known list is kept and `connected` drops. */
  const [devices, setDevices] = useState<Device[] | null>(null)
  const [connected, setConnected] = useState(true)
  const [pendingRemoval, setPendingRemoval] = useState<Relay | null>(null)

  async function refreshDevices() {
    try {
      const { devices } = await getJson<{ devices: Device[] }>("/api/v1/devices")
      setDevices(devices)
      setConnected(true)
      return devices
    } catch {
      setConnected(false)
      return []
    }
  }
  const pollsDevices = devicePages.includes(route.page)
  useLiveRefresh(refreshDevices, ["devices"], pollsDevices)

  const deviceList = devices ?? []
  const device = deviceList.find(item => item.id === route.deviceId)
  const backToDevices = () => goBack({ page: "devices" })
  const openDevice = (item: Device) => navigate({ page: "device", deviceId: item.id })

  function renderPage() {
    switch (route.page) {
      case "home": return <HomePage devices={deviceList} onOpen={navigate} />
      case "deviceTypes": return <DeviceCatalogPage devices={deviceList} onBack={backToDevices} onRefresh={refreshDevices} onAdded={added => navigate(added ? { page: "device", deviceId: added.id } : { page: "devices" }, true)} />
      case "lightSettings": return <LightSettingsPage />
      case "lightPresets": return <LightPresetsPage />
      case "lights": return <LightsPage devices={deviceList} lightId={route.lightId} onClearFilter={() => navigate({ page: "lights" })} />
      case "actions": return <ActionsPage filter={route.filter} highlight={route.highlight} onClearFilter={() => navigate({ page: "actions" })} onConfigureInputs={() => navigate({ page: "device", deviceId: "controller" })} />
      case "map": return <Suspense fallback={<Muted>{t("connecting")}</Muted>}><MapPage theme={theme} onOpen={navigate} /></Suspense>
      case "zones": return <ZonesPage />
      case "network": return <NetworkPage />
      case "mqtt": return <MqttPage />
      case "logs": return <LogsPage />
      case "test": return <TestPage devices={deviceList} onRefreshDevices={refreshDevices} />
      case "system": return <SystemPage />
      case "notes": return <NotesPage />
      case "device":
        if (devices === null) return <Muted>{t("connecting")}</Muted>
        if (device?.kind === "relay") return <RelayPage key={device.id} relay={device} onBack={backToDevices} onRefresh={refreshDevices} onRemove={() => setPendingRemoval(device)} />
        if (device?.kind === "controller") return <ControllerPage key={device.id} device={device} onBack={backToDevices} onRefresh={refreshDevices} />
        break // Unknown or removed device: show the list.
      case "devices": break
    }
    return <DeviceListPage devices={devices} onAdd={() => navigate({ page: "deviceTypes" })} onOpen={openDevice} onRemove={setPendingRemoval} />
  }

  return <div className="whisli-page min-h-screen text-sm">
    <header className="whisli-line flex h-14 items-center gap-2 border-b px-4 md:px-6">
      <b className="text-base">{t("appName")}</b>
      <span className="whisli-muted ml-2 hidden sm:inline">{t("appSubtitle")}</span>
      {pollsDevices && !connected && <span role="status" className="whisli-status-error ml-2 inline-flex min-w-0 items-center gap-1.5 text-xs"><CircleX className="size-4 shrink-0" /><span className="truncate">{t("controllerUnreachable")}</span></span>}
      <NavLink to={{ page: "notes" }} onNavigate={navigate} current={route.page === "notes"} title={t("notesTitle")}
        className={cn(buttonVariants({ variant: "ghost", size: "icon" }), "ml-auto", route.page === "notes" && "whisli-nav-active")}>
        <NotebookPen className="size-4" />
      </NavLink>
      <select className="whisli-select h-8 text-xs" aria-label={t("language")} value={locale} onChange={event => setLocale(event.target.value as Locale)}>
        {Object.entries(labels).map(([value, label]) => <option key={value} value={value}>{label}</option>)}
      </select>
      <Button variant="ghost" size="icon" aria-label={t(theme === "light" ? "themeDark" : "themeLight")} title={t("theme")} onClick={toggleTheme}>
        {theme === "light" ? <Moon className="size-4" /> : <Sun className="size-4" />}
      </Button>
      <Button variant="ghost" size="icon" aria-label={t("logout")} title={t("logout")} onClick={() => void logout()}>
        <LogOut className="size-4" />
      </Button>
    </header>

    <div className="md:grid md:min-h-[calc(100vh-56px)] md:grid-cols-[220px_1fr]">
      {/* Phone: horizontal, scrollable tab bar. Desktop: sidebar with the firmware version at the bottom. */}
      <aside className="whisli-line border-b md:flex md:flex-col md:border-r md:border-b-0 md:px-4 md:py-5">
        <nav className="flex gap-1 overflow-x-auto px-2 py-2 [scrollbar-width:none] md:block md:space-y-1 md:overflow-visible md:p-0">
          {navigation.map(({ page, label, icon: Icon, children }) => {
            const inSection = route.page === page || Boolean(children?.some(child => child.page === route.page))
            return <Fragment key={page}>
              <NavLink to={{ page }} onNavigate={navigate} current={!children && inSection} className={cn("flex shrink-0 items-center gap-2 rounded-md px-3 py-2 text-left md:w-full md:gap-3", inSection && !children ? "whisli-nav-active" : inSection ? "font-medium text-[var(--ink)]" : "whisli-nav")}>
                <Icon className="size-4" />{t(label)}
              </NavLink>
              {inSection && children?.map(child => <NavLink key={child.page} to={{ page: child.page }} onNavigate={navigate} current={route.page === child.page}
                className={cn("flex shrink-0 items-center rounded-md px-3 py-1.5 text-left text-sm md:ml-7 md:w-[calc(100%-1.75rem)]", route.page === child.page ? "whisli-nav-active" : "whisli-nav")}>
                {t(child.label)}
              </NavLink>)}
            </Fragment>
          })}
        </nav>
        <footer className="whisli-line whisli-muted mt-auto hidden border-t pt-4 text-xs md:block">
          <p>{t("firmwareLabel")}</p>
          <p className="mt-1 font-mono">{__WHISLI_VERSION__}</p>
        </footer>
      </aside>
      <main className="min-w-0 px-4 py-6 md:px-8 md:py-7 lg:px-10">{renderPage()}</main>
    </div>

    {pendingRemoval && <RemoveDeviceDialog
      device={pendingRemoval}
      onClose={() => setPendingRemoval(null)}
      onRemoved={async () => {
        setPendingRemoval(null)
        if (route.deviceId === pendingRemoval.id) navigate({ page: "devices" }, true)
        await refreshDevices()
      }}
    />}
  </div>
}

/**
 * Menu entry as a real link: right click, middle click and Ctrl/Cmd+click open it in a new tab or
 * window; a plain click stays in the app without reloading.
 */
function NavLink({ to, current, className, title, onNavigate, children }: { to: AppRoute; current: boolean; className: string; title?: string; onNavigate: (route: AppRoute) => void; children: ReactNode }) {
  return <a href={routeHash(to)} aria-current={current ? "page" : undefined} className={className} title={title} aria-label={title}
    onClick={event => {
      if (event.button !== 0 || event.metaKey || event.ctrlKey || event.shiftKey || event.altKey) return
      event.preventDefault()
      onNavigate(to)
    }}>{children}</a>
}

function RemoveDeviceDialog({ device, onClose, onRemoved }: { device: Relay; onClose: () => void; onRemoved: () => void }) {
  const { t } = useI18n()
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState("")

  async function remove() {
    setBusy(true)
    const result = await post("/api/v1/devices/remove", { id: device.id })
    setBusy(false)
    if (result.ok) onRemoved()
    else setError(result.message || t("operationFailed"))
  }

  return <MessageBox
    open
    type="danger"
    title={t("removeDeviceTitle")}
    message={t("removeDeviceConfirm")}
    detail={device.name}
    error={error}
    confirmLabel={busy ? t("removing") : t("remove")}
    cancelLabel={t("cancel")}
    busy={busy}
    onConfirm={() => void remove()}
    onClose={onClose}
  />
}
