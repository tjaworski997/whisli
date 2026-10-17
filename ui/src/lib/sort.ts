import { useRef, useState } from "react"

const collator = new Intl.Collator(undefined, { numeric: true, sensitivity: "base" })
/** Natural, locale-aware text order: "Lampa 2" before "Lampa 10", Polish letters in place. */
export const naturalCompare = (left: string, right: string) => collator.compare(left, right)

/** A sort choice remembered in this browser. Falls back when the stored value is not an option. */
export function useRememberedSort<T extends string>(key: string, options: readonly T[], fallback: T) {
  const [value, setValue] = useState<T>(() => {
    try { const saved = localStorage.getItem(key) as T | null; return saved && options.includes(saved) ? saved : fallback } catch { return fallback }
  })
  const choose = (next: T) => {
    setValue(next)
    try { localStorage.setItem(key, next) } catch { /* private mode: not remembered */ }
  }
  return [value, choose] as const
}

/**
 * Sorts `items`, but keeps the order steady while the page is open: it is worked out again only when
 * the sort mode changes. Renaming a row does not make it jump away under the cursor; things added
 * meanwhile go to the end until the next visit.
 */
export function useStableOrder<T>(items: T[], idOf: (item: T) => string | number, compare: (left: T, right: T) => number, mode: string) {
  const memory = useRef<{ mode: string; order: (string | number)[] }>({ mode: "", order: [] })
  const known = new Set(memory.current.order)
  if (memory.current.mode !== mode || memory.current.order.length === 0) {
    memory.current = { mode, order: [...items].sort(compare).map(idOf) }
  } else if (items.some(item => !known.has(idOf(item)))) {
    const fresh = items.filter(item => !known.has(idOf(item))).sort(compare).map(idOf)
    memory.current = { mode, order: [...memory.current.order, ...fresh] }
  }
  const position = new Map(memory.current.order.map((id, index) => [id, index]))
  return [...items].sort((left, right) => (position.get(idOf(left)) ?? 0) - (position.get(idOf(right)) ?? 0))
}

/** How buttons are ordered on the Actions page and the map (one remembered choice for both). */
export const buttonSorts = ["zone", "name", "input"] as const
export type ButtonSort = typeof buttonSorts[number]
export const buttonSortKey = "whisli-buttons-sort"
type SortableButton = { name: string; hardwareName: string }
export const compareButtons = (sort: ButtonSort) => (left: SortableButton, right: SortableButton) =>
  sort === "name" ? naturalCompare(left.name, right.name) || naturalCompare(left.hardwareName, right.hardwareName) : naturalCompare(left.hardwareName, right.hardwareName)

/** Gestures always in the controller's order (click, double, triple, long, hold), whatever was added first. */
export const gestureOrder = (triggers: string[]) => (left: { trigger: string }, right: { trigger: string }) => triggers.indexOf(left.trigger) - triggers.indexOf(right.trigger)
