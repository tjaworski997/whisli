import { Cpu } from "lucide-react"

import { AllOutputsButtons, ChannelTable, OutputToggle } from "@/components/channel-table"
import { AutoSaveInput, Field, Muted, PageHeader, SaveStatus, Section, Status } from "@/components/page"
import { useDeviceCommands } from "@/hooks/use-device-commands"
import { useI18n } from "@/i18n"
import { channelLabel } from "@/lib/format"
import { triggerLabels } from "@/lib/labels"
import type { ControllerDevice } from "@/types"

export function ControllerPage({ device, onBack, onRefresh }: { device: ControllerDevice; onBack: () => void; onRefresh: () => unknown }) {
  const { t } = useI18n()
  const commands = useDeviceCommands(device, onRefresh)

  return <>
    <PageHeader
      back={{ label: t("devices"), onClick: onBack }}
      icon={Cpu}
      title={device.name}
      description={t("controllerBoard")}
      actions={<><Status online={device.online} /><span className="whisli-muted text-xs">{t("builtInDevice")}</span></>}
    />

    <Section bordered className="mt-6" title={t("basicSettings")} description={t("autoSaveHint")}>
      <Field label={t("deviceName")} className="mt-3 max-w-md"><AutoSaveInput value={device.name} onSave={name => { if (name) void commands.saveSettings({ name }) }} /></Field>
      <SaveStatus saving={commands.saving} message={commands.message} />
    </Section>

    <Section title={t("digitalInputsTitle")} description={t("digitalInputsDescription")}>
      <ChannelTable
        prefix="DI"
        stateLabel={t("inputLevel")}
        disabled={commands.saving}
        onRename={commands.renameInput}
        onUsedChange={commands.setInputUsed}
        rows={device.inputsHigh.map((high, index) => ({
          name: device.inputNames[index] ?? "",
          used: device.inputUsed[index] ?? false,
          hint: `${t("lastEvent")}: ${t(triggerLabels[device.inputLastEvents[index]] ?? "inputEventNone")}`,
          state: <span className="whisli-muted font-mono text-xs">{high ? t("inputHigh") : t("inputLow")}</span>,
        }))}
      />
    </Section>

    <Section title={t("digitalOutputsTitle")} description={t("digitalOutputsDescription")} actions={<AllOutputsButtons busy={commands.busyAll} disabled={!device.online} onSetAll={commands.setAllChannels} />}>
      <Muted className="mt-2 text-xs">{t("relayUsedDescription")}</Muted>
      <ChannelTable
        prefix="DO"
        stateLabel={t("state")}
        disabled={commands.saving}
        onRename={commands.renameChannel}
        onUsedChange={commands.setChannelUsed}
        rows={device.channels.map((on, index) => ({
          name: device.channelNames[index] ?? "",
          used: device.channelUsed[index] ?? false,
          state: <OutputToggle on={on} busy={commands.busyChannel === index} disabled={!device.online} label={`${t("testOutput")} ${channelLabel("DO", index)}`} onClick={() => void commands.toggleChannel(index)} />,
        }))}
      />
    </Section>
  </>
}
