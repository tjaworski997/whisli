import { useEffect, useState } from "react"

export type Theme = "light" | "dark"
const storageKey = "whisli.theme"
const themeColors: Record<Theme, string> = { light: "#fafafa", dark: "#09090b" }

export function useTheme() {
  // The initial theme (saved choice or system preference) is applied by an inline script in index.html before first paint.
  const [theme, setTheme] = useState<Theme>(() => document.documentElement.dataset.theme === "dark" ? "dark" : "light")
  useEffect(() => {
    document.documentElement.dataset.theme = theme
    document.querySelector('meta[name="theme-color"]')?.setAttribute("content", themeColors[theme])
  }, [theme])

  // Only an explicit choice is remembered; until then the panel follows the system setting.
  function toggleTheme() {
    const next: Theme = theme === "light" ? "dark" : "light"
    try { localStorage.setItem(storageKey, next) } catch { /* private mode: the choice just isn't remembered */ }
    setTheme(next)
  }
  return { theme, toggleTheme }
}
