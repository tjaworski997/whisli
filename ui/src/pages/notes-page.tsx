import { useEffect, useRef, useState } from "react"

import { ErrorText, Muted, PageHeader } from "@/components/page"
import { useI18n } from "@/i18n"
import { apiFetch, getJson } from "@/lib/api"

type NoteData = { text: string; modified: number; maxLength: number }
type SaveState = "saved" | "pending" | "saving" | "error"

/** Wait after the last keystroke before saving. Leaving the field or the page saves at once. */
const saveDelayMs = 1200

/** Plain-text installation notes, saved as you type. No save button. */
export function NotesPage() {
  const { t, locale } = useI18n()
  const [note, setNote] = useState<NoteData | null>(null)
  const [text, setText] = useState("")
  const [modified, setModified] = useState(0)
  const [state, setState] = useState<SaveState>("saved")
  const [loadError, setLoadError] = useState(false)
  const [caret, setCaret] = useState({ line: 1, column: 1 })
  const savedText = useRef("")
  const latestText = useRef("")
  const timer = useRef<number | undefined>(undefined)

  useEffect(() => {
    getJson<NoteData>("/api/v1/notes").then(data => {
      setNote(data)
      setText(data.text)
      setModified(data.modified)
      savedText.current = data.text
      latestText.current = data.text
    }, () => setLoadError(true))
  }, [])

  async function save(keepalive = false) {
    window.clearTimeout(timer.current)
    const body = latestText.current
    if (body === savedText.current) { setState("saved"); return }
    setState("saving")
    try {
      // keepalive lets the last save finish while the tab closes.
      const response = await apiFetch("/api/v1/notes", {
        method: "POST", body, keepalive,
        headers: { "Content-Type": "text/plain; charset=utf-8", "X-Client-Time": String(Math.floor(Date.now() / 1000)) },
      })
      if (!response.ok) throw new Error(String(response.status))
      const result = await response.json() as { modified: number }
      savedText.current = body
      setModified(result.modified)
      setState(latestText.current === body ? "saved" : "pending")
    } catch {
      setState("error")
    }
  }

  // Leaving the page (another tab, closing it, another panel page) saves what is left.
  useEffect(() => {
    const flush = () => { if (latestText.current !== savedText.current) void save(true) }
    const onHidden = () => { if (document.visibilityState === "hidden") flush() }
    window.addEventListener("pagehide", flush)
    document.addEventListener("visibilitychange", onHidden)
    return () => {
      window.removeEventListener("pagehide", flush)
      document.removeEventListener("visibilitychange", onHidden)
      flush()
    }
  }, [])

  function change(next: string) {
    setText(next)
    latestText.current = next
    setState(next === savedText.current ? "saved" : "pending")
    window.clearTimeout(timer.current)
    timer.current = window.setTimeout(() => void save(), saveDelayMs)
  }

  function trackCaret(field: HTMLTextAreaElement) {
    const before = field.value.slice(0, field.selectionStart)
    const lines = before.split("\n")
    setCaret({ line: lines.length, column: lines[lines.length - 1].length + 1 })
  }

  const maxLength = note?.maxLength ?? 10000
  const formatted = modified ? new Date(modified * 1000).toLocaleString(locale === "pl" ? "pl-PL" : "en-GB", { dateStyle: "medium", timeStyle: "short" }) : null
  const stateLabel = { saved: t("notesSaved"), pending: t("notesPending"), saving: t("notesSaving"), error: t("notesSaveError") }[state]

  return <div className="flex h-[calc(100vh-56px-3.5rem)] min-h-[420px] flex-col md:h-[calc(100vh-56px-3.5rem)]">
    <PageHeader title={t("notesTitle")} description={t("notesDescription")} />
    {loadError && <ErrorText>{t("notesLoadError")}</ErrorText>}
    {!note && !loadError ? <Muted className="mt-6">{t("connecting")}</Muted> : note && <>
      {/* One editor surface: borderless text area with a status bar, like a code editor. */}
      <div className="whisli-line mt-4 flex min-h-0 flex-1 flex-col overflow-hidden rounded-lg border">
        <textarea
          className="min-h-0 w-full flex-1 resize-none bg-transparent px-5 py-4 font-mono text-[11px] leading-[18px] outline-none placeholder:text-[var(--muted)] placeholder:opacity-60"
          value={text}
          maxLength={maxLength}
          spellCheck={false}
          aria-label={t("notesTitle")}
          placeholder={t("notesPlaceholder")}
          onChange={event => { change(event.target.value); trackCaret(event.target) }}
          onSelect={event => trackCaret(event.currentTarget)}
          onBlur={() => void save()}
        />
        <footer className="whisli-line whisli-raised whisli-muted flex flex-wrap items-center gap-x-5 gap-y-1 border-t px-4 py-1.5 text-[11px] tabular-nums">
          <span>{t("notesCaret", { line: caret.line, column: caret.column })}</span>
          <span>{t("notesLength", { count: text.length.toLocaleString(locale), max: maxLength.toLocaleString(locale) })}</span>
          <span className="hidden sm:inline">{formatted ? t("notesModified", { date: formatted }) : t("notesNeverModified")}</span>
          <span className={state === "error" ? "whisli-status-error ml-auto" : "ml-auto"} role="status">{stateLabel}</span>
        </footer>
      </div>
    </>}
  </div>
}
