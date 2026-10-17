Import("env")

import json
from pathlib import Path


project_dir = Path(env["PROJECT_DIR"])
source = project_dir / "data" / "device_profiles.json"
output = project_dir / "include" / "whisli" / "app" / "device_profiles.generated.hpp"
catalog = json.loads(source.read_text(encoding="utf-8"))


def cpp_string(value: str) -> str:
    return json.dumps(value, ensure_ascii=False)


class_ids = [item["id"] for item in catalog["classes"]]
profile_ids = [item["id"] for item in catalog["profiles"]]
if len(class_ids) != len(set(class_ids)) or len(profile_ids) != len(set(profile_ids)):
    raise ValueError("Device class and profile IDs must be unique")
if any(item["deviceClass"] not in class_ids for item in catalog["profiles"]):
    raise ValueError("Every device profile must refer to a known device class")
translation_keys = {
    item[key]
    for item in catalog["classes"]
    for key in ("nameKey", "descriptionKey")
} | {
    item[key]
    for item in catalog["profiles"]
    for key in ("nameKey", "descriptionKey", "defaultNameKey")
}
for locale in ("pl", "en"):
    dictionary = json.loads((project_dir / "ui" / "src" / "i18n" / f"{locale}.json").read_text(encoding="utf-8"))
    missing = translation_keys - dictionary.keys()
    if missing:
        raise ValueError(f"Missing {locale} translation keys referenced by device profiles: {sorted(missing)}")

lines = [
    "// Generated from data/device_profiles.json. Do not edit manually.",
    "#pragma once",
    f"constexpr uint8_t kDeviceProfileSchemaVersion = {int(catalog['schemaVersion'])};",
    "constexpr DeviceClassDefinition kDeviceClasses[] = {",
]
for item in catalog["classes"]:
    lines.append(
        "  {"
        + ", ".join(
            [
                cpp_string(item["id"]),
                cpp_string(item["nameKey"]),
                cpp_string(item["descriptionKey"]),
                cpp_string(item["icon"]),
                "true" if item["available"] else "false",
            ]
        )
        + "},"
    )
lines.extend(["};", "constexpr DeviceProfileDefinition kDeviceProfiles[] = {"])
for item in catalog["profiles"]:
    lines.append(
        "  {"
        + ", ".join(
            [
                cpp_string(item["id"]),
                cpp_string(item["deviceClass"]),
                cpp_string(item["nameKey"]),
                cpp_string(item["descriptionKey"]),
                cpp_string(item["defaultNameKey"]),
                cpp_string(item["communication"]),
                cpp_string(item["searchKeywords"]),
                str(int(item["defaultChannelCount"])),
                str(int(item["defaultCoilStart"])),
                str(int(item["minimumChannelCount"])),
                str(int(item["maximumChannelCount"])),
                "true" if item["editableChannelCount"] else "false",
                "true" if item["editableCoilStart"] else "false",
                "true" if item["available"] else "false",
            ]
        )
        + "},"
    )
lines.extend(["};", ""])
output.write_text("\n".join(lines), encoding="utf-8")
