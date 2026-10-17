export type Relay = {
  kind: "relay"
  id: string
  name: string
  profile: string
  profileNameKey: string
  address: number
  channelCount: number
  coilStart: number
  active: boolean
  online: boolean
  channels: boolean[]
  channelUsed: boolean[]
  channelNames: string[]
}

export type ControllerDevice = {
  kind: "controller"
  id: "controller"
  name: string
  profile: string
  profileNameKey: string
  builtIn: true
  active: true
  online: boolean
  channelCount: number
  channels: boolean[]
  channelUsed: boolean[]
  channelNames: string[]
  inputsHigh: boolean[]
  inputUsed: boolean[]
  inputNames: string[]
  inputLastEvents: string[]
}

export type Device = Relay | ControllerDevice

export type SystemInfo = {
  firmware: { version: string; build: string }
  uptimeSeconds: number
  hardware: {
    board: string
    model: string
    revision: number
    cpuMhz: number
    cores: number
    flashMb: number
    psramMb: number
    freeHeapKb: number
    wifiMac: string
  }
  network: { ethernetIp: string; wifiConnected: boolean; wifiIp: string; ssid: string }
  storage: { firmwareKb: number; otaAvailableKb: number; nvsUsedEntries?: number; nvsFreeEntries?: number; filesUsedKb?: number; filesTotalKb?: number }
}

export type DeviceLogEntry = { id: number; uptimeMs: number; level: "info" | "warning" | "error"; category: string; message: string }
export type DeviceLogResponse = { sessionId: number; capacity: number; retentionSeconds: number; hasMore: boolean; entries: DeviceLogEntry[] }

/** `value`: percent for setLevel, stepUp and stepDown. */
export type PanelAction = { id: number; inputId: string; trigger: string; command: string; targetId: string; enabled: boolean; value: number }
export type PanelEvent = { id: number; inputId: string; trigger: string }
export type PanelInput = { id: string; name: string; hardwareName: string; high: boolean; lastEvent: string }
/** `dimmable`: lights on DMX, which take brightness commands. */
export type PanelTarget = { id: string; group: "light" | "relay" | "hook"; name: string; state: boolean; online: boolean; dimmable?: boolean; kind?: Light["kind"] }
export type ActionPanelData = { inputs: PanelInput[]; targets: PanelTarget[]; events: PanelEvent[]; actions: PanelAction[]; triggers: string[]; commands: string[] }

export type DeviceClassDefinition = { id: string; nameKey: string; descriptionKey: string; icon: string; available: boolean }
export type DeviceProfileDefinition = {
  id: string
  deviceClass: string
  nameKey: string
  descriptionKey: string
  defaultNameKey: string
  communication: string
  searchKeywords: string
  defaultChannelCount: number
  defaultCoilStart: number
  minimumChannelCount: number
  maximumChannelCount: number
  editableChannelCount: boolean
  editableCoilStart: boolean
  available: boolean
}
export type DeviceCatalogData = { classes: DeviceClassDefinition[]; profiles: DeviceProfileDefinition[] }

export type Light = {
  id: number
  name: string
  kind: "none" | "relay" | "dmx" | "cct" | "rgb" | "rgbw"
  type: "none" | "switch" | "dimmable" | "cct" | "rgb" | "rgbw"
  level: number
  relayTarget: string
  dmxChannel: number
  colorTemperature: number
  /** Only for RGB/RGBW lights: the colour at full brightness (white is always 0 for RGB). */
  color?: RgbwColor
  /** Only for DMX lights: how the level maps to DMX output. */
  dimming?: Dimming
  /** This light's own behaviour; missing keys follow the defaults. */
  behavior?: Partial<LightBehavior>
}

/** How lights behave beyond their level. Global defaults, which each light may override per group. */
export type LightBehavior = {
  fadeIn: boolean
  fadeInMs: number
  fadeOut: boolean
  fadeOutMs: number
  /** "On" returns to the last brightness, or to a fixed percentage. */
  onLevel: "last" | number
  maxLevel: number
  /** After a controller restart: as before it (true) or off. */
  restore: boolean
}
export type LightsResponse = { lights: Light[]; defaults: LightBehavior }

export type DimCurve = "linear" | "perceptual" | "square"
/** `minimum` is the output (0–50 %) of the lowest non-zero level. */
export type Dimming = { minimum: number; curve: DimCurve }

export type RgbwColor = { red: number; green: number; blue: number; white: number }

/** A named part of the house, e.g. "Parter – salon". Flat: no floors or nesting. */
export type Zone = { id: number; name: string }
/** Zones in display order, and which zone each thing (by its API id: "light-3", "controller.input-2") belongs to. */
export type ZonesResponse = { zones: Zone[]; assignments: Record<string, number> }

/** A named white (CCT lights) or colour (RGB/RGBW lights). Applying one never changes brightness. */
export type LightPreset = { id: number; name: string; kind: "temperature" | "color"; kelvin: number; color: RgbwColor }

/** Network state and settings (/api/v1/network). Passwords never come back, only whether one is set. */
export type NetworkInfo = {
  ethernet: { connected: boolean; link: boolean; ip: string; gateway: string; mac: string }
  wifi: { connected: boolean; ssid: string; savedSsid: string; ip: string; rssi: number; mac: string }
  accessPoint: { enabled: boolean; active: boolean; fallback: boolean; ssid: string; ip: string; customPassword: boolean }
  mqtt: { enabled: boolean; host: string; port: number; user: string; hasPassword: boolean; baseTopic: string; discovery: boolean; discoveryPrefix: string
    /** Live connection state of the client, with the reason when it failed. */
    status: "disabled" | "connecting" | "connected" | "error"; error: string }
}
export type WifiNetwork = { ssid: string; rssi: number; open: boolean }

/** Chaos mode: random lights and outputs until stopped (/api/v1/test). */
export type TestStatus = { chaos: { running: boolean; restoring: boolean; elapsedSeconds: number; switches: number } }

/** An HTTP request a button action can send ("hook-<id>" as the action target). */
export type HttpHook = {
  id: number
  name: string
  method: "GET" | "POST" | "PUT" | "DELETE"
  url: string
  /** "Name: value" per line. */
  headers: string
  body: "none" | "json" | "form" | "text"
  bodyTemplate: string
  /** https:// without checking the certificate. */
  insecure: boolean
}
export type HttpHookResult = { sent: boolean; status: number; error: string; response: string }
