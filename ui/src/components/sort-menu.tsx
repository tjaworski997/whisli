import { ArrowUpDown } from "lucide-react"

import { IconMenu, IconMenuChoice } from "@/components/ui/icon-menu"
import { useI18n, type TranslationKey } from "@/i18n"

/** Sort choice behind an icon; the tooltip says what is chosen. */
export function SortMenu<T extends string>({ value, options, onChange }: { value: T; options: { value: T; label: TranslationKey }[]; onChange: (value: T) => void }) {
  const { t } = useI18n()
  const current = options.find(option => option.value === value)
  return <IconMenu icon={ArrowUpDown} label={`${t("sortBy")}: ${current ? t(current.label) : ""}`}>
    <IconMenuChoice title={t("sortBy")} value={value} options={options.map(option => ({ value: option.value, label: t(option.label) }))} onChange={onChange} />
  </IconMenu>
}
