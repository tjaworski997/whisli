#pragma once

namespace whisli::system {

// One recursive lock around the application state. HTTP handlers run in the server task and
// loop() runs in its own; whoever touches lights, devices or actions holds this lock.
void beginStateLock();

class StateLock {
 public:
  StateLock();
  ~StateLock();
  StateLock(const StateLock&) = delete;
  StateLock& operator=(const StateLock&) = delete;
};

}  // namespace whisli::system
