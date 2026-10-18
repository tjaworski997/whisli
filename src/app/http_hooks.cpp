#include "whisli/app/http_hooks.hpp"

#include <ETH.h>
#include <WiFi.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <mbedtls/ssl.h>

#include <algorithm>
#include <ctime>
#include <memory>

#include "whisli/app/state_events.hpp"
#include "whisli/net/json.hpp"
#include "whisli/system/config_store.hpp"
#include "whisli/system/log.hpp"
#include "whisli/system/state_lock.hpp"
#include "whisli/system/task_priorities.hpp"

namespace whisli::app {
namespace {

constexpr char kNamespace[] = "whisli-hooks";
constexpr char kSecretNamespace[] = "whisli-hookauth";  // headers: in backups only encrypted
constexpr uint8_t kFormat = 2;  // 1 had no flags byte
constexpr int kTimeoutMs = 5000;
constexpr size_t kMaxResponse = 512;

String key(uint8_t id) { return "h" + String(id); }

// Stored as [format][method][body kind][flags] then name, URL and body, each as a 16-bit length and
// bytes. Format 1 had no flags byte.
constexpr uint8_t kFlagInsecure = 1;

size_t headerSize(uint8_t format) { return format == 1 ? 3 : 4; }

// "Ignore certificate errors": instead of the CA bundle, a TLS set-up that does not verify the
// server. ESP-TLS sets "verification required" just before calling this, so it can be lowered here.
esp_err_t attachWithoutVerification(void* configuration) {
  mbedtls_ssl_conf_authmode(static_cast<mbedtls_ssl_config*>(configuration), MBEDTLS_SSL_VERIFY_NONE);
  return ESP_OK;
}
void appendText(std::vector<uint8_t>& data, const String& text) {
  const uint16_t length = static_cast<uint16_t>(text.length());
  data.push_back(length & 0xff);
  data.push_back(length >> 8);
  data.insert(data.end(), text.c_str(), text.c_str() + length);
}

bool readText(const std::vector<uint8_t>& data, size_t& offset, String& output) {
  if (offset + 2 > data.size()) return false;
  const uint16_t length = data[offset] | data[offset + 1] << 8;
  offset += 2;
  if (offset + length > data.size()) return false;
  output = String(reinterpret_cast<const char*>(data.data() + offset), length);
  offset += length;
  return true;
}

String clean(const String& value) {
  String text = value;
  text.trim();
  return text;
}

enum class Encoding { Url, Json, Plain };

String percentEncode(const String& value) {
  static const char hex[] = "0123456789ABCDEF";
  String output;
  for (const char character : value) {
    const uint8_t byte = static_cast<uint8_t>(character);
    if (isAlphaNumeric(character) || character == '-' || character == '_' || character == '.' || character == '~') output += character;
    else { output += '%'; output += hex[byte >> 4]; output += hex[byte & 0x0f]; }
  }
  return output;
}

struct Variables {
  String button, buttonId, gesture, zone, controller, ip, time, uptime;
};

// Replaces {name} with its value, encoded for where it lands. Only a run of letters and digits
// between braces is a variable, so JSON's own braces and unknown names stay as written.
String expand(const String& text, const Variables& variables, Encoding encoding) {
  const auto encode = [&](const String& value) {
    return encoding == Encoding::Url ? percentEncode(value) : encoding == Encoding::Json ? whisli::net::jsonEscape(value) : value;
  };
  String output;
  const int length = text.length();
  for (int position = 0; position < length; ++position) {
    if (text[position] != '{') { output += text[position]; continue; }
    int end = position + 1;
    while (end < length && isAlphaNumeric(text[end])) ++end;
    const String name = text.substring(position + 1, end);
    const String* value = end < length && text[end] == '}' ? (
        name == "button" ? &variables.button : name == "buttonId" ? &variables.buttonId :
        name == "gesture" ? &variables.gesture : name == "zone" ? &variables.zone :
        name == "controller" ? &variables.controller : name == "ip" ? &variables.ip :
        name == "time" ? &variables.time : name == "uptime" ? &variables.uptime : nullptr) : nullptr;
    if (!value) { output += '{'; continue; }
    output += encode(*value);
    position = end;  // past the closing brace
  }
  return output;
}

}  // namespace

const char* HttpHookManager::methodName(HookMethod method) {
  switch (method) {
    case HookMethod::Get: return "GET";
    case HookMethod::Put: return "PUT";
    case HookMethod::Delete: return "DELETE";
    default: return "POST";
  }
}

const char* HttpHookManager::bodyName(HookBody body) {
  switch (body) {
    case HookBody::None: return "none";
    case HookBody::Form: return "form";
    case HookBody::Text: return "text";
    default: return "json";
  }
}

bool HttpHookManager::parseMethod(const String& value, HookMethod& output) {
  if (value == "GET") output = HookMethod::Get;
  else if (value == "POST") output = HookMethod::Post;
  else if (value == "PUT") output = HookMethod::Put;
  else if (value == "DELETE") output = HookMethod::Delete;
  else return false;
  return true;
}

bool HttpHookManager::parseBody(const String& value, HookBody& output) {
  if (value == "none") output = HookBody::None;
  else if (value == "json") output = HookBody::Json;
  else if (value == "form") output = HookBody::Form;
  else if (value == "text") output = HookBody::Text;
  else return false;
  return true;
}

void HttpHookManager::begin() {
  whisli::system::ConfigStore store;
  store.begin(kNamespace, true);
  store.forEach([&](const String& name, nvs_type_t, const std::vector<uint8_t>& data) {
    if (!name.startsWith("h") || data.size() < 4 || (data[0] != 1 && data[0] != kFormat)) return;
    const long id = name.substring(1).toInt();
    size_t offset = headerSize(data[0]);
    String hookName;
    if (id < 1 || id > 255 || !readText(data, offset, hookName)) return;
    index_.push_back({static_cast<uint8_t>(id), hookName});
  });
  std::sort(index_.begin(), index_.end(), [](const Entry& left, const Entry& right) { return left.id < right.id; });
  jobs_ = xQueueCreate(16, sizeof(Job));
  // TLS needs a roomy stack; the task only ever runs when an action sends something.
  xTaskCreatePinnedToCore(taskEntry, "whisli-http", 10240, this, whisli::system::kMqttTaskPriority, nullptr, 0);
}

bool HttpHookManager::exists(uint8_t id) const {
  for (const Entry& entry : index_) if (entry.id == id) return true;
  return false;
}

String HttpHookManager::name(uint8_t id) const {
  for (const Entry& entry : index_) if (entry.id == id) return entry.name;
  return String();
}

bool HttpHookManager::load(uint8_t id, HttpHook& output) {
  whisli::system::ConfigStore store;
  store.begin(kNamespace, true);
  nvs_type_t type;
  std::vector<uint8_t> data;
  if (!store.read(key(id).c_str(), type, data) || data.size() < 4 || (data[0] != 1 && data[0] != kFormat)) return false;
  output = HttpHook{};
  output.id = id;
  output.method = static_cast<HookMethod>(data[1]);
  output.body = static_cast<HookBody>(data[2]);
  output.insecure = data[0] != 1 && (data[3] & kFlagInsecure);
  size_t offset = headerSize(data[0]);
  if (!readText(data, offset, output.name) || !readText(data, offset, output.url) || !readText(data, offset, output.bodyTemplate)) return false;
  whisli::system::ConfigStore secrets;
  secrets.begin(kSecretNamespace, true);
  output.headers = secrets.getString(key(id).c_str());
  return true;
}

bool HttpHookManager::save(const HttpHook& hook) {
  std::vector<uint8_t> data = {kFormat, static_cast<uint8_t>(hook.method), static_cast<uint8_t>(hook.body), static_cast<uint8_t>(hook.insecure ? kFlagInsecure : 0)};
  appendText(data, hook.name);
  appendText(data, hook.url);
  appendText(data, hook.bodyTemplate);
  whisli::system::ConfigStore store, secrets;
  if (!store.begin(kNamespace) || !secrets.begin(kSecretNamespace)) return false;
  const bool ok = store.putBytes(key(hook.id).c_str(), data.data(), data.size()) == data.size() &&
                  (hook.headers.isEmpty() ? (secrets.remove(key(hook.id).c_str()), true) : secrets.putString(key(hook.id).c_str(), hook.headers) == hook.headers.length());
  if (ok) notifyChange(kTopicActions);
  return ok;
}

uint8_t HttpHookManager::create(const String& rawName) {
  const String hookName = clean(rawName);
  if (index_.size() >= kMaxHooks || hookName.isEmpty() || hookName.length() > 48) return 0;
  uint8_t id = 1;
  while (exists(id)) ++id;
  HttpHook hook;
  hook.id = id;
  hook.name = hookName;
  hook.bodyTemplate = "{\n  \"button\": \"{button}\",\n  \"gesture\": \"{gesture}\",\n  \"zone\": \"{zone}\",\n  \"controller\": \"{controller}\",\n  \"time\": \"{time}\"\n}";
  if (!save(hook)) return 0;
  index_.push_back({id, hookName});
  return id;
}

bool HttpHookManager::update(const HttpHook& requested, String& reason) {
  HttpHook hook = requested;
  hook.name = clean(hook.name);
  hook.url = clean(hook.url);
  if (!exists(hook.id)) { reason = "Nie ma takiego webhooka."; return false; }
  if (hook.name.isEmpty() || hook.name.length() > 48) { reason = "Nazwa musi mieć od 1 do 48 znaków."; return false; }
  if (!hook.url.isEmpty() && !hook.url.startsWith("http://") && !hook.url.startsWith("https://")) { reason = "Adres musi zaczynać się od http:// albo https://."; return false; }
  if (hook.url.length() > kMaxHookUrl || hook.headers.length() > kMaxHookHeaders || hook.bodyTemplate.length() > kMaxHookBody) { reason = "Adres, nagłówki albo treść są za długie."; return false; }
  if (!save(hook)) { reason = "Nie udało się zapisać webhooka."; return false; }
  for (Entry& entry : index_) if (entry.id == hook.id) entry.name = hook.name;
  return true;
}

bool HttpHookManager::remove(uint8_t id) {
  for (auto entry = index_.begin(); entry != index_.end(); ++entry) {
    if (entry->id != id) continue;
    whisli::system::ConfigStore store, secrets;
    store.begin(kNamespace);
    secrets.begin(kSecretNamespace);
    store.remove(key(id).c_str());
    secrets.remove(key(id).c_str());
    index_.erase(entry);
    notifyChange(kTopicActions);
    return true;
  }
  return false;
}

bool HttpHookManager::trigger(uint8_t id, const HookContext& context) {
  if (!jobs_ || !exists(id)) return false;
  Job job = {};
  job.id = id;
  strlcpy(job.inputId, context.inputId.c_str(), sizeof(job.inputId));
  strlcpy(job.gesture, context.gesture.c_str(), sizeof(job.gesture));
  return xQueueSend(jobs_, &job, 0) == pdTRUE;
}

HookResult HttpHookManager::test(uint8_t id, const HookContext& context) {
  HttpHook hook;
  if (!load(id, hook)) return HookResult{false, 0, "Nie ma takiego webhooka.", String()};
  return send(hook, context);
}

void HttpHookManager::taskEntry(void* context) { static_cast<HttpHookManager*>(context)->task(); }

void HttpHookManager::task() {
  for (;;) {
    Job job;
    if (xQueueReceive(jobs_, &job, portMAX_DELAY) != pdTRUE) continue;
    HttpHook hook;
    if (!load(job.id, hook)) continue;
    const HookResult result = send(hook, HookContext{job.inputId, job.gesture});
    const String message = "HTTP \"" + hook.name + "\": " + (result.sent ? String(result.status) : result.error);
    whisli::system::write(result.sent && result.status < 400 ? whisli::system::LogLevel::Info : whisli::system::LogLevel::Warning, "actions", message.c_str());
  }
}

HookResult HttpHookManager::send(const HttpHook& hook, const HookContext& context) {
  HookResult result;
  if (hook.url.isEmpty()) { result.error = "Brak adresu."; return result; }
  if (!ETH.hasIP() && WiFi.status() != WL_CONNECTED) { result.error = "Brak sieci."; return result; }

  // Variables, read under the state lock (names and zones belong to the control side).
  Variables variables;
  {
    whisli::system::StateLock lock;
    const ControllerDevice controller = controller_.snapshot();
    variables.controller = controller.name;
    variables.buttonId = context.inputId;
    variables.gesture = context.gesture;
    const String prefix = "controller.input-";
    if (context.inputId.startsWith(prefix)) {
      const long input = context.inputId.substring(prefix.length()).toInt();
      if (input >= 1 && input <= kControllerInputCount) {
        variables.button = controller.inputNames[input - 1];
        if (variables.button.isEmpty()) variables.button = "DI" + String(input < 10 ? "0" : "") + String(input);
      }
    } else if (context.inputId.isEmpty()) {
      variables.button = "Test";
      variables.gesture = context.gesture.isEmpty() ? "test" : context.gesture;
    }
    const uint8_t zone = zones_.zoneOf(context.inputId);
    for (uint8_t index = 0; index < zones_.count(); ++index) if (zones_.at(index).id == zone) variables.zone = zones_.at(index).name;
  }
  variables.ip = ETH.hasIP() ? ETH.localIP().toString() : WiFi.localIP().toString();
  variables.uptime = String(millis() / 1000);
  const time_t now = time(nullptr);
  if (now > 1700000000) {
    char text[32];
    struct tm utc;
    gmtime_r(&now, &utc);  // the controller keeps UTC
    strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
    variables.time = text;
  }

  const String url = expand(hook.url, variables, Encoding::Url);
  const bool hasBody = hook.body != HookBody::None && hook.method != HookMethod::Get;
  const String body = hasBody ? expand(hook.bodyTemplate, variables, hook.body == HookBody::Json ? Encoding::Json : hook.body == HookBody::Form ? Encoding::Url : Encoding::Plain) : String();

  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.timeout_ms = kTimeoutMs;
  // https:// against the common public CAs, or not checked at all when asked to.
  config.crt_bundle_attach = hook.insecure ? attachWithoutVerification : esp_crt_bundle_attach;
  config.skip_cert_common_name_check = hook.insecure;
  config.method = hook.method == HookMethod::Get ? HTTP_METHOD_GET : hook.method == HookMethod::Put ? HTTP_METHOD_PUT :
                  hook.method == HookMethod::Delete ? HTTP_METHOD_DELETE : HTTP_METHOD_POST;
  config.disable_auto_redirect = false;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) { result.error = "Nieprawidłowy adres."; return result; }

  if (hasBody) {
    esp_http_client_set_header(client, "Content-Type", hook.body == HookBody::Json ? "application/json" :
                                                          hook.body == HookBody::Form ? "application/x-www-form-urlencoded" : "text/plain; charset=utf-8");
  }
  // "Name: value" per line; empty lines and lines without a colon are skipped.
  int lineStart = 0;
  while (lineStart < static_cast<int>(hook.headers.length())) {
    int lineEnd = hook.headers.indexOf('\n', lineStart);
    if (lineEnd < 0) lineEnd = hook.headers.length();
    const String line = hook.headers.substring(lineStart, lineEnd);
    const int colon = line.indexOf(':');
    if (colon > 0) {
      String headerName = line.substring(0, colon);
      String headerValue = expand(line.substring(colon + 1), variables, Encoding::Plain);
      headerName.trim();
      headerValue.trim();
      if (!headerName.isEmpty()) esp_http_client_set_header(client, headerName.c_str(), headerValue.c_str());
    }
    lineStart = lineEnd + 1;
  }

  esp_err_t error = esp_http_client_open(client, body.length());
  if (error == ESP_OK && body.length() && esp_http_client_write(client, body.c_str(), body.length()) != static_cast<int>(body.length())) error = ESP_FAIL;
  if (error == ESP_OK && esp_http_client_fetch_headers(client) < 0) error = ESP_FAIL;
  if (error != ESP_OK) {
    result.error = String("Brak połączenia z serwerem (") + esp_err_to_name(error) + ").";
  } else {
    result.sent = true;
    result.status = esp_http_client_get_status_code(client);
    char buffer[kMaxResponse + 1];
    const int read = esp_http_client_read_response(client, buffer, kMaxResponse);
    if (read > 0) { buffer[read] = '\0'; result.response = String(buffer); }
  }
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  return result;
}

}  // namespace whisli::app
