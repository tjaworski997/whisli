import { ToggleLeft, Trash2 } from "lucide-react"

import { InputTable } from "@/components/channel-table"
import { AutoSaveInput, Field, Muted, PageHeader, SaveStatus, Section, Status } from "@/components/page"
import { Button } from "@/components/ui/button"
import { useDeviceCommands } from "@/hooks/use-device-commands"
import { useI18n, type TranslationKey } from "@/i18n"
import { modbusBus } from "@/lib/labels"
import type { InputModule } from "@/types"

type ModuleSettings = Pick<InputModule, "name" | "address" | "channelCount" | "coilStart">

const inRange = (value: number, min: number, max: number) => Number.isInteger(value) && value >= min && value <= max
const isValid = (settings: ModuleSettings) => settings.name !== "" && inRange(settings.address, 1, 247) && inRange(settings.channelCount, 1, 32) && inRange(settings.coilStart, 0, 65503)

/** A Modbus module with button inputs: settings, and per input its name, "used" flag and live state. */
export function InputModulePage({ module, onBack, onRefresh, onRemove }: { module: InputModule; onBack: () => void; onRefresh: () => unknown; onRemove: () => void }) {
  const { t } = useI18n()
  const commands = useDeviceCommands(module, onRefresh)

  function saveSetting(field: keyof ModuleSettings, raw: string) {
    const settings: ModuleSettings = { name: module.name, address: module.address, channelCount: module.channelCount, coilStart: module.coilStart, [field]: field === "name" ? raw : Number(raw) }
    if (!isValid(settings)) { commands.setMessage(t("inputModuleSettingsInvalid")); return }
    void commands.saveSettings(settings)
  }

  return <>
    <PageHeader
      back={{ label: t("devices"), onClick: onBack }}
      icon={ToggleLeft}
      title={module.name}
      description={t(module.profileNameKey as TranslationKey)}
      actions={<>
        <Status online={module.online} />
        <Button variant="outline" disabled={commands.saving} onClick={() => void commands.setActive(!module.active)}>{module.active ? t("deactivate") : t("activate")}</Button>
        <Button variant="danger" onClick={onRemove}><Trash2 className="size-4" />{t("remove")}</Button>
      </>}
    />

    <Section bordered className="mt-6" title={t("basicSettings")} description={t("autoSaveHint")}>
      <div className="mt-3 grid max-w-3xl gap-3 sm:grid-cols-[minmax(220px,1fr)_150px_150px]">
        <Field label={t("deviceName")}><AutoSaveInput value={module.name} onSave={value => saveSetting("name", value)} /></Field>
        <Field label={t("modbusId")}><AutoSaveInput type="number" min={1} max={247} value={module.address} onSave={value => saveSetting("address", value)} /></Field>
        <Field label={t("inputCount")}><AutoSaveInput type="number" min={1} max={32} value={module.channelCount} onSave={value => saveSetting("channelCount", value)} /></Field>
      </div>
      <details className="whisli-line mt-4 max-w-3xl border-t pt-3">
        <summary className="cursor-pointer font-medium">{t("advancedSettings")}</summary>
        <Muted className="mt-2 text-xs">{t("advancedSettingsDescription")}</Muted>
        <div className="mt-3 grid gap-3 sm:grid-cols-[150px_1fr]">
          <Field label={t("inputRegisterStart")}><AutoSaveInput type="number" min={0} max={65503} value={module.coilStart} onSave={value => saveSetting("coilStart", value)} /></Field>
          <Muted className="self-end pb-2 text-xs">{t("inputBusInfo")}</Muted>
        </div>
      </details>
      <SaveStatus saving={commands.saving} message={commands.message}>{modbusBus} · 9600 8N1 · Slave {module.address} · {t(module.active ? "deviceActive" : "deviceInactive")}</SaveStatus>
    </Section>

    <Section className="mt-6" title={t("moduleInputsTitle")} description={t("moduleInputsDescription")}>
      <InputTable
        prefix="X"
        firstNumber={0}
        disabled={commands.saving}
        onRename={commands.renameInput}
        onUsedChange={commands.setInputUsed}
        // PNP inputs: the terminal goes high while the button is pressed.
        inputs={module.inputPressed.map((pressed, index) => ({ name: module.inputNames[index] ?? "", used: module.inputUsed[index] ?? false, high: pressed, lastEvent: module.inputLastEvents[index] ?? "none" }))}
      />
    </Section>
  </>
}
