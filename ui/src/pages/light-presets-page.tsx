import { Plus, SlidersHorizontal, Trash2 } from "lucide-react"
import { useEffect, useState } from "react"

import { ColorTemperaturePicker, kelvinToMix, mixToKelvin } from "@/components/color-temperature-picker"
import { AutoSaveInput, ErrorText, Muted, PageHeader, Section } from "@/components/page"
import { RgbwPicker } from "@/components/rgbw-picker"
import { Button } from "@/components/ui/button"
import { MessageBox } from "@/components/ui/message-box"
import { usePresets } from "@/hooks/use-presets"
import { useI18n, type TranslationKey } from "@/i18n"
import { getJson, post, type ApiParams } from "@/lib/api"
import type { ActionPanelData, LightPreset, RgbwColor } from "@/types"

/** Lights › Presets: named whites and colours to set on lights from their dialog or from actions. */
export function LightPresetsPage() {
  const { t } = useI18n()
  const { presets, refresh } = usePresets()
  const [error, setError] = useState("")
  const [busy, setBusy] = useState(false)
  const [addedId, setAddedId] = useState<number | null>(null)
  const [removing, setRemoving] = useState<LightPreset | null>(null)
  // How many actions use each preset, to say what removing one affects.
  const [usage, setUsage] = useState<Record<number, number>>({})
  useEffect(() => {
    getJson<ActionPanelData>("/api/v1/action-panel").then(data => {
      const counts: Record<number, number> = {}
      for (const action of data.actions) if (action.command === "preset" && action.value) counts[action.value] = (counts[action.value] ?? 0) + 1
      setUsage(counts)
    }, () => undefined)
  }, [presets])

  async function run(path: string, params: ApiParams) {
    setBusy(true)
    const result = await post(path, params)
    setBusy(false)
    setError(result.ok ? "" : result.message || t("presetSaveError"))
    await refresh()
    return result
  }

  // Like a new light or zone: a record with a default name, selected to type over.
  async function add(kind: LightPreset["kind"]) {
    const base = t(kind === "temperature" ? "presetDefaultWhite" : "presetDefaultColor")
    const taken = new Set(presets.map(preset => preset.name.toLocaleLowerCase()))
    let name = base
    for (let number = 2; taken.has(name.toLocaleLowerCase()); number += 1) name = `${base} ${number}`
    const result = await run("/api/v1/presets", { kind, name })
    setAddedId(Number(result.data.id) || null)
  }

  const section = (kind: LightPreset["kind"], title: TranslationKey, description: TranslationKey, addLabel: TranslationKey) => {
    const items = presets.filter(preset => preset.kind === kind)
    return <Section bordered className={kind === "temperature" ? "mt-6 py-6" : "mt-0 border-t-0 py-6"} title={t(title)} description={t(description)}
      actions={<Button variant="outline" disabled={busy} onClick={() => void add(kind)}><Plus className="size-4" />{t(addLabel)}</Button>}>
      {items.length === 0 ? <Muted className="mt-4 text-sm">{t("presetsEmpty")}</Muted>
        : <div className="mt-4 divide-y divide-[var(--line)]">{items.map(preset => <PresetRow key={preset.id} preset={preset} busy={busy} focused={preset.id === addedId}
          onFocused={() => setAddedId(null)} onSave={params => void run("/api/v1/presets/update", { id: preset.id, ...params })} onRemove={() => setRemoving(preset)} />)}</div>}
    </Section>
  }

  return <>
    <PageHeader eyebrow={t("lights")} title={t("presetsTitle")} description={t("presetsDescription")} />
    {error && <ErrorText>{error}</ErrorText>}
    <div className="max-w-4xl">
      {section("temperature", "presetsWhites", "presetsWhitesDescription", "presetAddWhite")}
      {section("color", "presetsColors", "presetsColorsDescription", "presetAddColor")}
    </div>
    <MessageBox
      open={removing !== null}
      type="danger"
      title={t("presetRemoveTitle")}
      message={removing && usage[removing.id] ? t("presetRemoveUsed", { count: usage[removing.id] }) : t("presetRemoveMessage")}
      detail={removing?.name}
      confirmLabel={t("remove")}
      cancelLabel={t("cancel")}
      busy={busy}
      onConfirm={() => { if (removing) void run("/api/v1/presets/remove", { id: removing.id }).then(() => setRemoving(null)) }}
      onClose={() => setRemoving(null)}
    />
  </>
}

function PresetRow({ preset, busy, focused, onFocused, onSave, onRemove }: {
  preset: LightPreset
  busy: boolean
  focused: boolean
  onFocused: () => void
  onSave: (params: ApiParams) => void
  onRemove: () => void
}) {
  const { t } = useI18n()
  // Edited locally while dragging, saved when let go.
  const [mix, setMix] = useState(kelvinToMix(preset.kelvin))
  const [color, setColor] = useState<RgbwColor>(preset.color)
  useEffect(() => setMix(kelvinToMix(preset.kelvin)), [preset.kelvin])
  useEffect(() => setColor(preset.color), [preset.color])

  return <div className="grid grid-cols-[auto_minmax(0,1fr)_auto] items-center gap-x-3 gap-y-2 py-3 sm:grid-cols-[auto_minmax(0,1fr)_minmax(14rem,1.2fr)_auto]">
    <SlidersHorizontal className="whisli-muted size-5 shrink-0" aria-hidden />
    <AutoSaveInput inline className="font-medium" value={preset.name} maxLength={24} aria-label={t("presetName")}
      autoFocus={focused} onFocus={event => { if (focused) { event.currentTarget.select(); onFocused() } }}
      onSave={name => { if (name) onSave({ name }) }} />
    <div className="col-span-3 row-start-2 sm:col-span-1 sm:row-start-auto">
      {preset.kind === "temperature"
        ? <ColorTemperaturePicker value={mix} disabled={busy} onChange={setMix} onCommit={next => onSave({ kelvin: mixToKelvin(next) })} />
        : <RgbwPicker value={color} hasWhite hideLabel disabled={busy} onChange={setColor} onCommit={next => onSave(next)} />}
    </div>
    <Button variant="dangerGhost" size="icon" className="col-start-3 row-start-1 sm:col-start-4" disabled={busy} aria-label={t("presetRemove")} title={t("presetRemove")} onClick={onRemove}><Trash2 className="size-4" /></Button>
  </div>
}
