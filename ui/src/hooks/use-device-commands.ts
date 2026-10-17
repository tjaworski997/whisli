import { useState } from "react"

import { useI18n } from "@/i18n"
import { post, type ApiParams } from "@/lib/api"
import type { Device } from "@/types"

/** Settings and output commands for a single device page. Every call reports the firmware's message and refreshes the device list. */
export function useDeviceCommands(device: Device, refresh: () => unknown) {
  const { t } = useI18n()
  const [message, setMessage] = useState("")
  const [saving, setSaving] = useState(false)
  const [busyChannel, setBusyChannel] = useState<number | null>(null)
  const [busyAll, setBusyAll] = useState(false)

  async function send(path: string, params: ApiParams, refreshDelayMs = 0) {
    const result = await post(path, { id: device.id, ...params })
    setMessage(result.message || (result.ok ? "" : t("operationFailed")))
    // Outputs report their new state with a delay (Modbus round trip), so refresh a bit later.
    if (refreshDelayMs) window.setTimeout(refresh, refreshDelayMs)
    else if (result.ok) await refresh()
    return result.ok
  }

  async function save(path: string, params: ApiParams) {
    setSaving(true)
    try { return await send(path, params) } finally { setSaving(false) }
  }

  return {
    message,
    setMessage,
    saving,
    busyChannel,
    busyAll,
    saveSettings: (settings: ApiParams) => save("/api/v1/devices/settings", settings),
    setActive: (active: boolean) => save("/api/v1/devices/active", { active }),
    renameChannel: (index: number, name: string) => save("/api/v1/devices/channel/name", { channel: index + 1, name }),
    setChannelUsed: (index: number, used: boolean) => save("/api/v1/devices/channel/usage", { channel: index + 1, used }),
    renameInput: (index: number, name: string) => save("/api/v1/devices/input/name", { channel: index + 1, name }),
    setInputUsed: (index: number, used: boolean) => save("/api/v1/devices/input/usage", { channel: index + 1, used }),
    async toggleChannel(index: number) {
      setBusyChannel(index)
      try { await send("/api/v1/devices/channel", { channel: index + 1, state: device.channels[index] ? "off" : "on" }, 350) } finally { setBusyChannel(null) }
    },
    async setAllChannels(on: boolean) {
      setBusyAll(true)
      try { await send("/api/v1/devices/channels/all", { state: on ? "on" : "off" }, 1500) } finally { setBusyAll(false) }
    },
  }
}
