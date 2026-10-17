import { Radio, Trash2 } from "lucide-react"

import { AllOutputsButtons, ChannelTable, OutputToggle } from "@/components/channel-table"
import { AutoSaveInput, Field, Muted, PageHeader, SaveStatus, Section, Status } from "@/components/page"
import { Button } from "@/components/ui/button"
import { useDeviceCommands } from "@/hooks/use-device-commands"
import { useI18n, type TranslationKey } from "@/i18n"
import { channelLabel } from "@/lib/format"
import { modbusBus } from "@/lib/labels"
import type { Relay } from "@/types"

type RelaySettings = Pick<Relay, "name" | "address" | "channelCount" | "coilStart">

const inRange = (value: number, min: number, max: number) => Number.isInteger(value) && value >= min && value <= max
const isValid = (settings: RelaySettings) => settings.name !== "" && inRange(settings.address, 1, 247) && inRange(settings.channelCount, 1, 32) && inRange(settings.coilStart, 0, 65503)

export function RelayPage({ relay, onBack, onRefresh, onRemove }: { relay: Relay; onBack: () => void; onRefresh: () => unknown; onRemove: () => void }) {
  const { t } = useI18n()
  const commands = useDeviceCommands(relay, onRefresh)

  function saveSetting(field: keyof RelaySettings, raw: string) {
    const settings: RelaySettings = { name: relay.name, address: relay.address, channelCount: relay.channelCount, coilStart: relay.coilStart, [field]: field === "name" ? raw : Number(raw) }
    if (!isValid(settings)) { commands.setMessage(t("relaySettingsInvalid")); return }
    void commands.saveSettings(settings)
  }

  return <>
    <PageHeader
      back={{ label: t("devices"), onClick: onBack }}
      icon={Radio}
      title={relay.name}
      description={t(relay.profileNameKey as TranslationKey)}
      actions={<>
        <Status online={relay.online} />
        <Button variant="outline" disabled={commands.saving} onClick={() => void commands.setActive(!relay.active)}>{relay.active ? t("deactivate") : t("activate")}</Button>
        <Button variant="danger" onClick={onRemove}><Trash2 className="size-4" />{t("remove")}</Button>
      </>}
    />

    <Section bordered className="mt-6" title={t("basicSettings")} description={t("autoSaveHint")}>
      <div className="mt-3 grid max-w-3xl gap-3 sm:grid-cols-[minmax(220px,1fr)_150px_150px]">
        <Field label={t("deviceName")}><AutoSaveInput value={relay.name} onSave={value => saveSetting("name", value)} /></Field>
        <Field label={t("modbusId")}><AutoSaveInput type="number" min={1} max={247} value={relay.address} onSave={value => saveSetting("address", value)} /></Field>
        <Field label={t("relayCount")}><AutoSaveInput type="number" min={1} max={32} value={relay.channelCount} onSave={value => saveSetting("channelCount", value)} /></Field>
      </div>
      <details className="whisli-line mt-4 max-w-3xl border-t pt-3">
        <summary className="cursor-pointer font-medium">{t("advancedSettings")}</summary>
        <Muted className="mt-2 text-xs">{t("advancedSettingsDescription")}</Muted>
        <div className="mt-3 grid gap-3 sm:grid-cols-[150px_1fr]">
          <Field label={t("coilStart")}><AutoSaveInput type="number" min={0} max={65503} value={relay.coilStart} onSave={value => saveSetting("coilStart", value)} /></Field>
          <Muted className="self-end pb-2 text-xs">{t("relayBusInfo")}</Muted>
        </div>
      </details>
      <SaveStatus saving={commands.saving} message={commands.message}>{modbusBus} · 9600 8N1 · Slave {relay.address} · {t(relay.active ? "deviceActive" : "deviceInactive")}</SaveStatus>
    </Section>

    <Section className="mt-6" title={t("relayChannelsTitle")} description={t("relayChannelsDescription")} actions={<AllOutputsButtons busy={commands.busyAll} disabled={!relay.online} onSetAll={commands.setAllChannels} />}>
      <Muted className="mt-2 text-xs">{t("relayUsedDescription")}</Muted>
      <ChannelTable
        prefix="CH"
        stateLabel={t("state")}
        disabled={commands.saving}
        onRename={commands.renameChannel}
        onUsedChange={commands.setChannelUsed}
        rows={relay.channels.map((on, index) => ({
          name: relay.channelNames[index] ?? "",
          used: relay.channelUsed[index] ?? false,
          state: <OutputToggle on={on} busy={commands.busyChannel === index} disabled={!relay.online} label={`${t("testOutput")} ${channelLabel("CH", index)}`} onClick={() => void commands.toggleChannel(index)} />,
        }))}
      />
    </Section>
  </>
}
