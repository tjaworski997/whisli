import { Slider } from "@/components/ui/slider"
import { useI18n } from "@/i18n"

// Typical tunable-white LED strip: warm channel 2700 K, cool channel 6500 K.
const warmKelvin = 2700
const coolKelvin = 6500
const kelvinStep = 50

// The firmware mixes two channels by percent (0 = warm only, 100 = cool only).
// Mixing two whites is close to linear in mireds (10^6 / K), not in kelvin.
const mired = (kelvin: number) => 1e6 / kelvin
export function mixToKelvin(percent: number) {
  const value = mired(warmKelvin) + (mired(coolKelvin) - mired(warmKelvin)) * percent / 100
  return Math.round(1e6 / value / kelvinStep) * kelvinStep
}
export function kelvinToMix(kelvin: number) {
  return Math.round((mired(warmKelvin) - mired(kelvin)) / (mired(warmKelvin) - mired(coolKelvin)) * 100)
}

/** Approximate sRGB of a black body at the given temperature (Tanner Helland's fit). */
function kelvinColor(kelvin: number) {
  const t = kelvin / 100
  const clamp = (value: number) => Math.round(Math.min(255, Math.max(0, value)))
  const red = t <= 66 ? 255 : 329.7 * (t - 60) ** -0.1332
  const green = t <= 66 ? 99.47 * Math.log(t) - 161.12 : 288.12 * (t - 60) ** -0.0755
  const blue = t >= 66 ? 255 : t <= 19 ? 0 : 138.52 * Math.log(t - 10) - 305.04
  return `rgb(${clamp(red)} ${clamp(green)} ${clamp(blue)})`
}

/** Indicative swatch: the range is stretched (2000–9000 K) so the cool end reads as bluish rather than page-white. */
export const kelvinSwatch = (kelvin: number) => kelvinColor(2000 + (kelvin - warmKelvin) * 7000 / (coolKelvin - warmKelvin))

export function ColorTemperaturePicker({ value, disabled, className, onChange, onCommit }: {
  /** Mix in percent, as stored by the firmware. */
  value: number
  disabled?: boolean
  className?: string
  onChange: (mix: number) => void
  onCommit: (mix: number) => void
}) {
  const { t } = useI18n()
  const kelvin = mixToKelvin(value)
  return <Slider
    className={className}
    value={[kelvin]} min={warmKelvin} max={coolKelvin} step={kelvinStep} disabled={disabled}
    label={t("lightColorTemperature")}
    valueLabel={<><span className="whisli-line size-3 rounded-[3px] border" style={{ backgroundColor: kelvinSwatch(kelvin) }} aria-hidden />{kelvin} K</>}
    onValueChange={next => onChange(kelvinToMix(next[0] ?? kelvin))}
    onValueCommitted={next => onCommit(kelvinToMix(next[0] ?? kelvin))}
  />
}
