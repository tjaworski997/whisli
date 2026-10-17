#pragma once

#include <cstdint>

namespace whisli::bootstrap {

// Systemowa warstwa dostępu do sterownika: sieć (Ethernet, Wi-Fi, punkt dostępowy do konfiguracji),
// serwer HTTP z logowaniem, strona /setup oraz aktualizacja firmware przez OTA.
void begin();
void poll();

// Restarts the controller after `delayMs`, from its own task so the HTTP answer still goes out.
void scheduleRestart(uint32_t delayMs);

}  // namespace whisli::bootstrap
