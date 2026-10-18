#include "whisli/bootstrap/bootstrap_portal.hpp"

#include <Arduino.h>
#include <ETH.h>
#include <Network.h>
#include <Preferences.h>
#include <Update.h>
#include <mbedtls/base64.h>
#include <WiFi.h>

#include <algorithm>
#include <memory>

#include "whisli/net/auth.hpp"
#include "whisli/net/http_server.hpp"
#include "whisli/net/json.hpp"
#include "whisli/net/mqtt_settings.hpp"
#include "whisli/system/config_backup.hpp"
#include "whisli/system/log.hpp"
#include "whisli/system/notes.hpp"
#include "whisli/version.generated.hpp"

namespace whisli::bootstrap {
namespace {

using whisli::net::jsonEscape;
using whisli::net::Method;
using whisli::net::Request;

constexpr char kSetupSsid[] = "Whisli-Setup";
// WPA2 requires a password of at least eight characters.
constexpr char kSetupPassword[] = "whisli_gogo";
constexpr char kPreferencesNamespace[] = "whisli-net";
constexpr char kSsidKey[] = "ssid";
constexpr char kPasswordKey[] = "password";
constexpr char kApEnabledKey[] = "apOn";
constexpr char kApPasswordKey[] = "apPass";
// With the access point switched off, it comes back on its own after this long without Ethernet
// and Wi-Fi, so the controller can always be reached.
constexpr unsigned long kApFallbackMs = 60000;

Preferences preferences;
// Wi-Fi setup state is shared by loop() and the HTTP task. It has its own small lock so neither side
// ever waits on the application state lock (which the button/light task needs).
SemaphoreHandle_t wifiMutex = xSemaphoreCreateMutex();
struct WifiLock {
  WifiLock() { xSemaphoreTake(wifiMutex, portMAX_DELAY); }
  ~WifiLock() { xSemaphoreGive(wifiMutex); }
};
bool wifiConnectionPending = false;
bool wifiConnectionSucceeded = false;
String pendingWifiSsid;
String pendingWifiPassword;
unsigned long wifiConnectionStartedAt = 0;
bool apEnabled = true;        // the setting
bool apFallback = false;      // on although switched off: no other network
unsigned long lastOnlineAt = 0;

// Pin mapping verified against Waveshare ESP32-S3-POE-ETH-8DI-8DO documentation.
constexpr int kEthChipSelectPin = 16;
constexpr int kEthInterruptPin = 12;
constexpr int kEthResetPin = 39;
constexpr int kEthSpiClockPin = 15;
constexpr int kEthSpiMisoPin = 14;
constexpr int kEthSpiMosiPin = 13;
constexpr int kW5500PhyAddress = 1;

const char kPage[] PROGMEM = R"HTML(
<!doctype html><html lang="pl"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Whisli · bootstrap</title><style>
:root{color-scheme:dark;--bg:#050505;--ink:#f2f2f2;--muted:#8d8d8d;--line:#2a2a2a;--soft:#141414}*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:14px ui-monospace,SFMono-Regular,Menlo,Monaco,Consolas,monospace;line-height:1.55}.shell{max-width:820px;margin:auto;padding:45px 22px 70px}header{padding:0 0 28px;border-bottom:1px solid var(--line)}h1{font-size:26px;font-weight:500;letter-spacing:-.06em;margin:0}header p{color:var(--muted);margin:6px 0 0}.section{padding:28px 0;border-bottom:1px solid var(--line)}h2{font-size:11px;font-weight:500;letter-spacing:.16em;color:var(--muted);text-transform:uppercase;margin:0 0 18px}.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px 42px}dl{display:grid;grid-template-columns:125px 1fr;gap:7px;margin:0}dt{color:var(--muted)}dd{margin:0;word-break:break-word}.active{color:#fff}.idle{color:#777}form{display:grid;gap:12px}.row{display:grid;grid-template-columns:1fr 1fr auto;gap:9px}button,input,select{appearance:none;font:inherit;color:var(--ink);background:transparent;border:1px solid var(--line);border-radius:0;padding:10px 11px}button{cursor:pointer;white-space:nowrap}button:hover{border-color:#fff}button:disabled{color:#777;border-color:#222;cursor:wait}.quiet{color:var(--muted)}.hint,#notice{font-size:12px;color:var(--muted);margin:0}.pulse:before{content:'·';display:inline-block;margin-right:7px;animation:pulse .8s steps(2,end) infinite}@keyframes pulse{50%{opacity:.15}}@media(max-width:640px){.shell{padding:28px 18px}.grid,.row{grid-template-columns:1fr}dl{grid-template-columns:105px 1fr}}
</style></head><body><main class="shell"><header><h1>Whisli</h1><p>bootloader / <span id="version">…</span></p></header>
<section class="section"><h2>Sieć</h2><div class="grid"><dl><dt>Ethernet</dt><dd id="ethernetIp">oczekiwanie na DHCP</dd><dt>Wi‑Fi</dt><dd id="wifi">nie połączono</dd><dt>IP Wi‑Fi</dt><dd id="stationIp">—</dd></dl><dl><dt>Brama</dt><dd id="gateway">—</dd><dt>Setup AP</dt><dd id="apIp">—</dd><dt>Panel</dt><dd>HTTP / hasło</dd></dl></div></section>
<section class="section"><h2>Połącz z Wi‑Fi <span class="quiet">(opcjonalnie)</span></h2><form id="network"><div class="row"><input id="ssid" name="ssid" list="wifi-networks" placeholder="SSID — wpisz lub wybierz z listy" required><datalist id="wifi-networks"></datalist><input name="password" type="password" placeholder="Hasło Wi‑Fi"><button type="button" id="scan"><span>Szukaj sieci Wi‑Fi</span></button></div><button type="submit">Połącz i zapisz</button></form><p class="hint">Dane Wi‑Fi zostaną zapisane po udanym połączeniu.</p><p id="notice"></p></section>
<section class="section"><h2>System</h2><div class="grid"><dl><dt>Sterownik</dt><dd id="board">…</dd><dt>Układ</dt><dd id="chip">…</dd><dt>Rewizja</dt><dd id="revision">…</dd><dt>CPU</dt><dd id="cpu">…</dd><dt>Rdzenie</dt><dd id="cores">…</dd></dl><dl><dt>Flash</dt><dd id="flash">…</dd><dt>PSRAM</dt><dd id="psram">…</dd><dt>MAC Wi‑Fi</dt><dd id="mac">…</dd><dt>Build</dt><dd id="build">…</dd><dt>Firmware</dt><dd id="firmware">…</dd></dl></div></section>
<section class="section"><h2>Aktualizacja firmware</h2><form id="update"><input type="file" name="firmware" accept=".bin,application/octet-stream" required><button type="submit">Wgraj firmware</button></form><p class="hint">Po poprawnym zapisie urządzenie uruchomi się ponownie.</p></section>
</main><script>
const q=s=>document.querySelector(s),notice=t=>q('#notice').textContent=t;
async function api(url,opt){let r=await fetch(url,opt);let data=await r.json().catch(()=>({message:'Błędna odpowiedź'}));if(!r.ok)throw Error(data.message||'Operacja nie powiodła się');return data}
function show(s){q('#version').textContent=s.firmware.version;q('#firmware').textContent=s.firmware.version;q('#ethernetIp').textContent=s.network.ethernetIp||'oczekiwanie na DHCP';q('#wifi').textContent=s.network.connected?'połączono / '+s.network.ssid:'nie połączono';q('#stationIp').textContent=s.network.stationIp||'—';q('#gateway').textContent=s.network.gateway||'—';q('#apIp').textContent=s.network.accessPointIp;q('#board').textContent=s.hardware.board;q('#chip').textContent=s.hardware.model;q('#revision').textContent=s.hardware.revision;q('#cpu').textContent=s.hardware.cpuMhz+' MHz';q('#cores').textContent=s.hardware.cores;q('#flash').textContent=s.hardware.flashMb+' MB';q('#psram').textContent=s.hardware.psramMb+' MB';q('#mac').textContent=s.hardware.wifiMac;q('#build').textContent=s.firmware.build}
async function status(){try{show(await api('/api/status'))}catch(e){notice(e.message)}}
const scan=q('#scan');async function scanWifi(){scan.disabled=true;scan.classList.add('pulse');scan.firstElementChild.textContent='Skanowanie Wi‑Fi';try{await api('/api/wifi/scan/start',{method:'POST'});for(;;){await new Promise(r=>setTimeout(r,450));let d=await api('/api/wifi/scan');if(d.state==='running')continue;let list=q('#wifi-networks');list.innerHTML='';d.networks.forEach(n=>{let o=document.createElement('option');o.value=n.ssid;list.append(o)});notice(d.networks.length?'Wybierz sieć z podpowiedzi lub wpisz SSID ręcznie.':'Nie znaleziono sieci. Możesz wpisać SSID ręcznie.');break}}catch(e){notice(e.message)}finally{scan.disabled=false;scan.classList.remove('pulse');scan.firstElementChild.textContent='Szukaj sieci Wi‑Fi'}}scan.onclick=scanWifi;
q('#network').onsubmit=async e=>{e.preventDefault();let f=new FormData(e.target);notice('Łączenie z siecią…');try{await api('/api/wifi/connect',{method:'POST',body:new URLSearchParams(f)});for(;;){await new Promise(r=>setTimeout(r,500));let d=await api('/api/wifi/connect/status');if(d.state==='connecting')continue;notice(d.message);await status();break}}catch(e){notice(e.message)}};
q('#update').onsubmit=async e=>{e.preventDefault();let f=new FormData(e.target);notice('Trwa aktualizacja…');try{let d=await api('/update',{method:'POST',body:f});notice(d.message)}catch(e){notice(e.message)}};status();setInterval(status,5000);
</script></body></html>
)HTML";


void startAccessPoint() {
  // The Wi-Fi driver would save its configuration to the settings storage (NVS) on every change; the
  // network credentials are kept in "whisli-net" anyway, and the storage is small.
  WiFi.persistent(false);
  apEnabled = preferences.getBool(kApEnabledKey, true);
  WiFi.mode(apEnabled ? WIFI_AP_STA : WIFI_STA);
  // Modem sleep adds latency spikes that stall large uploads (OTA) over Wi-Fi; the board is mains/PoE powered.
  WiFi.setSleep(false);
  if (!apEnabled) return;
  WiFi.softAP(kSetupSsid, preferences.getString(kApPasswordKey, kSetupPassword).c_str());
  Serial.printf("[WIFI] setup AP: %s / %s\n", kSetupSsid, WiFi.softAPIP().toString().c_str());
}

void setAccessPoint(bool on) {
  if (on) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(kSetupSsid, preferences.getString(kApPasswordKey, kSetupPassword).c_str());
  } else {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
  }
}

bool accessPointActive() { return WiFi.getMode() & WIFI_MODE_AP; }

// Switched off and no other way in: bring the access point back until a network returns.
void watchAccessPointFallback() {
  const bool online = ETH.hasIP() || WiFi.status() == WL_CONNECTED;
  if (online) lastOnlineAt = millis();
  if (apEnabled) return;
  if (!apFallback && !online && millis() - lastOnlineAt >= kApFallbackMs) {
    apFallback = true;
    setAccessPoint(true);
    whisli::system::write(whisli::system::LogLevel::Warning, "network", "No Ethernet or Wi-Fi for a minute: setup access point switched on");
  } else if (apFallback && online) {
    apFallback = false;
    setAccessPoint(false);
    whisli::system::write(whisli::system::LogLevel::Info, "network", "Network back: setup access point switched off again");
  }
}

void startEthernet() {
  Network.begin();
  Network.onEvent([](arduino_event_id_t event, arduino_event_info_t) {
    if (event == ARDUINO_EVENT_ETH_GOT_IP) {
      Serial.printf("[ETH] DHCP: %s\n", ETH.localIP().toString().c_str());
      whisli::system::write(whisli::system::LogLevel::Info, "network", "Ethernet connected and received DHCP address");
    } else if (event == ARDUINO_EVENT_ETH_DISCONNECTED) {
      Serial.println("[ETH] link down");
      whisli::system::write(whisli::system::LogLevel::Warning, "network", "Ethernet link disconnected");
    }
  });
  const bool started = ETH.begin(ETH_PHY_W5500, kW5500PhyAddress,
                                 kEthChipSelectPin, kEthInterruptPin, kEthResetPin,
                                 SPI2_HOST, kEthSpiClockPin, kEthSpiMisoPin,
                                 kEthSpiMosiPin);
  if (!started) Serial.println("[ETH] W5500 failed to start");
  else Serial.println("[ETH] waiting for link and DHCP");
}

void startWifiConnection(const String& ssid, const String& password) {
  WifiLock lock;
  pendingWifiSsid = ssid;
  pendingWifiPassword = password;
  wifiConnectionSucceeded = false;
  wifiConnectionPending = true;
  wifiConnectionStartedAt = millis();
  WiFi.disconnect();
  WiFi.begin(ssid.c_str(), password.c_str());
  whisli::system::write(whisli::system::LogLevel::Info, "network", "Wi-Fi connection started");
}

void sendStatus(Request& request) {
  const bool connected = WiFi.status() == WL_CONNECTED;
  String body = "{\"firmware\":{\"version\":\"" + String(kFirmwareVersion) +
                "\",\"build\":\"" + String(__DATE__ " " __TIME__) +
                "\"},\"network\":{\"connected\":" + String(connected ? "true" : "false") +
                ",\"ethernetIp\":\"" + (ETH.hasIP() ? ETH.localIP().toString() : "") + "\"" +
                ",\"ssid\":\"" + jsonEscape(connected ? WiFi.SSID() : String()) +
                "\",\"stationIp\":\"" + (connected ? WiFi.localIP().toString() : "") +
                "\",\"gateway\":\"" + (connected ? WiFi.gatewayIP().toString() : "") +
                "\",\"accessPointIp\":\"" + WiFi.softAPIP().toString() +
                "\"},\"hardware\":{\"board\":\"Waveshare ESP32-S3-POE-ETH-8DI-8DO" +
                String("\",\"model\":\"") + ESP.getChipModel() +
                "\",\"revision\":\"" + String(ESP.getChipRevision()) +
                "\",\"cpuMhz\":" + String(ESP.getCpuFreqMHz()) +
                ",\"cores\":" + String(ESP.getChipCores()) +
                ",\"flashMb\":" + String(ESP.getFlashChipSize() / 1024 / 1024) +
                ",\"psramMb\":" + String(ESP.getPsramSize() / 1024 / 1024) +
                ",\"wifiMac\":\"" + WiFi.macAddress() + "\"}}";
  request.sendJson(200, body);
}

// --- Firmware upload -------------------------------------------------------------------------
// Accepts the image as the raw request body or as the single file of a multipart form (curl -F,
// older panels). The body is streamed straight into the OTA partition.
class FirmwareUpload {
 public:
  bool run(Request& request) {
    const String contentType = request.header("Content-Type");
    const int boundaryAt = contentType.indexOf("boundary=");
    multipart_ = contentType.startsWith("multipart/form-data") && boundaryAt >= 0;
    if (multipart_) delimiter_ = "\r\n--" + contentType.substring(boundaryAt + 9);
    if (request.contentLength() == 0) return fail("Firmware upload is empty");
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) return fail("Could not start firmware update");
    whisli::system::write(whisli::system::LogLevel::Info, "firmware", "Firmware upload started");

    std::unique_ptr<uint8_t[]> chunk(new uint8_t[kChunkSize]);
    size_t remaining = request.contentLength();
    while (remaining > 0 && !finished_) {
      const int received = request.read(chunk.get(), std::min(remaining, kChunkSize));
      if (received <= 0) return fail("Firmware upload aborted");
      remaining -= received;
      if (!consume(chunk.get(), received)) return false;
    }
    if (multipart_ && !finished_) return fail("Firmware upload has no closing boundary");
    if (written_ == 0 || !Update.end(true) || !Update.isFinished()) return fail("Firmware image rejected");
    char message[88];
    snprintf(message, sizeof(message), "Firmware received: %u KB", static_cast<unsigned>(written_ / 1024));
    whisli::system::write(whisli::system::LogLevel::Info, "firmware", message);
    return true;
  }

 private:
  static constexpr size_t kChunkSize = 4096;

  bool consume(const uint8_t* data, size_t length) {
    if (!multipart_) return write(data, length);
    pending_.insert(pending_.end(), data, data + length);
    if (!inBody_) {
      // Skip the part headers, which end with an empty line.
      static constexpr char kHeaderEnd[] = "\r\n\r\n";
      const auto end = std::search(pending_.begin(), pending_.end(), kHeaderEnd, kHeaderEnd + 4);
      if (end == pending_.end()) return pending_.size() < 2048 || fail("Firmware upload has invalid form headers");
      pending_.erase(pending_.begin(), end + 4);
      inBody_ = true;
    }
    // Write everything before the closing delimiter; keep a tail that might be its beginning.
    const auto delimiter = std::search(pending_.begin(), pending_.end(), delimiter_.c_str(), delimiter_.c_str() + delimiter_.length());
    if (delimiter != pending_.end()) {
      finished_ = true;
      return write(pending_.data(), delimiter - pending_.begin());
    }
    const size_t keep = std::min(pending_.size(), static_cast<size_t>(delimiter_.length()));
    const size_t ready = pending_.size() - keep;
    if (!write(pending_.data(), ready)) return false;
    pending_.erase(pending_.begin(), pending_.begin() + ready);
    return true;
  }

  bool write(const uint8_t* data, size_t length) {
    if (length == 0) return true;
    if (Update.write(const_cast<uint8_t*>(data), length) != length) return fail("Flash write failed");
    written_ += length;
    return true;
  }

  bool fail(const char* reason) {
    char message[88];
    snprintf(message, sizeof(message), "%s after %u KB: %s", reason, static_cast<unsigned>(written_ / 1024),
             Update.hasError() ? Update.errorString() : "-");
    whisli::system::write(whisli::system::LogLevel::Error, "firmware", message);
    if (Update.isRunning()) Update.abort();
    return false;
  }

  bool multipart_ = false;
  bool inBody_ = false;
  bool finished_ = false;
  String delimiter_;
  std::vector<uint8_t> pending_;
  size_t written_ = 0;
};

void handleFirmwareUpload(Request& request) {
  FirmwareUpload upload;
  if (!upload.run(request)) {
    request.sendJson(500, "{\"message\":\"Aktualizacja nie powiodła się lub plik jest niekompletny.\"}");
    return;
  }
  whisli::system::write(whisli::system::LogLevel::Info, "firmware", "Firmware upload verified; restarting controller");
  // Answer first and restart a moment later, so the browser receives a clean response.
  request.setHeader("Connection", "close");
  request.sendJson(200, "{\"message\":\"Firmware zapisany. Trwa restart urządzenia…\"}");
  scheduleRestart(1000);
}

void sendAuthResult(Request& request, const whisli::net::auth::LoginResult& result, const char* success) {
  switch (result.status) {
    case 200: request.sendJson(200, String("{\"message\":\"") + success + "\"}"); break;
    case 400: request.sendJson(400, "{\"message\":\"Hasło nie może być puste ani dłuższe niż 64 znaki.\"}"); break;
    default: request.sendJson(401, "{\"message\":\"Nieprawidłowe hasło.\"}");
  }
}

void configureAuthRoutes() {
  namespace auth = whisli::net::auth;
  using whisli::net::on;
  using whisli::net::onPublic;
  using whisli::net::StateAccess;
  onPublic("/api/v1/auth/login", Method::Post, [](Request& request) {
    const auto result = auth::login(request.arg("password"), request.arg("remember") == "true");
    if (result.status == 200) request.setHeader("Set-Cookie", result.setCookie);
    sendAuthResult(request, result, "Zalogowano.");
  });
  onPublic("/api/v1/auth/logout", Method::Post, [](Request& request) {
    request.setHeader("Set-Cookie", auth::logout(request.header("Cookie")));
    request.sendJson(200, "{\"message\":\"Wylogowano.\"}");
  });
  on("/api/v1/auth", Method::Get, [](Request& request) {
    request.sendJson(200, String("{\"defaultPassword\":") + (auth::usingDefaultPassword() ? "true" : "false") + "}");
  }, StateAccess::None);
  on("/api/v1/auth/password", Method::Post, [](Request& request) {
    sendAuthResult(request, auth::changePassword(request.header("Cookie"), request.arg("current"), request.arg("password")), "Hasło zmienione.");
  }, StateAccess::None);
}

void configureBackupRoutes() {
  using whisli::net::on;
  using whisli::net::StateAccess;
  // Locked: a consistent snapshot, never half of a save in progress.
  on("/api/v1/config/backup", Method::Get, [](Request& request) {
    request.setHeader("Content-Disposition", "attachment; filename=\"whisli_config.json\"");
    request.sendJson(200, whisli::system::backup::exportJson());
  });
  // With passphrase=… (8+ characters): network passwords included, encrypted with it. POST keeps it out of URLs.
  on("/api/v1/config/backup", Method::Post, [](Request& request) {
    const String passphrase = request.arg("passphrase");
    if (passphrase.length() < 8 || passphrase.length() > 64) { request.sendJson(400, "{\"message\":\"Hasło kopii musi mieć od 8 do 64 znaków.\"}"); return; }
    const String body = whisli::system::backup::exportJson(passphrase);
    if (body.isEmpty()) { request.sendJson(500, "{\"message\":\"Nie udało się zaszyfrować haseł.\"}"); return; }
    request.setHeader("Content-Disposition", "attachment; filename=\"whisli_config.json\"");
    request.sendJson(200, body);
  });
  // The file arrives as the raw request body. Applying it means a restart, so every module loads it.
  whisli::net::onStream("/api/v1/config/restore", Method::Post, [](Request& request) {
    constexpr size_t kMaxBackup = 256 * 1024;
    const size_t length = request.contentLength();
    if (length == 0 || length > kMaxBackup) { request.sendJson(413, "{\"message\":\"Plik jest pusty albo za duży.\"}"); return; }
    std::unique_ptr<char, decltype(&free)> body(static_cast<char*>(ps_malloc(length)), free);
    if (!body) { request.sendJson(500, "{\"message\":\"Za mało pamięci na wczytanie pliku.\"}"); return; }
    size_t received = 0;
    while (received < length) {
      const int chunk = request.read(reinterpret_cast<uint8_t*>(body.get() + received), length - received);
      if (chunk <= 0) { request.sendJson(400, "{\"message\":\"Plik nie dotarł w całości.\"}"); return; }
      received += chunk;
    }
    // The backup password comes base64-encoded in a header (UTF-8 safe, kept out of the URL).
    String passphrase;
    const String encoded = request.header("X-Backup-Passphrase");
    if (!encoded.isEmpty()) {
      unsigned char decoded[96] = {};
      size_t decodedLength = 0;
      if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decodedLength, reinterpret_cast<const unsigned char*>(encoded.c_str()), encoded.length()) == 0) {
        passphrase = String(reinterpret_cast<const char*>(decoded));
      }
    }
    const auto result = whisli::system::backup::import(body.get(), length, passphrase);
    if (!result.error.isEmpty()) {
      whisli::system::write(whisli::system::LogLevel::Warning, "system", ("Configuration restore rejected: " + result.error).c_str());
      request.sendJson(400, "{\"message\":\"" + whisli::net::jsonEscape(result.error) + "\"}");
      return;
    }
    whisli::system::write(whisli::system::LogLevel::Info, "system", result.secretsApplied ? "Configuration and network passwords restored from a backup; restarting"
                                                                                          : "Configuration restored from a backup; restarting");
    const char* note = !result.secretsPresent ? "" : result.secretsApplied ? " Hasła sieci też wczytane." :
                       result.wrongPassphrase ? " Hasła sieci pominięte — nieprawidłowe hasło kopii." : " Hasła sieci pominięte — nie podano hasła kopii.";
    request.setHeader("Connection", "close");
    request.sendJson(200, "{\"message\":\"Konfiguracja wczytana." + String(note) + " Sterownik uruchamia się ponownie.\",\"secretsApplied\":" +
                          (result.secretsApplied ? "true" : "false") + "}");
    scheduleRestart(1000);
  });

  // Installation notes: plain text, saved by the panel as the user types.
  on("/api/v1/notes", Method::Get, [](Request& request) {
    const auto note = whisli::system::notes::load();
    request.sendJson(200, "{\"text\":\"" + whisli::net::jsonEscape(note.text) + "\",\"modified\":" + String(note.modified) +
                          ",\"maxLength\":" + String(whisli::system::notes::kMaxCharacters) + "}");
  }, StateAccess::None);
  // The text arrives as the raw body (UTF-8); X-Client-Time stamps it when the controller has no clock.
  whisli::net::onStream("/api/v1/notes", Method::Post, [](Request& request) {
    const size_t length = request.contentLength();
    if (length > whisli::system::notes::kMaxBytes) { request.sendJson(413, "{\"message\":\"Notatka jest za długa.\"}"); return; }
    std::unique_ptr<char, decltype(&free)> body(static_cast<char*>(ps_malloc(length + 1)), free);
    if (!body) { request.sendJson(500, "{\"message\":\"Za mało pamięci.\"}"); return; }
    size_t received = 0;
    while (received < length) {
      const int chunk = request.read(reinterpret_cast<uint8_t*>(body.get() + received), length - received);
      if (chunk <= 0) { request.sendJson(400, "{\"message\":\"Notatka nie dotarła w całości.\"}"); return; }
      received += chunk;
    }
    body.get()[length] = '\0';
    const uint32_t clientTime = static_cast<uint32_t>(request.header("X-Client-Time").toInt());
    const time_t now = time(nullptr);
    if (!whisli::system::notes::save(String(body.get()), now > 1700000000 ? 0 : clientTime)) {
      request.sendJson(500, "{\"message\":\"Nie udało się zapisać notatki.\"}");
      return;
    }
    request.sendJson(200, "{\"modified\":" + String(whisli::system::notes::load().modified) + "}");
  });
}

void sendNetwork(Request& request) {
  const bool wifiConnected = WiFi.status() == WL_CONNECTED;
  const auto mqtt = whisli::net::mqtt::load();
  const auto mqttStatus = whisli::net::mqtt::status();
  const auto flag = [](bool value) { return value ? "true" : "false"; };
  String body = "{\"ethernet\":{\"connected\":" + String(flag(ETH.hasIP())) + ",\"link\":" + flag(ETH.linkUp()) +
                ",\"ip\":\"" + (ETH.hasIP() ? ETH.localIP().toString() : "") + "\",\"gateway\":\"" + (ETH.hasIP() ? ETH.gatewayIP().toString() : "") +
                "\",\"mac\":\"" + ETH.macAddress() + "\"}";
  body += ",\"wifi\":{\"connected\":" + String(flag(wifiConnected)) + ",\"ssid\":\"" + jsonEscape(wifiConnected ? WiFi.SSID() : String()) +
          "\",\"savedSsid\":\"" + jsonEscape(preferences.getString(kSsidKey, "")) + "\",\"ip\":\"" + (wifiConnected ? WiFi.localIP().toString() : "") +
          "\",\"rssi\":" + String(wifiConnected ? WiFi.RSSI() : 0) + ",\"mac\":\"" + WiFi.macAddress() + "\"}";
  body += ",\"accessPoint\":{\"enabled\":" + String(flag(apEnabled)) + ",\"active\":" + flag(accessPointActive()) +
          ",\"fallback\":" + flag(apFallback) + ",\"ssid\":\"" + kSetupSsid + "\",\"ip\":\"" +
          (accessPointActive() ? WiFi.softAPIP().toString() : "") + "\",\"customPassword\":" + flag(preferences.isKey(kApPasswordKey)) + "}";
  // Passwords never leave the controller; the panel only learns whether one is set.
  body += ",\"mqtt\":{\"enabled\":" + String(flag(mqtt.enabled)) + ",\"host\":\"" + jsonEscape(mqtt.host) + "\",\"port\":" + String(mqtt.port) +
          ",\"user\":\"" + jsonEscape(mqtt.user) + "\",\"hasPassword\":" + flag(!mqtt.password.isEmpty()) +
          ",\"baseTopic\":\"" + jsonEscape(mqtt.baseTopic) + "\",\"discovery\":" + flag(mqtt.discovery) +
          ",\"discoveryPrefix\":\"" + jsonEscape(mqtt.discoveryPrefix) + "\",\"deviceName\":\"" + jsonEscape(mqtt.deviceName) + "\",\"status\":\"" +
          whisli::net::mqtt::stateName(mqttStatus.state) + "\",\"error\":\"" + jsonEscape(mqttStatus.error) + "\"}}";
  request.sendJson(200, body);
}

// Wi-Fi routes live under /api/v1/network/wifi/ for the panel and under /api/wifi/ for the /setup page.
void onWifi(const char* path, Method method, whisli::net::Handler handler) {
  whisli::net::on((String("/api/v1/network/wifi/") + path).c_str(), method, handler, whisli::net::StateAccess::None);
  whisli::net::on((String("/api/wifi/") + path).c_str(), method, std::move(handler), whisli::net::StateAccess::None);
}

void configureNetworkRoutes() {
  using whisli::net::on;
  using whisli::net::StateAccess;
  on("/api/v1/network", Method::Get, sendNetwork, StateAccess::None);
  // enabled=true|false, password (optional, 8–63 characters for WPA2).
  on("/api/v1/network/ap", Method::Post, [](Request& request) {
    const String enabled = request.arg("enabled");
    const String password = request.arg("password");
    if ((enabled != "true" && enabled != "false") || (!password.isEmpty() && (password.length() < 8 || password.length() > 63))) {
      request.sendJson(400, "{\"message\":\"Hasło punktu dostępowego musi mieć od 8 do 63 znaków.\"}");
      return;
    }
    if (!password.isEmpty()) preferences.putString(kApPasswordKey, password);
    apEnabled = enabled == "true";
    preferences.putBool(kApEnabledKey, apEnabled);
    apFallback = false;
    lastOnlineAt = millis();
    request.sendJson(200, "{\"message\":\"Ustawienia punktu dostępowego zapisane.\"}");
    setAccessPoint(apEnabled);  // after answering: a panel opened through the access point loses it here
    whisli::system::write(whisli::system::LogLevel::Info, "network", apEnabled ? "Setup access point switched on" : "Setup access point switched off");
  }, StateAccess::None);
  on("/api/v1/network/wifi/forget", Method::Post, [](Request& request) {
    preferences.remove(kSsidKey);
    preferences.remove(kPasswordKey);
    {
      WifiLock lock;
      wifiConnectionPending = false;
    }
    WiFi.disconnect();
    whisli::system::write(whisli::system::LogLevel::Info, "network", "Wi-Fi network forgotten");
    request.sendJson(200, "{\"message\":\"Sieć Wi-Fi zapomniana.\"}");
  }, StateAccess::None);
  on("/api/v1/network/mqtt", Method::Post, [](Request& request) {
    auto settings = whisli::net::mqtt::load();
    const long port = request.arg("port").toInt();
    settings.enabled = request.arg("enabled") == "true";
    settings.host = request.arg("host");
    settings.host.trim();
    settings.user = request.arg("user");
    if (request.hasArg("password")) settings.password = request.arg("password");  // sent only when changed
    if (request.arg("clearPassword") == "true") settings.password = "";
    settings.baseTopic = request.arg("baseTopic");
    settings.discovery = request.arg("discovery") == "true";
    settings.discoveryPrefix = request.arg("discoveryPrefix");
    if (request.hasArg("deviceName")) {
      settings.deviceName = request.arg("deviceName");
      settings.deviceName.trim();
    }
    if (settings.deviceName.length() > 64 || port < 1 || port > 65535 || (settings.enabled && settings.host.isEmpty()) || settings.host.length() > 64 ||
        settings.user.length() > 64 || settings.password.length() > 64 ||
        !whisli::net::mqtt::validTopic(settings.baseTopic) || !whisli::net::mqtt::validTopic(settings.discoveryPrefix)) {
      request.sendJson(400, "{\"message\":\"Sprawdź serwer, port (1–65535) i tematy (bez # i +, bez pustych poziomów).\"}");
      return;
    }
    settings.port = static_cast<uint16_t>(port);
    if (!whisli::net::mqtt::save(settings)) { request.sendJson(500, "{\"message\":\"Nie udało się zapisać ustawień MQTT.\"}"); return; }
    whisli::system::write(whisli::system::LogLevel::Info, "network", "MQTT settings saved");
    request.sendJson(200, "{\"message\":\"Ustawienia MQTT zapisane.\"}");
  }, StateAccess::None);
  // Tries the settings from the form (not saved yet); an empty password means the saved one.
  on("/api/v1/network/mqtt/test", Method::Post, [](Request& request) {
    auto settings = whisli::net::mqtt::load();
    const long port = request.arg("port").toInt();
    settings.host = request.arg("host");
    settings.host.trim();
    settings.user = request.arg("user");
    if (request.hasArg("password")) settings.password = request.arg("password");
    if (request.arg("clearPassword") == "true") settings.password = "";
    if (port < 1 || port > 65535 || settings.host.isEmpty() || settings.host.length() > 64 || settings.user.length() > 64 || settings.password.length() > 64) {
      request.sendJson(400, "{\"message\":\"Sprawdź serwer i port (1–65535).\"}");
      return;
    }
    settings.port = static_cast<uint16_t>(port);
    const auto result = whisli::net::mqtt::test(settings);
    whisli::system::write(result.ok ? whisli::system::LogLevel::Info : whisli::system::LogLevel::Warning, "network",
                          (String("MQTT test ") + settings.host + ": " + (result.ok ? "connected" : "failed")).c_str());
    request.sendJson(result.ok ? 200 : 502, "{\"message\":\"" + whisli::net::jsonEscape(result.message) + "\",\"elapsedMs\":" + String(result.elapsedMs) + "}");
  }, StateAccess::None);
}

void configureRoutes() {
  using whisli::net::on;
  using whisli::net::StateAccess;
  configureAuthRoutes();
  configureNetworkRoutes();
  configureBackupRoutes();
  // None of these touch lights, devices or actions, so none of them takes the state lock.
  on("/setup", Method::Get, [](Request& request) { request.send(200, "text/html; charset=utf-8", kPage, strlen(kPage)); }, StateAccess::None);
  on("/api/status", Method::Get, sendStatus, StateAccess::None);
  onWifi("scan/start", Method::Post, [](Request& request) {
    if (WiFi.scanComplete() != WIFI_SCAN_RUNNING) {
      WiFi.scanDelete();
      WiFi.scanNetworks(true, true);
    }
    request.sendJson(202, "{\"state\":\"running\"}");
  });
  onWifi("scan", Method::Get, [](Request& request) {
    const int found = WiFi.scanComplete();
    if (found == WIFI_SCAN_RUNNING || found == WIFI_SCAN_FAILED) {
      request.sendJson(200, "{\"state\":\"running\",\"networks\":[]}");
      return;
    }
    String body = "{\"state\":\"complete\",\"networks\":[";
    for (int i = 0; i < found; ++i) {
      if (i) body += ',';
      body += "{\"ssid\":\"" + jsonEscape(WiFi.SSID(i)) + "\",\"rssi\":" + String(WiFi.RSSI(i)) +
              ",\"open\":" + String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false") + "}";
    }
    request.sendJson(200, body + "]}");
  });
  onWifi("connect/status", Method::Get, [](Request& request) {
    WifiLock lock;
    if (wifiConnectionPending) {
      request.sendJson(200, "{\"state\":\"connecting\",\"message\":\"Łączenie z siecią Wi-Fi…\"}");
    } else if (wifiConnectionSucceeded) {
      request.sendJson(200, "{\"state\":\"connected\",\"message\":\"Połączono i zapisano. Adres Wi-Fi: " + WiFi.localIP().toString() + "\"}");
    } else {
      request.sendJson(200, "{\"state\":\"failed\",\"message\":\"Nie udało się połączyć z tą siecią. Dane nie zostały zapisane.\"}");
    }
  });
  onWifi("connect", Method::Post, [](Request& request) {
    const String ssid = request.arg("ssid");
    if (ssid.isEmpty()) { request.sendJson(400, "{\"message\":\"Wybierz sieć Wi-Fi.\"}"); return; }
    startWifiConnection(ssid, request.arg("password"));
    request.sendJson(202, "{\"state\":\"connecting\",\"message\":\"Łączenie z siecią Wi-Fi…\"}");
  });
  on("/api/wifi/forget", Method::Post, [](Request& request) {
    preferences.clear();
    request.sendJson(200, "{\"message\":\"Dane Wi-Fi usunięte. Urządzenie zostanie uruchomione ponownie.\"}");
    scheduleRestart(500);
  }, StateAccess::None);
  // /update is kept for the /setup page and older panels; the app panel posts to /api/v1/firmware.
  whisli::net::onStream("/update", Method::Post, handleFirmwareUpload);
  whisli::net::onStream("/api/v1/firmware", Method::Post, handleFirmwareUpload);
}

}  // namespace

// Restarts from its own short-lived task, so the delay does not depend on loop() being free.
void scheduleRestart(uint32_t delayMs) {
  static bool scheduled = false;
  if (scheduled) return;
  scheduled = true;
  xTaskCreate([](void* argument) {
    vTaskDelay(pdMS_TO_TICKS(reinterpret_cast<uintptr_t>(argument)));
    ESP.restart();
  }, "whisli-restart", 4096, reinterpret_cast<void*>(static_cast<uintptr_t>(delayMs)), 5, nullptr);
}

void begin() {
  preferences.begin(kPreferencesNamespace, false);
  startEthernet();
  startAccessPoint();
  const String savedSsid = preferences.getString(kSsidKey, "");
  if (!savedSsid.isEmpty()) startWifiConnection(savedSsid, preferences.getString(kPasswordKey, ""));
  whisli::net::auth::begin();
  whisli::net::begin();
  configureRoutes();
}

void poll() {
  whisli::net::auth::poll();
  watchAccessPointFallback();
  String ssid;
  String password;
  {
    WifiLock lock;
    if (!wifiConnectionPending) return;
    if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == pendingWifiSsid) {
      ssid = pendingWifiSsid;
      password = pendingWifiPassword;
      wifiConnectionPending = false;
      wifiConnectionSucceeded = true;
      pendingWifiPassword = "";
    } else if (millis() - wifiConnectionStartedAt >= 15000) {
      wifiConnectionPending = false;
      wifiConnectionSucceeded = false;
      pendingWifiPassword = "";
      Serial.println("[WIFI] connection timed out; saved credentials unchanged");
      whisli::system::write(whisli::system::LogLevel::Error, "network", "Wi-Fi connection timed out");
      return;
    } else {
      return;
    }
  }
  // Flash writes take a while; do them outside the lock.
  preferences.putString(kSsidKey, ssid);
  preferences.putString(kPasswordKey, password);
  Serial.printf("[WIFI] connected: %s\n", WiFi.localIP().toString().c_str());
  whisli::system::write(whisli::system::LogLevel::Info, "network", "Wi-Fi connected and credentials saved");
}

}  // namespace whisli::bootstrap
