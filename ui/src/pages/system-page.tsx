import { Download, FileClock, FileUp, KeyRound, RotateCw, Upload } from "lucide-react"
import { useEffect, useState, type FormEvent } from "react"

import { Field, Muted, PageHeader, Section } from "@/components/page"
import { Button, buttonVariants } from "@/components/ui/button"
import { Checkbox } from "@/components/ui/checkbox"
import { Dialog } from "@/components/ui/dialog"
import { MessageBox } from "@/components/ui/message-box"
import { useI18n, type TranslationKey, type TranslationParams } from "@/i18n"
import { apiFetch, getJson, post } from "@/lib/api"
import { fileSizeKb } from "@/lib/format"
import { cn } from "@/lib/utils"
import type { SystemInfo } from "@/types"

type UpdateLog = { time: string; message: string; kind: "info" | "success" | "error" }

const restartTimeoutMs = 60000
const notAppliedAfterMs = 10000
const kindClass: Record<UpdateLog["kind"], string> = { info: "whisli-muted", success: "whisli-status-ok", error: "whisli-status-error" }

const sleep = (ms: number) => new Promise(resolve => window.setTimeout(resolve, ms))

type RunningFirmware = { version: string; uptimeSeconds: number }

async function runningFirmware(): Promise<RunningFirmware | null> {
  try {
    const response = await apiFetch(`/api/v1/system?check=${Date.now()}`, { cache: "no-store" })
    if (!response.ok) return null
    const info = await response.json() as SystemInfo
    return { version: info.firmware?.version ?? "", uptimeSeconds: info.uptimeSeconds }
  } catch {
    return null // Offline, e.g. while restarting.
  }
}

/** Waits until the controller answers again with its uptime started over. */
async function waitForRestart(before: RunningFirmware | null) {
  const startedAt = Date.now()
  while (Date.now() - startedAt < restartTimeoutMs) {
    await sleep(1000)
    const now = await runningFirmware()
    if (now && (!before || now.uptimeSeconds < before.uptimeSeconds)) return true
  }
  return false
}

/** The build embeds its version (see scripts/build_firmware.sh) as plain text in firmware.bin. */
async function versionInFile(file: File) {
  const text = new TextDecoder("latin1").decode(await file.arrayBuffer())
  return /\d+\.\d{3}\.\d{12}/.exec(text)?.[0] ?? ""
}

export function SystemPage() {
  const { t } = useI18n()
  const [logs, setLogs] = useState<UpdateLog[]>([])
  const [uploading, setUploading] = useState(false)
  const [selectedFile, setSelectedFile] = useState<File | null>(null)

  function addLog(message: string, kind: UpdateLog["kind"] = "info") {
    setLogs(entries => [...entries, { time: new Date().toLocaleTimeString(), message, kind }])
  }
  const log = (key: TranslationKey, params?: TranslationParams, kind: UpdateLog["kind"] = "info") => addLog(t(key, params), kind)

  // A restart or a dropped connection is not proof that OTA succeeded: an aborted
  // upload leaves the old firmware running. Only the version from the uploaded
  // file (or, without one, a changed version after a restart) counts as success.
  async function waitForRestart(before: RunningFirmware | null, expected: string) {
    const startedAt = Date.now()
    log("firmwareWaiting")
    while (Date.now() - startedAt < restartTimeoutMs) {
      await sleep(900)
      const now = await runningFirmware()
      if (!now) continue
      const restarted = !before || now.uptimeSeconds < before.uptimeSeconds
      const isNew = expected ? now.version === expected : now.version !== before?.version
      if (isNew && (restarted || expected !== before?.version)) {
        log("firmwareBack", { version: now.version || "?" }, "success")
        log("firmwareReloading")
        window.setTimeout(() => window.location.reload(), 800)
        return
      }
      // Still the old firmware: the new image did not boot, or (no restart well after the
      // controller's 3 s reboot delay) the upload never completed.
      const notApplied = !restarted && Date.now() - startedAt > notAppliedAfterMs
      if (before && now.version === before.version && ((restarted && expected !== before.version) || notApplied)) {
        log(restarted ? "firmwareRestartedOld" : "firmwareNotApplied", { version: now.version || "?" }, "error")
        setUploading(false)
        return
      }
    }
    log("firmwareRestartTimeout", undefined, "error")
    setUploading(false)
  }

  async function updateFirmware(event: FormEvent<HTMLFormElement>) {
    event.preventDefault()
    const form = event.currentTarget
    const file = selectedFile ?? (form.elements.namedItem("firmware") as HTMLInputElement).files?.[0]
    if (!file) { log("firmwareMissing", undefined, "error"); return }

    setUploading(true)
    const [before, expected] = await Promise.all([runningFirmware(), versionInFile(file)])
    log("firmwareUploadStart", { name: file.name, size: fileSizeKb(file) })
    if (expected) log("firmwareFileVersion", { version: expected, current: before?.version || "?" })
    const request = new XMLHttpRequest()
    let lastProgress = 0
    request.open("POST", "/api/v1/firmware")
    request.setRequestHeader("Authorization", "Session")  // see apiFetch
    request.upload.onprogress = progress => {
      if (!progress.lengthComputable) return
      const percent = Math.floor(progress.loaded / progress.total * 100)
      if (percent === 100 || percent >= lastProgress + 10) {
        lastProgress = percent
        log("firmwareProgress", { percent })
      }
    }
    request.onerror = () => {
      // The browser cannot distinguish a failed transfer from a connection
      // dropped during reboot. Report the uncertainty and verify the version.
      log("firmwareConnectionError", undefined, "info")
      void waitForRestart(before, expected)
    }
    request.onload = () => {
      if (request.status < 200 || request.status >= 300) {
        let message = ""
        try { message = (JSON.parse(request.responseText) as { message?: string }).message ?? "" } catch { /* not JSON */ }
        addLog(message || t("firmwareHttpError", { status: request.status }), "error")
        setUploading(false)
        return
      }
      log("firmwareSaved", undefined, "success")
      log("firmwareRestartSoon")
      void waitForRestart(before, expected)
    }
    // The image goes as the raw request body; the controller streams it straight into flash.
    request.setRequestHeader("Content-Type", "application/octet-stream")
    request.send(file)
  }

  return <>
    <PageHeader eyebrow={t("navSystem")} title={t("navSystemSettings")} />
    <PasswordSection />
    <RestartSection />
    <BackupSection />
    <Section bordered className="mt-0 max-w-3xl border-t-0 py-6" title={t("firmwareUpdateTitle")} description={t("firmwareDescription")}>
      <form onSubmit={event => void updateFirmware(event)} className="mt-4 space-y-3">
        <div className="flex flex-wrap items-center gap-3">
          <label className="whisli-raised whisli-line inline-flex cursor-pointer items-center gap-2 rounded-md border px-3 py-2 text-sm font-medium transition-colors hover:border-[var(--ink)]">
            <FileUp className="size-4" />{t("firmwareChooseFile")}
            <input className="sr-only" name="firmware" type="file" accept=".bin,application/octet-stream" required disabled={uploading} onChange={event => { const file = event.currentTarget.files?.[0] ?? null; setSelectedFile(file); if (file) log("firmwareSelected", { name: file.name, size: fileSizeKb(file) }) }} />
          </label>
          <Button variant="outline" type="submit" disabled={uploading || !selectedFile}><Upload className="size-4" />{uploading ? t("firmwareUploading") : t("firmwareUpload")}</Button>
        </div>
        {selectedFile && <div className="whisli-raised whisli-line grid gap-1 rounded-md border p-3 text-sm sm:grid-cols-[auto_minmax(0,1fr)_auto] sm:items-center sm:gap-3">
          <FileClock className="whisli-muted size-4" />
          <span className="truncate font-medium" title={selectedFile.name}>{selectedFile.name}</span>
          <span className="whisli-muted text-xs sm:text-right">{fileSizeKb(selectedFile)} KB · {new Date(selectedFile.lastModified).toLocaleString()}</span>
        </div>}
      </form>
      <div aria-live="polite" className="whisli-raised mt-5 rounded-md p-3 font-mono text-xs">
        <Muted className="mb-2">{t("firmwareLogTitle")}</Muted>
        {logs.length
          ? <ol className="space-y-1">{logs.map((entry, index) => <li key={index} className={kindClass[entry.kind]}><time className="mr-3 opacity-70">{entry.time}</time><span>&gt; {entry.message}</span></li>)}</ol>
          : <Muted>{t("firmwareLogEmpty")}</Muted>}
      </div>
    </Section>
  </>
}

/** Shows whether the default password is still in use; the change happens in a dialog. */
function PasswordSection() {
  const { t } = useI18n()
  const [usingDefault, setUsingDefault] = useState(false)
  const [changing, setChanging] = useState(false)
  const [changed, setChanged] = useState(false)
  useEffect(() => { getJson<{ defaultPassword: boolean }>("/api/v1/auth").then(info => setUsingDefault(info.defaultPassword), () => undefined) }, [])

  return <Section bordered className="mt-6 max-w-3xl py-6" title={t("passwordTitle")} description={t("passwordDescription")}
    actions={<Button variant="outline" onClick={() => { setChanged(false); setChanging(true) }}><KeyRound className="size-4" />{t("passwordChange")}</Button>}>
    {changed
      ? <p role="status" className="whisli-status-ok mt-3">{t("passwordChanged")}</p>
      : usingDefault && <p role="status" className="whisli-status-error mt-3">{t("passwordDefaultWarning")}</p>}
    <Muted className="mt-3 text-xs">{t("passwordResetHint")}</Muted>
    {changing && <PasswordDialog onClose={() => setChanging(false)} onChanged={isDefault => { setChanging(false); setChanged(true); setUsingDefault(isDefault) }} />}
  </Section>
}

function PasswordDialog({ onClose, onChanged }: { onClose: () => void; onChanged: (isDefault: boolean) => void }) {
  const { t } = useI18n()
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState<TranslationKey | null>(null)

  async function submit(event: FormEvent<HTMLFormElement>) {
    event.preventDefault()
    const data = new FormData(event.currentTarget)
    const [current, password, repeated] = ["current", "password", "repeated"].map(name => String(data.get(name) ?? ""))
    if (password !== repeated) { setError("passwordMismatch"); return }
    setBusy(true)
    const response = await post("/api/v1/auth/password", { current, password })
    setBusy(false)
    if (response.ok) {
      const info = await getJson<{ defaultPassword: boolean }>("/api/v1/auth").catch(() => ({ defaultPassword: false }))
      onChanged(info.defaultPassword)
    } else {
      setError(response.status === 401 ? "passwordWrongCurrent" : response.status === 400 ? "passwordInvalid" : "operationFailed")
    }
  }

  return <Dialog open busy={busy} onClose={onClose} labelledBy="password-dialog-title" describedBy="password-dialog-description">
    <form onSubmit={event => void submit(event)} onChange={() => setError(null)}>
      <div className="flex items-start gap-3">
        <span className="whisli-raised flex size-9 shrink-0 items-center justify-center rounded-full"><KeyRound className="size-4" /></span>
        <div className="min-w-0">
          <h2 id="password-dialog-title" className="font-semibold">{t("passwordChange")}</h2>
          <p id="password-dialog-description" className="whisli-muted mt-1 text-sm">{t("passwordDialogDescription")}</p>
        </div>
      </div>
      <input type="text" name="username" autoComplete="username" value="whisli" readOnly hidden />
      <div className="mt-5 grid gap-3">
        <Field label={t("passwordCurrent")}><input className="whisli-input h-9" type="password" name="current" autoComplete="current-password" required disabled={busy} /></Field>
        <Field label={t("passwordNew")}><input className="whisli-input h-9" type="password" name="password" autoComplete="new-password" maxLength={64} required disabled={busy} /></Field>
        <Field label={t("passwordRepeat")}><input className="whisli-input h-9" type="password" name="repeated" autoComplete="new-password" maxLength={64} required disabled={busy} /></Field>
      </div>
      {error && <p role="alert" className="whisli-status-error mt-4 text-sm">{t(error)}</p>}
      <div className="mt-6 flex justify-end gap-2">
        <Button variant="outline" disabled={busy} onClick={onClose}>{t("cancel")}</Button>
        <Button type="submit" disabled={busy}>{t(busy ? "saving" : "passwordChange")}</Button>
      </div>
    </form>
  </Dialog>
}

/** Local date and time for file names: 20261015.102345 */
function fileStamp(date: Date) {
  const two = (value: number) => String(value).padStart(2, "0")
  return `${date.getFullYear()}${two(date.getMonth() + 1)}${two(date.getDate())}.${two(date.getHours())}${two(date.getMinutes())}${two(date.getSeconds())}`
}

type BackupFile = { name: string; text: string; firmware: string; created: number; entries: number; secrets: boolean }
type BackupStatus = { key: TranslationKey; className: string; message?: string }

/** Saves the configuration to a file and loads it back (which restarts the controller). */
function BackupSection() {
  const { t } = useI18n()
  const [pending, setPending] = useState<BackupFile | null>(null)
  const [busy, setBusy] = useState(false)
  const [status, setStatus] = useState<BackupStatus | null>(null)
  // Network passwords go into the file only on request, encrypted with this backup password.
  const [withSecrets, setWithSecrets] = useState(false)
  const [passphrase, setPassphrase] = useState("")
  const [restorePassphrase, setRestorePassphrase] = useState("")

  async function download() {
    setStatus(null)
    if (withSecrets && passphrase.length < 8) { setStatus({ key: "backupPassphraseShort", className: "whisli-status-error" }); return }
    try {
      const response = withSecrets
        ? await apiFetch("/api/v1/config/backup", { method: "POST", cache: "no-store", body: new URLSearchParams({ passphrase }) })
        : await apiFetch("/api/v1/config/backup", { cache: "no-store" })
      if (!response.ok) throw new Error(String(response.status))
      const link = document.createElement("a")
      link.href = URL.createObjectURL(await response.blob())
      link.download = `whisli_config_${fileStamp(new Date())}.json`
      link.click()
      URL.revokeObjectURL(link.href)
    } catch {
      setStatus({ key: "backupDownloadFailed", className: "whisli-status-error" })
    }
  }

  async function choose(file: File) {
    setStatus(null)
    const text = await file.text()
    try {
      const data = JSON.parse(text) as { application?: string; firmware?: string; created?: number; entries?: unknown[]; secrets?: unknown }
      if (data.application !== "Whisli" || !Array.isArray(data.entries)) throw new Error("not a backup")
      setRestorePassphrase("")
      setPending({ name: file.name, text, firmware: data.firmware ?? "", created: data.created ?? 0, entries: data.entries.length, secrets: Boolean(data.secrets) })
    } catch {
      setStatus({ key: "backupNotABackup", className: "whisli-status-error" })
    }
  }

  async function restore(file: BackupFile) {
    setBusy(true)
    const before = await runningFirmware()
    let result: { ok: boolean; message: string }
    try {
      const headers: Record<string, string> = { "Content-Type": "application/json" }
      // UTF-8 → base64, so any characters survive the header.
      if (file.secrets && restorePassphrase) headers["X-Backup-Passphrase"] = btoa(String.fromCharCode(...new TextEncoder().encode(restorePassphrase)))
      const response = await apiFetch("/api/v1/config/restore", { method: "POST", headers, body: file.text })
      const data = await response.json().catch(() => ({})) as { message?: string }
      result = { ok: response.ok, message: data.message ?? "" }
    } catch {
      result = { ok: false, message: "" }
    }
    setPending(null)
    if (!result.ok) {
      setBusy(false)
      setStatus({ key: "backupRestoreFailed", className: "whisli-status-error", message: result.message })
      return
    }
    // The controller says whether the network passwords came along; keep that in view.
    setStatus({ key: "backupRestoring", className: "whisli-muted", message: result.message })
    const back = await waitForRestart(before)
    setBusy(false)
    setStatus(back ? { key: "backupRestored", className: "whisli-status-ok", message: result.message.replace(/\s*Sterownik uruchamia się ponownie\.?$/, "") || undefined }
      : { key: "restartFailed", className: "whisli-status-error" })
  }

  return <Section bordered className="mt-0 max-w-3xl border-t-0 py-6" title={t("backupTitle")} description={t("backupDescription")}>
    <div className="mt-4 flex flex-wrap items-center gap-3">
      <Button variant="outline" disabled={busy} onClick={() => void download()}><Download className="size-4" />{t("backupDownload")}</Button>
      <label className={cn(buttonVariants({ variant: "outline" }), busy ? "pointer-events-none opacity-50" : "cursor-pointer")}>
        <Upload className="size-4" />{t("backupRestore")}
        <input className="sr-only" type="file" accept=".json,application/json" disabled={busy}
          onChange={event => { const file = event.currentTarget.files?.[0]; event.currentTarget.value = ""; if (file) void choose(file) }} />
      </label>
      {status && <span role="status" className={`text-sm ${status.className}`}>{status.message || t(status.key)}</span>}
    </div>
    <div className="mt-4 grid gap-2">
      <label className="flex items-center gap-2 text-sm">
        <Checkbox checked={withSecrets} disabled={busy} onCheckedChange={next => { setWithSecrets(next === true); setStatus(null) }} />{t("backupWithSecrets")}
      </label>
      {withSecrets && <Field label={t("backupPassphrase")} className="max-w-sm">
        <input className="whisli-input h-9" type="password" value={passphrase} minLength={8} maxLength={64} autoComplete="new-password" placeholder={t("backupPassphrasePlaceholder")}
          onChange={event => setPassphrase(event.currentTarget.value)} />
      </Field>}
      <Muted className="text-xs">{t(withSecrets ? "backupSecretsHint" : "backupNoSecretsHint")}</Muted>
    </div>
    <MessageBox
      open={pending !== null}
      type="question"
      title={t("backupConfirmTitle")}
      message={t("backupConfirmMessage")}
      detail={pending && <>
        <span className="block">{pending.name}</span>
        <span className="whisli-muted block text-xs font-normal">
          {t("backupFileInfo", { count: pending.entries, version: pending.firmware || "?" })}
          {pending.created > 0 && ` · ${new Date(pending.created * 1000).toLocaleString()}`}
        </span>
        {pending.secrets && <Field label={t("backupPassphraseRestore")} className="mt-3 font-normal">
          <input className="whisli-input h-9" type="password" value={restorePassphrase} maxLength={64} autoComplete="off" placeholder={t("backupPassphraseOptional")}
            onChange={event => setRestorePassphrase(event.currentTarget.value)} />
        </Field>}
      </>}
      confirmLabel={t(busy ? "backupRestoreBusy" : "backupRestoreConfirm")}
      cancelLabel={t("cancel")}
      busy={busy}
      onConfirm={() => { if (pending) void restore(pending) }}
      onClose={() => setPending(null)}
    />
  </Section>
}

type RestartState = "idle" | "confirm" | "restarting" | "back" | "failed"

/** Restarts the controller and waits until it answers again (its uptime starts over). */
function RestartSection() {
  const { t } = useI18n()
  const [state, setState] = useState<RestartState>("idle")

  async function restart() {
    const before = await runningFirmware()
    const result = await post("/api/v1/system/restart")
    if (!result.ok) { setState("failed"); return }
    setState("restarting")
    setState(await waitForRestart(before) ? "back" : "failed")
  }

  const status: Partial<Record<RestartState, { key: TranslationKey; className: string }>> = {
    restarting: { key: "restartWaiting", className: "whisli-muted" },
    back: { key: "restartDone", className: "whisli-status-ok" },
    failed: { key: "restartFailed", className: "whisli-status-error" },
  }
  return <Section bordered className="mt-0 max-w-3xl border-t-0 py-6" title={t("restartTitle")} description={t("restartDescription")}>
    <div className="mt-4 flex flex-wrap items-center gap-3">
      <Button variant="outline" disabled={state === "restarting"} onClick={() => setState("confirm")}>
        <RotateCw className={state === "restarting" ? "size-4 animate-spin" : "size-4"} />{t(state === "restarting" ? "restartRunning" : "restartButton")}
      </Button>
      {status[state] && <span role="status" className={`text-sm ${status[state]!.className}`}>{t(status[state]!.key)}</span>}
    </div>
    <MessageBox
      open={state === "confirm"}
      type="question"
      title={t("restartConfirmTitle")}
      message={t("restartConfirmMessage")}
      confirmLabel={t("restartButton")}
      cancelLabel={t("cancel")}
      onConfirm={() => void restart()}
      onClose={() => setState("idle")}
    />
  </Section>
}
