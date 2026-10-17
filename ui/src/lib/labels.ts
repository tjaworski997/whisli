import type { TranslationKey } from "@/i18n"

export const modbusBus = "RS-485 · Modbus RTU"

export const triggerLabels: Record<string, TranslationKey> = {
  none: "inputEventNone",
  click: "inputEventClick",
  doubleClick: "inputEventDoubleClick",
  tripleClick: "inputEventTripleClick",
  longClick: "inputEventLongClick",
  hold: "inputEventHold",
}

export const triggerDescriptions: Record<string, TranslationKey> = {
  click: "inputEventClickDescription",
  doubleClick: "inputEventDoubleClickDescription",
  tripleClick: "inputEventTripleClickDescription",
  longClick: "inputEventLongClickDescription",
  hold: "inputEventHoldDescription",
}

export const commandLabels: Record<string, TranslationKey> = {
  toggle: "actionToggle",
  on: "actionTurnOn",
  off: "actionTurnOff",
  setLevel: "actionSetLevel",
  stepUp: "actionStepUp",
  stepDown: "actionStepDown",
  preset: "actionPreset",
  run: "actionRun",
}

/** Commands grouped as the action editor offers them. Light ones need a DMX light; "preset" one with colour or white. */
export const switchCommands = ["toggle", "on", "off"]
export const lightCommands = ["setLevel", "stepUp", "stepDown", "preset"]
/** Commands with a percentage (`value`); "preset" keeps a preset id there instead. */
export const valueCommands = ["setLevel", "stepUp", "stepDown"]
export const needsDimmable = (command: string) => lightCommands.includes(command)
/** The one command for webhooks, and the only one they take. */
export const hookCommand = "run"
export const presetLightKinds = ["cct", "rgb", "rgbw"]

/** "Rozjaśnij o 10%", "Barwa / kolor: Ciepła": a command's label with its percentage or preset. */
export function commandText(t: (key: TranslationKey) => string, command: string, value: number, presetName?: string) {
  const label = t(commandLabels[command] ?? "actionToggle")
  if (command === "preset") return presetName ? `${label}: ${presetName}` : label
  return valueCommands.includes(command) ? `${label} ${value}%` : label
}

export const logLevelLabels: Record<string, TranslationKey> = {
  info: "logInfo",
  warning: "logWarning",
  error: "logError",
}

export const logCategoryLabels: Record<string, TranslationKey> = {
  system: "logCategorySystem",
  network: "logCategoryNetwork",
  devices: "logCategoryDevices",
  relays: "logCategoryRelays",
  inputs: "logCategoryInputs",
  actions: "logCategoryActions",
  firmware: "logCategoryFirmware",
}
