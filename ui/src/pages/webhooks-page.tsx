import { ChevronDown, ChevronRight, Globe, Play, Plus, Trash2 } from "lucide-react"
import { useEffect, useRef, useState } from "react"

import { AutoSaveInput, ErrorText, Muted, PageHeader } from "@/components/page"
import { Button } from "@/components/ui/button"
import { Checkbox } from "@/components/ui/checkbox"
import { MessageBox } from "@/components/ui/message-box"
import { useI18n, type TranslationKey } from "@/i18n"
import { getJson, post } from "@/lib/api"
import { actionCountKey } from "@/lib/format"
import { cn } from "@/lib/utils"
import type { ActionPanelData, HttpHook, HttpHookResult } from "@/types"

type HooksData = { hooks: HttpHook[]; limits: { url: number; headers: number; body: number } }

const methods: HttpHook["method"][] = ["GET", "POST", "PUT", "DELETE"]
const bodyKinds: { value: HttpHook["body"]; label: TranslationKey }[] = [
  { value: "none", label: "hookBodyNone" }, { value: "json", label: "hookBodyJson" }, { value: "form", label: "hookBodyForm" }, { value: "text", label: "hookBodyText" },
]
const variables = ["button", "buttonId", "gesture", "zone", "controller", "ip", "time", "uptime"] as const

/** Example values for the preview; the controller fills the real ones when it sends. */
function sampleValues(): Record<(typeof variables)[number], string> {
  return {
    button: "Włącznik salon", buttonId: "controller.input-1", gesture: "doubleClick", zone: "Salon", controller: "Whisli Controller",
    ip: window.location.hostname, time: new Date().toISOString().replace(/\.\d+Z$/, "Z"), uptime: "3600",
  }
}

/** The same substitution as the controller: values encoded for where they land. */
function expand(text: string, encoding: "url" | "json" | "plain") {
  const values = sampleValues() as Record<string, string>
  return text.replace(/\{(\w+)\}/g, (whole, name: string) => {
    const value = values[name]
    if (value === undefined) return whole
    if (encoding === "url") return encodeURIComponent(value)
    if (encoding === "json") return JSON.stringify(value).slice(1, -1)
    return value
  })
}

/** Actions › Webhooks: named HTTP requests that button actions can call. */
export function WebhooksPage() {
  const { t } = useI18n()
  const [data, setData] = useState<HooksData | null>(null)
  const [usage, setUsage] = useState<Record<string, number>>({})
  const [error, setError] = useState("")
  const [busy, setBusy] = useState(false)
  const [open, setOpen] = useState<number | null>(null)
  const [addedId, setAddedId] = useState<number | null>(null)
  const [removing, setRemoving] = useState<HttpHook | null>(null)

  async function refresh() {
    try {
      setData(await getJson<HooksData>("/api/v1/hooks"))
      const panel = await getJson<ActionPanelData>("/api/v1/action-panel")
      const counts: Record<string, number> = {}
      for (const action of panel.actions) if (action.targetId.startsWith("hook-")) counts[action.targetId] = (counts[action.targetId] ?? 0) + 1
      setUsage(counts)
    } catch {
      setError(t("hooksLoadError"))
    }
  }
  useEffect(() => { void refresh() }, [])

  // Like a new light or zone: a record with a default name, opened and selected to type over.
  async function add() {
    const base = t("hookDefaultName")
    const taken = new Set((data?.hooks ?? []).map(hook => hook.name.toLocaleLowerCase()))
    let name = base
    for (let number = 2; taken.has(name.toLocaleLowerCase()); number += 1) name = `${base} ${number}`
    setBusy(true)
    const result = await post("/api/v1/hooks", { name })
    setBusy(false)
    if (!result.ok) { setError(result.message || t("hookSaveError")); return }
    const id = Number(result.data.id) || null
    await refresh()
    setOpen(id)
    setAddedId(id)
  }

  async function save(hook: HttpHook) {
    const result = await post("/api/v1/hooks/update", { ...hook, insecure: hook.insecure })
    setError(result.ok ? "" : result.message || t("hookSaveError"))
    if (result.ok) setData(current => current && { ...current, hooks: current.hooks.map(item => item.id === hook.id ? hook : item) })
    return result.ok
  }

  async function remove(hook: HttpHook) {
    setBusy(true)
    const result = await post("/api/v1/hooks/remove", { id: hook.id })
    setBusy(false)
    setRemoving(null)
    setError(result.ok ? "" : result.message || t("hookSaveError"))
    await refresh()
  }

  return <>
    <PageHeader eyebrow={t("automation")} title={t("hooksTitle")} description={t("hooksDescription")} actions={
      <Button variant="outline" disabled={busy || !data} onClick={() => void add()}><Plus className="size-4" />{t("hookAdd")}</Button>
    } />
    {error && <ErrorText>{error}</ErrorText>}
    {!data ? <Muted className="mt-6">{t("connecting")}</Muted>
      : data.hooks.length === 0 ? <Muted className="whisli-line mt-5 border-t py-4">{t("hooksEmpty")}</Muted>
      : <div className="whisli-line mt-5 divide-y divide-[var(--line)] border-y">
        {data.hooks.map(hook => <HookRow key={hook.id} hook={hook} limits={data.limits} expanded={open === hook.id} focused={addedId === hook.id} uses={usage[`hook-${hook.id}`] ?? 0}
          onToggle={() => setOpen(open === hook.id ? null : hook.id)} onFocused={() => setAddedId(null)} onSave={save} onRemove={() => setRemoving(hook)} />)}
      </div>}
    <MessageBox
      open={removing !== null}
      type="danger"
      title={t("hookRemoveTitle")}
      message={removing && usage[`hook-${removing.id}`] ? t("hookRemoveUsed", { count: usage[`hook-${removing.id}`] }) : t("hookRemoveMessage")}
      detail={removing?.name}
      confirmLabel={t("remove")}
      cancelLabel={t("cancel")}
      busy={busy}
      onConfirm={() => { if (removing) void remove(removing) }}
      onClose={() => setRemoving(null)}
    />
  </>
}

function HookRow({ hook, limits, expanded, focused, uses, onToggle, onFocused, onSave, onRemove }: {
  hook: HttpHook
  limits: HooksData["limits"]
  expanded: boolean
  focused: boolean
  uses: number
  onToggle: () => void
  onFocused: () => void
  onSave: (hook: HttpHook) => Promise<boolean>
  onRemove: () => void
}) {
  const { t } = useI18n()
  // Edited locally, saved when a field is left or a choice changes.
  const [draft, setDraft] = useState(hook)
  useEffect(() => setDraft(hook), [hook])
  const [testing, setTesting] = useState(false)
  const [result, setResult] = useState<HttpHookResult | null>(null)
  const bodyRef = useRef<HTMLTextAreaElement>(null)

  const change = (next: Partial<HttpHook>, saveNow = false) => {
    const updated = { ...draft, ...next }
    setDraft(updated)
    if (saveNow) void onSave(updated)
  }
  const commit = () => { if (JSON.stringify(draft) !== JSON.stringify(hook)) void onSave(draft) }

  async function test() {
    if (JSON.stringify(draft) !== JSON.stringify(hook) && !await onSave(draft)) return
    setTesting(true)
    setResult(null)
    const response = await post("/api/v1/hooks/test", { id: hook.id })
    setTesting(false)
    setResult(response.ok ? response.data as unknown as HttpHookResult : { sent: false, status: 0, error: response.message || t("hookTestFailed"), response: "" })
  }

  // Puts {name} where the cursor is in the body.
  function insertVariable(name: string) {
    const field = bodyRef.current
    const token = `{${name}}`
    if (!field) { change({ bodyTemplate: draft.bodyTemplate + token }); return }
    const start = field.selectionStart, end = field.selectionEnd
    const text = draft.bodyTemplate.slice(0, start) + token + draft.bodyTemplate.slice(end)
    change({ bodyTemplate: text }, true)
    requestAnimationFrame(() => { field.focus(); field.setSelectionRange(start + token.length, start + token.length) })
  }

  const hasBody = draft.method !== "GET" && draft.body !== "none"
  const preview = hasBody ? expand(draft.bodyTemplate, draft.body === "json" ? "json" : draft.body === "form" ? "url" : "plain") : ""
  const resultTone = result && result.sent && result.status < 400 ? "whisli-status-ok" : "whisli-status-error"

  return <div>
    <div className="flex items-center gap-3 py-3">
      <button type="button" className="whisli-muted shrink-0" aria-expanded={expanded} aria-label={t(expanded ? "collapse" : "expand")} onClick={onToggle}>
        {expanded ? <ChevronDown className="size-4" /> : <ChevronRight className="size-4" />}
      </button>
      <Globe className="whisli-muted size-5 shrink-0" aria-hidden />
      <div className="min-w-0 flex-1">
        <AutoSaveInput inline className="font-medium" value={hook.name} maxLength={48} aria-label={t("hookName")}
          autoFocus={focused} onFocus={event => { if (focused) { event.currentTarget.select(); onFocused() } }}
          onSave={name => { if (name) void onSave({ ...draft, name }) }} />
        <p className="whisli-muted truncate font-mono text-xs">
          <span className="whisli-line mr-2 rounded border px-1 py-px text-[10px]">{hook.method}</span>
          {hook.url || t("hookNoUrl")}
          {uses > 0 && <span className="ml-2 font-sans">· {t(actionCountKey(uses), { count: uses })}</span>}
        </p>
      </div>
      <Button variant="outline" disabled={testing || !draft.url} onClick={() => void test()}><Play className="size-3.5" />{testing ? t("hookTesting") : t("testAction")}</Button>
      <Button variant="dangerGhost" size="icon" aria-label={t("hookRemove")} title={t("hookRemove")} onClick={onRemove}><Trash2 className="size-4" /></Button>
    </div>

    {result && <div className={cn("whisli-raised mb-3 ml-12 rounded-lg px-3 py-2 text-xs", !expanded && "mb-3")}>
      <p className={cn("font-medium", resultTone)}>{result.sent ? t("hookTestStatus", { status: result.status }) : result.error}</p>
      {result.response && <pre className="whisli-muted mt-1 max-h-32 overflow-auto whitespace-pre-wrap break-all font-mono text-[11px]">{result.response}</pre>}
    </div>}

    {expanded && <div className="mb-5 ml-12 grid gap-4">
      <div className="grid gap-2 sm:grid-cols-[8rem_minmax(0,1fr)]">
        <label className="grid gap-1 text-xs">
          <span className="whisli-muted">{t("hookMethod")}</span>
          <select className="whisli-select" value={draft.method} onChange={event => change({ method: event.currentTarget.value as HttpHook["method"] }, true)}>
            {methods.map(method => <option key={method} value={method}>{method}</option>)}
          </select>
        </label>
        <label className="grid gap-1 text-xs">
          <span className="whisli-muted">{t("hookUrl")}</span>
          <input className="whisli-input font-mono text-xs" value={draft.url} maxLength={limits.url} placeholder="https://example.com/webhook/{gesture}" spellCheck={false}
            onChange={event => change({ url: event.currentTarget.value })} onBlur={commit} />
        </label>
      </div>

      <label className="grid gap-1 text-xs">
        <span className="whisli-muted">{t("hookHeaders")}</span>
        <textarea className="whisli-input min-h-16 resize-y font-mono text-xs leading-5" rows={2} value={draft.headers} maxLength={limits.headers} spellCheck={false}
          placeholder={"Authorization: Bearer …"} onChange={event => change({ headers: event.currentTarget.value })} onBlur={commit} />
        <span className="whisli-muted">{t("hookHeadersHint")}</span>
      </label>

      <div className="grid gap-1 text-xs">
        <span className="whisli-muted">{t("hookBody")}</span>
        <div className="flex flex-wrap gap-1.5" role="group" aria-label={t("hookBody")}>
          {bodyKinds.map(kind => <button key={kind.value} type="button" aria-pressed={draft.body === kind.value} disabled={draft.method === "GET"}
            className={cn("whisli-line rounded-full border px-3 py-1 transition-colors disabled:opacity-40", draft.body === kind.value ? "border-[var(--ink)] bg-[var(--ink)] text-[var(--page)]" : "hover:border-[var(--ink)]")}
            onClick={() => change({ body: kind.value }, true)}>{t(kind.label)}</button>)}
        </div>
        {draft.method === "GET" && <span className="whisli-muted">{t("hookBodyGet")}</span>}
      </div>

      {hasBody && <div className="grid gap-3 lg:grid-cols-2">
        <div className="grid gap-1 text-xs">
          <span className="whisli-muted">{t("hookBodyTemplate")}</span>
          <textarea ref={bodyRef} className="whisli-input min-h-40 resize-y font-mono text-xs leading-5" rows={8} value={draft.bodyTemplate} maxLength={limits.body} spellCheck={false}
            onChange={event => change({ bodyTemplate: event.currentTarget.value })} onBlur={commit} />
          <div className="flex flex-wrap gap-1">
            {variables.map(name => <button key={name} type="button" className="whisli-line whisli-muted rounded border px-1.5 py-0.5 font-mono text-[11px] hover:border-[var(--ink)] hover:text-[var(--ink)]"
              title={t("hookInsertVariable")} onMouseDown={event => event.preventDefault()} onClick={() => insertVariable(name)}>{`{${name}}`}</button>)}
          </div>
        </div>
        <div className="grid content-start gap-1 text-xs">
          <span className="whisli-muted">{t("hookPreview")}</span>
          <pre className="whisli-raised min-h-40 overflow-auto whitespace-pre-wrap break-all rounded-md p-3 font-mono text-xs leading-5">{preview}</pre>
        </div>
      </div>}

      <label className="inline-flex items-center gap-2 text-xs">
        <Checkbox checked={draft.insecure} onCheckedChange={checked => change({ insecure: checked === true }, true)} />
        <span>{t("hookInsecure")}</span>
        <span className="whisli-muted">{t("hookInsecureHint")}</span>
      </label>
    </div>}
  </div>
}
