#pragma once

namespace whisli::app {

// Starts the devices, the high-priority control task (buttons → actions → lights) and the API routes.
void begin();
// Low-priority housekeeping from loop(): pushes change notifications to the panel.
void poll();

}  // namespace whisli::app
