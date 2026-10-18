import { Cable, Lock, PlugZap, RadioTower, RefreshCw, Wifi, WifiOff } from "lucide-react"
import { useEffect, useState, type FormEvent } from "react"

import { ErrorText, Field, Muted, PageHeader, Section } from "@/components/page"
import { Button } from "@/components/ui/button"
import { Checkbox } from "@/components/ui/checkbox"
import { MessageBox } from "@/components/ui/message-box"
import { usePolling } from "@/hooks/use-polling"
import { useI18n, type TranslationKey } from "@/i18n"
import { getJson, post, type ApiParams } from "@/lib/api"
import { cn } from "@/lib/utils"
import type { Device, NetworkInfo, WifiNetwork } from "@/types"

const sleep = (ms: number) => new Promise(resolve => window.setTimeout(resolve, ms))
/** Rough signal quality from RSSI: -100 dBm and below is 0 %, -50 dBm and above 100 %. */
const signalPercent = (rssi: number) => Math.max(0, Math.min(100, 2 * (rssi + 100)))

/** Network state and settings, refreshed every few seconds while a page shows them. */
function useNetwork() {
  const [info, setInfo] = useState<NetworkInfo | null>(null)
  const [error, setError] = useState(false)
  async function refresh() {
    try { setInfo(await getJson<NetworkInfo>("/api/v1/network")); setError(false) } catch { setError(true) }
  }
  usePolling(refresh, 5000)
  return { info, error, refresh }
}

/** System › Network: Ethernet and Wi-Fi state, joining a Wi-Fi network and the setup access point. */
export function NetworkPage() {
  const { t } = useI18n()
  const { info, error, refresh } = useNetwork()
  return <>
    <PageHeader eyebrow={t("navSystem")} title={t("networkTitle")} description={t("networkDescription")} />
    {error && <ErrorText>{t("networkLoadError")}</ErrorText>}
    {!info ? <Muted className="mt-6">{t("connecting")}</Muted> : <div className="max-w-4xl">
      <ConnectionsSection info={info} />
      <WifiSection info={info} onChanged={refresh} />
      <AccessPointSection info={info} onChanged={refresh} />
    </div>}
  </>
}

/** System › MQTT: broker connection settings. */
export function MqttPage() {
  const { t } = useI18n()
  const { info, error, refresh } = useNetwork()
  return <>
    <PageHeader eyebrow={t("navSystem")} title={t("mqttTitle")} description={t("networkMqttDescription")} />
    {error && <ErrorText>{t("networkLoadError")}</ErrorText>}
    {!info ? <Muted className="mt-6">{t("connecting")}</Muted> : <div className="max-w-4xl">
      <MqttStatus info={info} />
      <MqttSection info={info} onChanged={refresh} />
      <MqttTopics info={info} />
    </div>}
  </>
}

function ConnectionsSection({ info }: { info: NetworkInfo }) {
  const { t } = useI18n()
  const { ethernet, wifi, accessPoint } = info
  return <Section bordered className="mt-6 py-6" title={t("networkConnections")}>
    <div className="mt-4 grid gap-3 sm:grid-cols-3">
      <StatusCard icon={Cable} title="Ethernet" on={ethernet.connected} state={t(ethernet.connected ? "networkConnected" : ethernet.link ? "networkNoAddress" : "networkNoCable")}
        rows={[[t("networkIp"), ethernet.ip], [t("networkGateway"), ethernet.gateway], ["MAC", ethernet.mac]]} />
      <StatusCard icon={wifi.connected ? Wifi : WifiOff} title="Wi-Fi" on={wifi.connected} state={wifi.connected ? wifi.ssid : t(wifi.savedSsid ? "networkWifiConnecting" : "networkWifiNone")}
        rows={[[t("networkIp"), wifi.ip], [t("networkSignal"), wifi.connected ? `${signalPercent(wifi.rssi)}% (${wifi.rssi} dBm)` : ""], ["MAC", wifi.mac]]} />
      <StatusCard icon={RadioTower} title={accessPoint.ssid} on={accessPoint.active}
        state={t(accessPoint.fallback ? "networkApFallback" : accessPoint.active ? "networkApOn" : "networkApOff")}
        rows={[[t("networkIp"), accessPoint.ip]]} />
    </div>
  </Section>
}

function StatusCard({ icon: Icon, title, on, state, rows }: { icon: typeof Wifi; title: string; on: boolean; state: string; rows: [string, string][] }) {
  return <div className="whisli-raised rounded-md p-3 text-sm">
    <p className="flex items-center gap-2 font-medium"><Icon className={cn("size-4", on ? "whisli-status-ok" : "whisli-muted")} />{title}</p>
    <p className={cn("mt-1 truncate text-xs", on ? "whisli-status-ok" : "whisli-muted")}>{state}</p>
    <dl className="mt-3 grid grid-cols-[auto_minmax(0,1fr)] gap-x-3 gap-y-1 text-xs">
      {rows.filter(([, value]) => value).map(([label, value]) => <InfoRow key={label} label={label} value={value} />)}
    </dl>
  </div>
}

function InfoRow({ label, value }: { label: string; value: string }) {
  return <><dt className="whisli-muted">{label}</dt><dd className="truncate font-mono" title={value}>{value}</dd></>
}

type WifiState = { kind: "idle" | "connecting" | "ok" | "error"; message?: string; key?: TranslationKey }

function WifiSection({ info, onChanged }: { info: NetworkInfo; onChanged: () => void }) {
  const { t } = useI18n()
  const [ssid, setSsid] = useState("")
  const [password, setPassword] = useState("")
  const [networks, setNetworks] = useState<WifiNetwork[] | null>(null)
  const [scanning, setScanning] = useState(false)
  const [state, setState] = useState<WifiState>({ kind: "idle" })
  const [forgetting, setForgetting] = useState(false)
  const chosen = networks?.find(network => network.ssid === ssid)

  async function scan() {
    setScanning(true)
    await post("/api/v1/network/wifi/scan/start")
    for (let attempt = 0; attempt < 30; attempt += 1) {
      await sleep(700)
      try {
        const result = await getJson<{ state: string; networks: WifiNetwork[] }>("/api/v1/network/wifi/scan")
        if (result.state === "running") continue
        // One entry per name, the strongest; strongest first.
        const best = new Map<string, WifiNetwork>()
        for (const network of result.networks) if (network.ssid && (best.get(network.ssid)?.rssi ?? -999) < network.rssi) best.set(network.ssid, network)
        setNetworks([...best.values()].sort((left, right) => right.rssi - left.rssi))
        break
      } catch { break }
    }
    setScanning(false)
  }

  async function connect(event: FormEvent) {
    event.preventDefault()
    setState({ kind: "connecting" })
    const started = await post("/api/v1/network/wifi/connect", { ssid: ssid.trim(), password })
    if (!started.ok) { setState({ kind: "error", message: started.message }); return }
    for (let attempt = 0; attempt < 25; attempt += 1) {
      await sleep(1000)
      try {
        const status = await getJson<{ state: string; message: string }>("/api/v1/network/wifi/connect/status")
        if (status.state === "connecting") continue
        setState(status.state === "connected" ? { kind: "ok", message: status.message } : { kind: "error", message: status.message })
        if (status.state === "connected") setPassword("")
        onChanged()
        return
      } catch { /* the controller may switch networks: keep asking */ }
    }
    setState({ kind: "error", key: "networkWifiTimeout" })
  }

  async function forget() {
    await post("/api/v1/network/wifi/forget")
    setForgetting(false)
    setState({ kind: "idle" })
    onChanged()
  }

  return <Section bordered className="mt-0 border-t-0 py-6" title="Wi-Fi" description={t("networkWifiDescription")}
    actions={info.wifi.savedSsid ? <Button variant="dangerGhost" onClick={() => setForgetting(true)}>{t("networkWifiForget")}</Button> : undefined}>
    <form onSubmit={event => void connect(event)} className="mt-4 grid gap-3 sm:grid-cols-[minmax(0,1fr)_minmax(0,1fr)_auto] sm:items-end">
      <Field label={t("networkSsid")}>
        <input className="whisli-input h-9" value={ssid} required maxLength={32} autoComplete="off" placeholder={info.wifi.savedSsid || t("networkSsidPlaceholder")} onChange={event => setSsid(event.currentTarget.value)} />
      </Field>
      <Field label={t("password")}>
        <input className="whisli-input h-9" type="password" value={password} maxLength={63} autoComplete="new-password" disabled={chosen?.open}
          placeholder={chosen?.open ? t("networkOpenNetwork") : ""} onChange={event => setPassword(event.currentTarget.value)} />
      </Field>
      <Button type="submit" disabled={state.kind === "connecting" || !ssid.trim()}>{t(state.kind === "connecting" ? "networkWifiConnectingNow" : "networkWifiConnect")}</Button>
    </form>
    {state.kind !== "idle" && state.kind !== "connecting" && <p role="status" className={cn("mt-3 text-sm", state.kind === "ok" ? "whisli-status-ok" : "whisli-status-error")}>{state.key ? t(state.key) : state.message}</p>}

    <div className="mt-5 flex items-center gap-3">
      <Button variant="outline" disabled={scanning} onClick={() => void scan()}><RefreshCw className={cn("size-4", scanning && "animate-spin")} />{t(scanning ? "networkScanning" : "networkScan")}</Button>
      {networks && <Muted className="text-xs">{t("networkScanHint")}</Muted>}
    </div>
    {networks && (networks.length === 0 ? <Muted className="mt-3 text-sm">{t("networkScanEmpty")}</Muted>
      : <ul className="whisli-line mt-3 divide-y divide-[var(--line)] border-y">
        {networks.map(network => <li key={network.ssid}>
          <button type="button" onClick={() => { setSsid(network.ssid); setPassword("") }}
            className={cn("flex w-full items-center gap-3 px-1 py-2 text-left text-sm hover:bg-[var(--raised)]", network.ssid === ssid && "font-medium")}>
            <SignalBars percent={signalPercent(network.rssi)} />
            <span className="min-w-0 flex-1 truncate">{network.ssid}</span>
            {!network.open && <Lock className="whisli-muted size-3.5" aria-label={t("networkSecured")} />}
            <span className="whisli-muted w-12 text-right text-xs tabular-nums">{signalPercent(network.rssi)}%</span>
          </button>
        </li>)}
      </ul>)}

    <MessageBox open={forgetting} type="danger" title={t("networkWifiForgetTitle")} message={t("networkWifiForgetMessage")} detail={info.wifi.savedSsid}
      confirmLabel={t("networkWifiForget")} cancelLabel={t("cancel")} onConfirm={() => void forget()} onClose={() => setForgetting(false)} />
  </Section>
}

function SignalBars({ percent }: { percent: number }) {
  return <span className="flex h-3.5 items-end gap-0.5" aria-hidden>
    {[25, 50, 75, 100].map((level, index) => <span key={level} className={cn("w-1 rounded-sm", percent >= level - 12 ? "bg-[var(--ink)]" : "bg-[var(--line)]")} style={{ height: `${(index + 1) * 25}%` }} />)}
  </span>
}

function AccessPointSection({ info, onChanged }: { info: NetworkInfo; onChanged: () => void }) {
  const { t } = useI18n()
  const { accessPoint } = info
  const [enabled, setEnabled] = useState(accessPoint.enabled)
  const [password, setPassword] = useState("")
  const [result, setResult] = useState<{ ok: boolean; message: string } | null>(null)
  const [confirmOff, setConfirmOff] = useState(false)
  // Opened through the access point itself: switching it off cuts this very connection.
  const viaAccessPoint = Boolean(accessPoint.ip) && window.location.hostname === accessPoint.ip

  async function save() {
    setConfirmOff(false)
    const params: ApiParams = { enabled }
    if (password) params.password = password
    const response = await post("/api/v1/network/ap", params)
    setResult({ ok: response.ok, message: response.message })
    if (response.ok) setPassword("")
    onChanged()
  }

  return <Section bordered className="mt-0 border-t-0 py-6" title={t("networkApTitle")} description={t("networkApDescription", { ssid: accessPoint.ssid })}>
    <div className="mt-4 grid gap-3 sm:grid-cols-[minmax(0,1fr)_minmax(0,1fr)_auto] sm:items-end">
      <label className="flex h-9 items-center gap-2 text-sm">
        <Checkbox checked={enabled} onCheckedChange={next => { setEnabled(next === true); setResult(null) }} />{t("networkApEnabled")}
      </label>
      <Field label={t(accessPoint.customPassword ? "networkApPasswordChange" : "networkApPasswordSet")}>
        <input className="whisli-input h-9" type="password" value={password} minLength={8} maxLength={63} autoComplete="new-password" placeholder={t("networkApPasswordPlaceholder")}
          onChange={event => { setPassword(event.currentTarget.value); setResult(null) }} />
      </Field>
      <Button variant="outline" disabled={enabled === accessPoint.enabled && !password} onClick={() => !enabled && accessPoint.enabled ? setConfirmOff(true) : void save()}>{t("save")}</Button>
    </div>
    {result && <p role="status" className={cn("mt-3 text-sm", result.ok ? "whisli-status-ok" : "whisli-status-error")}>{result.message}</p>}
    <Muted className="mt-3 text-xs">{t("networkApFallbackHint")}</Muted>
    <MessageBox open={confirmOff} type="question" title={t("networkApOffTitle")} message={t(viaAccessPoint ? "networkApOffViaAp" : "networkApOffMessage")}
      confirmLabel={t("networkApOff")} cancelLabel={t("cancel")} onConfirm={() => void save()} onClose={() => setConfirmOff(false)} />
  </Section>
}

function MqttSection({ info, onChanged }: { info: NetworkInfo; onChanged: () => void }) {
  const { t } = useI18n()
  const { mqtt } = info
  const [form, setForm] = useState({ ...mqtt, port: String(mqtt.port), password: "", clearPassword: false })
  const [result, setResult] = useState<{ ok: boolean; message: string } | null>(null)
  const [busy, setBusy] = useState(false)
  const [testing, setTesting] = useState(false)
  // The controller's own name: what Home Assistant shows while no other name is set here.
  const [controllerName, setControllerName] = useState("")
  useEffect(() => {
    getJson<{ devices: Device[] }>("/api/v1/devices").then(data => setControllerName(data.devices.find(device => device.kind === "controller")?.name ?? "")).catch(() => {})
  }, [])
  const set = (patch: Partial<typeof form>) => { setForm(current => ({ ...current, ...patch })); setResult(null) }

  async function save(event: FormEvent) {
    event.preventDefault()
    setBusy(true)
    const params: ApiParams = { enabled: form.enabled, host: form.host.trim(), port: form.port, user: form.user, baseTopic: form.baseTopic.trim(), discovery: form.discovery, discoveryPrefix: form.discoveryPrefix.trim(), deviceName: form.deviceName.trim() }
    if (form.password) params.password = form.password  // only a new password is sent
    if (form.clearPassword) params.clearPassword = true
    const response = await post("/api/v1/network/mqtt", params)
    setBusy(false)
    setResult({ ok: response.ok, message: response.message })
    if (response.ok) set({ password: "", clearPassword: false })
    onChanged()
  }

  // Tries what is in the form, saved or not; an empty password field means the saved password.
  async function testConnection() {
    setTesting(true)
    setResult(null)
    const params: ApiParams = { host: form.host.trim(), port: form.port, user: form.user }
    if (form.password) params.password = form.password
    if (form.clearPassword) params.clearPassword = true
    const response = await post("/api/v1/network/mqtt/test", params)
    setTesting(false)
    const elapsed = typeof response.data.elapsedMs === "number" ? response.data.elapsedMs : 0
    setResult({ ok: response.ok, message: response.ok ? t("mqttTestOk", { ms: elapsed }) : response.message || t("mqttTestError") })
  }

  const text = (key: "host" | "user" | "baseTopic" | "discoveryPrefix" | "deviceName", label: TranslationKey, extra: Partial<{ placeholder: string; required: boolean }> = {}) =>
    <Field label={t(label)}><input className="whisli-input h-9" value={form[key]} maxLength={64} autoComplete="off" onChange={event => set({ [key]: event.currentTarget.value })} {...extra} /></Field>

  return <Section bordered className="mt-6 py-6" title={t("networkMqttConnection")} description={t("networkMqttConnectionHint")}>
    <form onSubmit={event => void save(event)} className="mt-4 grid gap-4">
      <label className="flex items-center gap-2 text-sm"><Checkbox checked={form.enabled} onCheckedChange={next => set({ enabled: next === true })} />{t("networkMqttEnabled")}</label>
      <div className="grid gap-3 sm:grid-cols-[minmax(0,1fr)_8rem]">
        {text("host", "networkMqttHost", { placeholder: "192.168.1.10", required: form.enabled })}
        <Field label={t("networkMqttPort")}><input className="whisli-input h-9" type="number" min={1} max={65535} value={form.port} onChange={event => set({ port: event.currentTarget.value })} /></Field>
      </div>
      <div className="grid gap-3 sm:grid-cols-2">
        {text("user", "networkMqttUser")}
        <Field label={t("password")}>
          <input className="whisli-input h-9" type="password" value={form.password} maxLength={64} autoComplete="new-password" disabled={form.clearPassword}
            placeholder={mqtt.hasPassword && !form.clearPassword ? t("networkMqttPasswordKept") : ""} onChange={event => set({ password: event.currentTarget.value })} />
        </Field>
      </div>
      {mqtt.hasPassword && <label className="whisli-muted -mt-2 flex items-center gap-2 text-xs"><Checkbox checked={form.clearPassword} onCheckedChange={next => set({ clearPassword: next === true, password: "" })} />{t("networkMqttClearPassword")}</label>}
      <div className="grid gap-3 sm:grid-cols-2">
        <div className="grid gap-1.5">
          {text("baseTopic", "networkMqttTopic", { placeholder: "whisli", required: true })}
          <Muted className="text-xs">{t("networkMqttTopicHint", { topic: form.baseTopic.trim() || "whisli" })}</Muted>
        </div>
        <div className="grid gap-1.5">
          {text("discoveryPrefix", "networkMqttDiscoveryPrefix", { placeholder: "homeassistant", required: true })}
          <Muted className="text-xs">{t("networkMqttDiscoveryPrefixHint")}</Muted>
        </div>
      </div>
      <label className="flex items-center gap-2 text-sm"><Checkbox checked={form.discovery} onCheckedChange={next => set({ discovery: next === true })} />{t("networkMqttDiscovery")}</label>
      <div className="grid gap-1.5 sm:max-w-md">
        {text("deviceName", "networkMqttDeviceName", { placeholder: controllerName || "Whisli Controller" })}
        <p className="text-xs">{t("networkMqttDeviceNameShown")} <span className="font-medium">„{form.deviceName.trim() || controllerName || "Whisli Controller"}”</span></p>
        <Muted className="text-xs">{t("networkMqttDeviceNameHint")}</Muted>
      </div>
      <div className="flex flex-wrap items-center gap-3">
        <Button type="submit" variant="outline" disabled={busy || testing}>{t(busy ? "saving" : "save")}</Button>
        <Button type="button" variant="ghost" disabled={busy || testing || !form.host.trim()} onClick={() => void testConnection()}>
          <PlugZap className={cn("size-4", testing && "animate-pulse")} />{t(testing ? "mqttTesting" : "mqttTest")}
        </Button>
        {result && <span role="status" className={cn("text-sm", result.ok ? "whisli-status-ok" : "whisli-status-error")}>{result.message}</span>}
      </div>
    </form>
  </Section>
}


/** Where the link to the broker stands right now. */
function MqttStatus({ info }: { info: NetworkInfo }) {
  const { t } = useI18n()
  const { status, error, host, port } = info.mqtt
  const tone = status === "connected" ? "whisli-status-ok" : status === "error" ? "whisli-status-error" : "whisli-muted"
  return <section className="whisli-raised mt-6 flex items-center gap-3 rounded-xl p-4">
    <span className={cn("size-2.5 shrink-0 rounded-full", status === "connected" ? "bg-[var(--status-ok)]" : status === "error" ? "bg-[var(--status-error)]" : status === "connecting" ? "animate-pulse bg-[var(--ink)]/40" : "bg-[var(--line)]")} />
    <div className="min-w-0 flex-1">
      <p className={cn("text-sm font-medium", tone)}>{t(mqttStatusLabels[status])}</p>
      <p className="whisli-muted truncate text-xs">{status === "error" && error ? error : status === "disabled" ? t("mqttStatusDisabledHint") : `${host}:${port}`}</p>
    </div>
  </section>
}

const mqttStatusLabels: Record<NetworkInfo["mqtt"]["status"], TranslationKey> = {
  disabled: "mqttStatusDisabled", connecting: "mqttStatusConnecting", connected: "mqttStatusConnected", error: "mqttStatusError",
}

/** What the controller publishes and listens to, for anyone wiring it up by hand. */
function MqttTopics({ info }: { info: NetworkInfo }) {
  const { t } = useI18n()
  const base = info.mqtt.baseTopic || "whisli"
  const rows: [string, TranslationKey][] = [
    [`${base}/status`, "mqttTopicStatus"],
    [`${base}/light/<id>/state`, "mqttTopicLightState"],
    [`${base}/light/<id>/set`, "mqttTopicLightSet"],
    [`${base}/input/<n>/action`, "mqttTopicInput"],
  ]
  return <Section bordered className="mt-0 border-t-0 py-6" title={t("mqttTopicsTitle")} description={t(info.mqtt.discovery ? "mqttTopicsDiscovery" : "mqttTopicsNoDiscovery", { prefix: info.mqtt.discoveryPrefix })}>
    <dl className="mt-4 grid gap-x-6 gap-y-2 text-sm sm:grid-cols-[auto_minmax(0,1fr)]">
      {rows.map(([topic, label]) => <div key={topic} className="contents"><dt className="font-mono text-xs">{topic}</dt><dd className="whisli-muted text-xs">{t(label)}</dd></div>)}
    </dl>
  </Section>
}
