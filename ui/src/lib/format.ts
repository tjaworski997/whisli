const pad = (value: number) => String(value).padStart(2, "0")

export function formatUptime(seconds: number) {
  const days = Math.floor(seconds / 86400)
  return `${days}d ${pad(Math.floor((seconds % 86400) / 3600))}h ${pad(Math.floor((seconds % 3600) / 60))}m ${pad(seconds % 60)}s`
}

export function formatLogAge(uptimeMs: number) {
  const seconds = Math.floor(uptimeMs / 1000)
  return `+${pad(Math.floor(seconds / 3600))}:${pad(Math.floor((seconds % 3600) / 60))}:${pad(seconds % 60)}`
}

/** Hardware channel label, e.g. `CH01`, `DI08`. */
export function channelLabel(prefix: string, index: number) {
  return `${prefix}${pad(index + 1)}`
}

export function fileSizeKb(file: File) {
  return (file.size / 1024).toFixed(1)
}
