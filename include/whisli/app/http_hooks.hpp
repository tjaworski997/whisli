#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <vector>

#include "whisli/app/controller_io.hpp"
#include "whisli/app/zones.hpp"

namespace whisli::app {

constexpr uint8_t kMaxHooks = 128;
constexpr size_t kMaxHookUrl = 1024;
constexpr size_t kMaxHookHeaders = 2048;
constexpr size_t kMaxHookBody = 4096;

enum class HookMethod : uint8_t { Get = 0, Post = 1, Put = 2, Delete = 3 };
enum class HookBody : uint8_t { None = 0, Json = 1, Form = 2, Text = 3 };

// One HTTP request an action can send. `headers` holds "Name: value" lines (tokens go here) and is
// kept apart from the rest, so configuration backups carry it only in their encrypted part.
// URL, headers and body may use variables: {button}, {buttonId}, {gesture}, {zone}, {controller},
// {ip}, {time}, {uptime}; values are encoded for where they land (URL, JSON, form or plain text).
struct HttpHook {
  uint8_t id = 0;
  String name;
  HookMethod method = HookMethod::Post;
  String url;
  String headers;
  HookBody body = HookBody::Json;
  String bodyTemplate;
  bool insecure = false;  // https:// without checking the certificate (self-signed, home servers)
};

// Where a request came from: fills the variables.
struct HookContext {
  String inputId;  // "controller.input-1"; empty for a test from the webhooks page
  String gesture;  // "click", "doubleClick", …
};

struct HookResult {
  bool sent = false;  // a response arrived (any status)
  int status = 0;
  String error;       // why nothing arrived
  String response;    // the start of the response body
};

// Webhooks (HTTP integrations). Only ids, names and methods stay in memory; the full requests are read from
// files when sent, so many of them cost no RAM. Sending happens in a low-priority task with a
// timeout: a slow or unreachable server never holds up buttons and lights.
class HttpHookManager {
 public:
  HttpHookManager(ControllerIo& controller, ZoneManager& zones) : controller_(controller), zones_(zones) {}

  void begin();
  uint8_t count() const { return static_cast<uint8_t>(index_.size()); }
  uint8_t idAt(uint8_t index) const { return index_[index].id; }
  const String& nameAt(uint8_t index) const { return index_[index].name; }
  bool exists(uint8_t id) const;
  String name(uint8_t id) const;

  bool load(uint8_t id, HttpHook& output);
  uint8_t create(const String& name);  // new id, 0 on failure
  // `reason` receives why the request was refused (for the user).
  bool update(const HttpHook& hook, String& reason);
  bool remove(uint8_t id);

  // From an action (control path, under the state lock): queued, never waits. False when it cannot be.
  bool trigger(uint8_t id, const HookContext& context);
  // From the panel: sends now and returns what came back (up to the timeout).
  HookResult test(uint8_t id, const HookContext& context);

  static const char* methodName(HookMethod method);
  static const char* bodyName(HookBody body);
  static bool parseMethod(const String& value, HookMethod& output);
  static bool parseBody(const String& value, HookBody& output);

 private:
  struct Entry { uint8_t id; String name; };
  struct Job { uint8_t id; char inputId[32]; char gesture[16]; };
  static void taskEntry(void* context);
  void task();
  HookResult send(const HttpHook& hook, const HookContext& context);
  bool save(const HttpHook& hook);

  ControllerIo& controller_;
  ZoneManager& zones_;
  std::vector<Entry> index_;
  QueueHandle_t jobs_ = nullptr;
};

}  // namespace whisli::app
