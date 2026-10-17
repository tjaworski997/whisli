import { useEffect, useRef, useState, type FormEvent } from "react"

import { Field } from "@/components/page"
import { Button } from "@/components/ui/button"
import { Checkbox } from "@/components/ui/checkbox"
import { useI18n, type Locale } from "@/i18n"
import { post } from "@/lib/api"
import { signedIn } from "@/lib/auth"

export function LoginPage() {
  const { t, locale, setLocale, labels } = useI18n()
  const [password, setPassword] = useState("")
  const [remember, setRemember] = useState(false)
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState("")
  const input = useRef<HTMLInputElement>(null)
  // The field is disabled while checking; give it the cursor back for the next try.
  useEffect(() => { if (!busy) input.current?.focus() }, [busy])

  async function submit(event: FormEvent) {
    event.preventDefault()
    setBusy(true)
    setError("")
    const result = await post("/api/v1/auth/login", { password, remember })
    setBusy(false)
    if (result.ok) { signedIn(); return }
    setPassword("")
    setError(t(result.status === 401 ? "loginWrongPassword" : "controllerUnreachable"))
  }

  return <div className="whisli-page relative flex min-h-screen items-center justify-center px-4 text-sm">
    <select className="whisli-select absolute top-3 right-4 h-8 text-xs md:right-6" aria-label={t("language")} value={locale} onChange={event => setLocale(event.target.value as Locale)}>
      {Object.entries(labels).map(([value, label]) => <option key={value} value={value}>{label}</option>)}
    </select>
    <form onSubmit={event => void submit(event)} className="w-full max-w-sm">
      <p className="text-xl font-semibold">{t("appName")}</p>
      <p className="whisli-muted mt-1">{t("loginDescription")}</p>
      {/* Lets password managers file the password under one account. */}
      <input type="text" name="username" autoComplete="username" value="whisli" readOnly hidden />
      <Field label={t("password")} className="mt-6">
        <input ref={input} className="whisli-input h-10" type="password" name="password" autoComplete="current-password" required
          value={password} disabled={busy} onChange={event => setPassword(event.currentTarget.value)} />
      </Field>
      <label className="mt-4 flex items-center gap-2">
        <Checkbox checked={remember} disabled={busy} onCheckedChange={next => setRemember(next === true)} />{t("loginRemember")}
      </label>
      {error && <p role="alert" className="whisli-status-error mt-4">{error}</p>}
      <Button type="submit" className="mt-6 w-full" disabled={busy || !password}>{t(busy ? "loginBusy" : "loginButton")}</Button>
    </form>
  </div>
}
