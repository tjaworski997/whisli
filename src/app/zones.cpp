#include "whisli/app/zones.hpp"

#include "whisli/system/config_store.hpp"

#include "whisli/app/state_events.hpp"

namespace whisli::app {
namespace {

constexpr char kNamespace[] = "whisli-zones";

String clean(const String& name) {
  String value = name;
  value.trim();
  return value;
}

}  // namespace

void ZoneManager::begin() {
  // The counts follow from the lengths (older firmware also stored them as "count"/"assignedCount").
  whisli::system::ConfigStore preferences;
  preferences.begin(kNamespace, true);
  const size_t zonesSize = preferences.getBytesLength("zones");
  if (zonesSize % sizeof(Zone) == 0 && zonesSize / sizeof(Zone) <= kMaxZones && preferences.getBytes("zones", zones_, zonesSize) == zonesSize) {
    count_ = zonesSize / sizeof(Zone);
  }
  const size_t assignedSize = preferences.getBytesLength("assigned");
  if (assignedSize % sizeof(Assignment) == 0 && assignedSize / sizeof(Assignment) <= kMaxZoneAssignments &&
      preferences.getBytes("assigned", assignments_, assignedSize) == assignedSize) {
    assignmentCount_ = assignedSize / sizeof(Assignment);
  }
}

bool ZoneManager::save() {
  whisli::system::ConfigStore preferences;
  preferences.begin(kNamespace, false);
  preferences.remove("count");
  preferences.remove("assignedCount");
  bool ok = true;
  if (count_) ok = ok && preferences.putBytes("zones", zones_, sizeof(Zone) * count_) == sizeof(Zone) * count_;
  else preferences.remove("zones");
  if (assignmentCount_) ok = ok && preferences.putBytes("assigned", assignments_, sizeof(Assignment) * assignmentCount_) == sizeof(Assignment) * assignmentCount_;
  else preferences.remove("assigned");
  preferences.end();
  notifyChange(kTopicZones);
  return ok;
}

int ZoneManager::indexOf(uint8_t id) const {
  for (uint8_t index = 0; index < count_; ++index) if (zones_[index].id == id) return index;
  return -1;
}

bool ZoneManager::exists(uint8_t id) const { return indexOf(id) >= 0; }

bool ZoneManager::nameTaken(const String& name, uint8_t except) const {
  for (uint8_t index = 0; index < count_; ++index) {
    if (zones_[index].id != except && name.equalsIgnoreCase(zones_[index].name)) return true;
  }
  return false;
}

uint8_t ZoneManager::create(const String& rawName) {
  const String name = clean(rawName);
  if (count_ >= kMaxZones || name.isEmpty() || name.length() >= sizeof(Zone::name) || nameTaken(name, 0)) return 0;
  uint8_t id = 1;
  while (exists(id)) ++id;  // at most kMaxZones zones, so a free id below 255 always exists
  Zone& zone = zones_[count_++];
  zone = Zone{};
  zone.id = id;
  strlcpy(zone.name, name.c_str(), sizeof(zone.name));
  if (save()) return id;
  --count_;
  return 0;
}

bool ZoneManager::rename(uint8_t id, const String& rawName) {
  const String name = clean(rawName);
  const int index = indexOf(id);
  if (index < 0 || name.isEmpty() || name.length() >= sizeof(Zone::name) || nameTaken(name, id)) return false;
  strlcpy(zones_[index].name, name.c_str(), sizeof(Zone::name));
  return save();
}

bool ZoneManager::remove(uint8_t id) {
  const int index = indexOf(id);
  if (index < 0) return false;
  for (uint8_t next = index + 1; next < count_; ++next) zones_[next - 1] = zones_[next];
  --count_;
  uint16_t kept = 0;
  for (uint16_t current = 0; current < assignmentCount_; ++current) {
    if (assignments_[current].zone != id) assignments_[kept++] = assignments_[current];
  }
  assignmentCount_ = kept;
  return save();
}

bool ZoneManager::reorder(const uint8_t* ids, uint8_t count) {
  if (count != count_) return false;
  Zone ordered[kMaxZones];
  for (uint8_t position = 0; position < count; ++position) {
    const int index = indexOf(ids[position]);
    if (index < 0) return false;
    for (uint8_t earlier = 0; earlier < position; ++earlier) if (ids[earlier] == ids[position]) return false;
    ordered[position] = zones_[index];
  }
  memcpy(zones_, ordered, sizeof(Zone) * count);
  return save();
}

uint8_t ZoneManager::zoneOf(const String& target) const {
  for (uint16_t index = 0; index < assignmentCount_; ++index) {
    if (target == assignments_[index].target) return assignments_[index].zone;
  }
  return 0;
}

bool ZoneManager::assign(const String& target, uint8_t zone) {
  if (target.isEmpty() || target.length() >= sizeof(Assignment::target) || (zone && !exists(zone))) return false;
  for (uint16_t index = 0; index < assignmentCount_; ++index) {
    if (target != assignments_[index].target) continue;
    if (assignments_[index].zone == zone) return true;
    if (zone) {
      assignments_[index].zone = zone;
    } else {
      assignments_[index] = assignments_[--assignmentCount_];
    }
    return save();
  }
  if (!zone) return true;
  if (assignmentCount_ >= kMaxZoneAssignments) return false;
  Assignment& assignment = assignments_[assignmentCount_++];
  strlcpy(assignment.target, target.c_str(), sizeof(assignment.target));
  assignment.zone = zone;
  return save();
}

}  // namespace whisli::app
