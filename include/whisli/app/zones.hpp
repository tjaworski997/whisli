#pragma once

#include <Arduino.h>

namespace whisli::app {

constexpr uint8_t kMaxZones = 64;
constexpr uint16_t kMaxZoneAssignments = 1024;  // lights, buttons and anything else that gets a zone

// A named part of the house ("Parter – salon"). Flat on purpose: no floors or nesting.
struct Zone { uint8_t id = 0; char name[33] = {}; };

// Zones and what belongs to them. Things are named by the ids the API already uses ("light-3",
// "controller.input-2"), so lights and inputs keep their stored layouts and anything can get a zone
// later. Zones are kept in display order. Called under the state lock (HTTP routes).
class ZoneManager {
 public:
  void begin();
  uint8_t count() const { return count_; }
  const Zone& at(uint8_t index) const { return zones_[index]; }
  bool exists(uint8_t id) const;

  uint8_t create(const String& name);  // returns the new id, 0 on failure
  bool rename(uint8_t id, const String& name);
  bool remove(uint8_t id);              // things in it become unassigned
  bool reorder(const uint8_t* ids, uint8_t count);  // must list every zone exactly once

  uint8_t zoneOf(const String& target) const;  // 0 = no zone
  bool assign(const String& target, uint8_t zone);  // zone 0 removes the assignment
  uint16_t assignmentCount() const { return assignmentCount_; }
  const char* assignmentTarget(uint16_t index) const { return assignments_[index].target; }
  uint8_t assignmentZone(uint16_t index) const { return assignments_[index].zone; }

 private:
  struct Assignment { char target[24] = {}; uint8_t zone = 0; };
  int indexOf(uint8_t id) const;
  bool nameTaken(const String& name, uint8_t except) const;
  bool save();

  Zone zones_[kMaxZones];
  uint8_t count_ = 0;
  Assignment* assignments_ = new Assignment[kMaxZoneAssignments]();  // ~25 KB: lands in PSRAM
  uint16_t assignmentCount_ = 0;
};

}  // namespace whisli::app
