import { useState } from "react"

import { BehaviorFields, groupParams, type BehaviorGroup } from "@/components/light-behavior"
import { ErrorText, Muted, PageHeader } from "@/components/page"
import { useLiveRefresh } from "@/hooks/use-live-refresh"
import { useI18n } from "@/i18n"
import { getJson, post } from "@/lib/api"
import type { LightBehavior, LightsResponse } from "@/types"

/** Lights › Default settings: how every light behaves unless it overrides a setting. Saved per change. */
export function LightSettingsPage() {
  const { t } = useI18n()
  const [defaults, setDefaults] = useState<LightBehavior | null>(null)
  const [error, setError] = useState("")

  async function refresh() {
    try { setDefaults((await getJson<LightsResponse>("/api/v1/lights")).defaults); setError("") }
    catch { setError(t("lightsLoadError")) }
  }
  useLiveRefresh(refresh, ["lights"])

  async function change(group: BehaviorGroup, next: LightBehavior) {
    setDefaults(next)
    const result = await post("/api/v1/lights/defaults", groupParams(group, next))
    setError(result.ok ? "" : result.message || t("lightSaveError"))
    void refresh()
  }

  return <>
    <PageHeader eyebrow={t("lights")} title={t("lightDefaults")} description={t("lightDefaultsDescription")} />
    {error && <ErrorText>{error}</ErrorText>}
    {!defaults ? <Muted className="whisli-line mt-5 border-t py-4">{t("connecting")}</Muted>
      : <div className="whisli-line mt-5 max-w-4xl border-y"><BehaviorFields defaults={defaults} onChange={(group, next) => void change(group, next)} /></div>}
  </>
}
