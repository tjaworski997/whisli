import { RefreshCw } from "lucide-react"
import { useEffect, useRef, useState } from "react"

import { ErrorText, Muted, PageHeader } from "@/components/page"
import { Button } from "@/components/ui/button"
import { useI18n } from "@/i18n"
import { getJson } from "@/lib/api"
import { formatLogAge } from "@/lib/format"
import { logCategoryLabels, logLevelLabels } from "@/lib/labels"
import type { DeviceLogEntry, DeviceLogResponse } from "@/types"

const pageSize = "128"
const levelClass: Record<string, string> = { info: "whisli-status-ok", warning: "whisli-muted", error: "whisli-status-error" }

export function LogsPage() {
  const { t } = useI18n()
  const [level, setLevel] = useState("all")
  const [category, setCategory] = useState("all")
  const [entries, setEntries] = useState<DeviceLogEntry[]>([])
  const [capacity, setCapacity] = useState(4096)
  const [hasOlder, setHasOlder] = useState(false)
  const [error, setError] = useState(false)
  const [loading, setLoading] = useState(true)
  const [loadingOlder, setLoadingOlder] = useState(false)
  const [reloadNonce, setReloadNonce] = useState(0)
  const entriesRef = useRef<DeviceLogEntry[]>([])
  const sessionRef = useRef<number | null>(null)

  const fetchLogs = (options: Record<string, string> = {}) =>
    getJson<DeviceLogResponse>(`/api/v1/logs?${new URLSearchParams({ level, category, limit: pageSize, ...options })}`)

  function show(next: DeviceLogEntry[]) {
    entriesRef.current = next
    setEntries(next)
  }

  // Full load on filter change / manual refresh, then poll only for entries newer than the last one.
  // A new `sessionId` means the controller restarted, so the buffer is reloaded from scratch.
  useEffect(() => {
    let active = true
    let pending = false
    sessionRef.current = null
    show([])
    setHasOlder(false)
    setLoading(true)

    async function poll() {
      if (pending || document.hidden) return
      pending = true
      try {
        const latestId = entriesRef.current.at(-1)?.id ?? 0
        let result = await fetchLogs(sessionRef.current === null ? {} : { after: String(latestId) })
        if (!active) return
        const restarted = sessionRef.current !== null && result.sessionId !== sessionRef.current
        if (restarted) result = await fetchLogs()
        if (!active) return
        if (sessionRef.current === null || restarted) {
          sessionRef.current = result.sessionId
          show(result.entries)
          setHasOlder(result.hasMore)
        } else {
          const known = new Set(entriesRef.current.map(entry => entry.id))
          const fresh = result.entries.filter(entry => !known.has(entry.id))
          if (fresh.length) show([...entriesRef.current, ...fresh])
        }
        setCapacity(result.capacity)
        setError(false)
      } catch {
        if (active) setError(true)
      } finally {
        pending = false
        if (active) setLoading(false)
      }
    }

    void poll()
    const timer = window.setInterval(() => void poll(), 2000)
    return () => { active = false; window.clearInterval(timer) }
  }, [level, category, reloadNonce])

  async function loadOlder() {
    const oldest = entriesRef.current[0]
    if (loadingOlder || !oldest) return
    setLoadingOlder(true)
    try {
      const result = await fetchLogs({ before: String(oldest.id) })
      if (sessionRef.current !== null && result.sessionId !== sessionRef.current) { setReloadNonce(value => value + 1); return }
      const olderIds = new Set(result.entries.map(entry => entry.id))
      show([...result.entries, ...entriesRef.current.filter(entry => !olderIds.has(entry.id))])
      setHasOlder(result.hasMore)
    } catch {
      setError(true)
    } finally {
      setLoadingOlder(false)
    }
  }

  return <>
    <PageHeader
      eyebrow={t("navSystem")}
      title={t("systemLogs")}
      description={`${t("logsRetentionHint")} ${capacity} ${t("logEntries")}. ${t("logsRestartHint")}`}
      actions={<Button variant="outline" onClick={() => setReloadNonce(value => value + 1)}><RefreshCw className="size-4" />{t("refreshLogs")}</Button>}
    />
    <div className="mt-5 flex flex-wrap gap-2">
      <select className="whisli-select" value={level} onChange={event => setLevel(event.currentTarget.value)} aria-label={t("logLevel")}>
        <option value="all">{t("logAllLevels")}</option>
        {Object.entries(logLevelLabels).map(([value, key]) => <option key={value} value={value}>{t(key)}</option>)}
      </select>
      <select className="whisli-select" value={category} onChange={event => setCategory(event.currentTarget.value)} aria-label={t("logCategory")}>
        <option value="all">{t("logAllCategories")}</option>
        {Object.entries(logCategoryLabels).map(([value, key]) => <option key={value} value={value}>{t(key)}</option>)}
      </select>
    </div>
    {error && <ErrorText>{t("logsLoadError")}</ErrorText>}
    <div className="whisli-line mt-4 border-y" aria-live="polite">
      {loading && !entries.length ? <Muted className="py-4">{t("logsLoading")}</Muted>
        : !entries.length ? <Muted className="py-4">{t("logsEmpty")}</Muted>
        : <>
          {hasOlder && <div className="whisli-line border-b py-2"><Button variant="ghost" disabled={loadingOlder} onClick={() => void loadOlder()}>{loadingOlder ? t("logsLoading") : t("loadOlderLogs")}</Button></div>}
          <ol className="divide-y divide-[var(--line)]">
            {[...entries].reverse().map(entry => <li key={entry.id} className="grid gap-1 py-3 sm:grid-cols-[92px_84px_104px_minmax(0,1fr)] sm:items-start sm:gap-3">
              <time className="whisli-muted font-mono text-xs" title={`${entry.uptimeMs} ms`}>{formatLogAge(entry.uptimeMs)}</time>
              <span className={`text-xs font-medium ${levelClass[entry.level] ?? "whisli-muted"}`}>{t(logLevelLabels[entry.level] ?? "logInfo")}</span>
              <span className="whisli-muted text-xs">{logCategoryLabels[entry.category] ? t(logCategoryLabels[entry.category]) : entry.category}</span>
              <span className="break-words">{entry.message}</span>
            </li>)}
          </ol>
        </>}
    </div>
  </>
}
