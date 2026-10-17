import { createContext, useContext, useEffect, useMemo, useState, type ReactNode } from "react"

import en from "./en.json"
import pl from "./pl.json"

const dictionaries = { pl, en }
export type Locale = keyof typeof dictionaries
export type TranslationKey = keyof typeof pl
export type TranslationParams = Record<string, string | number>

const labels: Record<Locale, string> = { pl: "Polski", en: "English" }
const storageKey = "whisli.locale"

type I18n = { locale: Locale; setLocale: (locale: Locale) => void; t: (key: TranslationKey, params?: TranslationParams) => string; labels: typeof labels }
const I18nContext = createContext<I18n | null>(null)

function initialLocale(): Locale {
  const saved = localStorage.getItem(storageKey)
  if (saved === "pl" || saved === "en") return saved
  return navigator.language.toLowerCase().startsWith("pl") ? "pl" : "en"
}

/** Looks up `key` (falling back to the key itself, e.g. for unknown keys sent by firmware) and fills `{name}` placeholders. */
function translate(locale: Locale, key: TranslationKey, params?: TranslationParams) {
  const text: string = dictionaries[locale][key] ?? key
  if (!params) return text
  return text.replace(/\{(\w+)\}/g, (match, name: string) => name in params ? String(params[name]) : match)
}

export function I18nProvider({ children }: { children: ReactNode }) {
  const [locale, setLocale] = useState<Locale>(initialLocale)
  useEffect(() => {
    localStorage.setItem(storageKey, locale)
    document.documentElement.lang = locale
  }, [locale])
  const value = useMemo(() => ({ locale, setLocale, t: (key: TranslationKey, params?: TranslationParams) => translate(locale, key, params), labels }), [locale])
  return <I18nContext.Provider value={value}>{children}</I18nContext.Provider>
}

export function useI18n() {
  const value = useContext(I18nContext)
  if (!value) throw new Error("useI18n must be used inside I18nProvider")
  return value
}
