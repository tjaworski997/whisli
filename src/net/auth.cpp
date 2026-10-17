#include "whisli/net/auth.hpp"

#include <Preferences.h>
#include <esp_random.h>
#include <mbedtls/base64.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/sha256.h>

#include <ctime>

#include "whisli/net/http_server.hpp"
#include "whisli/system/log.hpp"

namespace whisli::net::auth {
namespace {

using whisli::system::LogLevel;

constexpr char kNamespace[] = "whisli-auth";
constexpr char kSaltKey[] = "salt";
constexpr char kHashKey[] = "hash";
constexpr char kSessionsKey[] = "sessions";
constexpr uint32_t kIterations = 4096;
constexpr char kCookieName[] = "whisli_session";
constexpr uint32_t kRememberSeconds = 30UL * 24 * 3600;
constexpr uint32_t kSessionSeconds = 24UL * 3600;  // without "remember me"
constexpr int kBootButtonPin = 0;
constexpr uint32_t kResetHoldMs = 10000;
constexpr size_t kMaxSessions = 8;

using Digest = uint8_t[32];

// Sessions survive restarts (an OTA update must not log the panel out). Only a hash of each token is
// kept. `expiresAt` is wall-clock time from NTP; 0 means the clock was unknown at login, and then the
// browser's cookie lifetime is the limit.
struct Session {
  Digest token;
  uint32_t expiresAt;
  bool used;
};

Preferences preferences;
SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
struct Lock {
  Lock() { xSemaphoreTake(mutex, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(mutex); }
};

uint8_t salt[16] = {};
Digest passwordHash = {};
bool customPassword = false;
Session sessions[kMaxSessions] = {};

// The last Basic header that matched and the last one that did not, so a client repeating the same
// header costs neither a PBKDF2 run nor another failed attempt.
Digest basicAccepted = {};
Digest basicRejected = {};

void sha256(const String& text, Digest output) {
  mbedtls_sha256(reinterpret_cast<const unsigned char*>(text.c_str()), text.length(), output, 0);
}

bool equal(const uint8_t* left, const uint8_t* right, size_t length) {
  uint8_t difference = 0;
  for (size_t index = 0; index < length; ++index) difference |= left[index] ^ right[index];
  return difference == 0;
}

bool empty(const Digest digest) {
  static const Digest zero = {};
  return equal(digest, zero, sizeof(Digest));
}

void derive(const String& password, const uint8_t* saltBytes, Digest output) {
  mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, reinterpret_cast<const unsigned char*>(password.c_str()),
                                password.length(), saltBytes, sizeof(salt), kIterations, sizeof(Digest), output);
}

bool passwordMatches(const String& password) {
  if (!customPassword) {
    const String expected(kDefaultPassword);
    return password.length() == expected.length() &&
           equal(reinterpret_cast<const uint8_t*>(password.c_str()), reinterpret_cast<const uint8_t*>(expected.c_str()), expected.length());
  }
  Digest candidate;
  derive(password, salt, candidate);
  return equal(candidate, passwordHash, sizeof(Digest));
}

uint32_t now() {
  const time_t seconds = time(nullptr);
  return seconds > 1700000000 ? static_cast<uint32_t>(seconds) : 0;  // before NTP the clock starts at 1970
}

void saveSessions() { preferences.putBytes(kSessionsKey, sessions, sizeof(sessions)); }

void endAllSessions(int keep = -1) {
  for (size_t index = 0; index < kMaxSessions; ++index) if (static_cast<int>(index) != keep) sessions[index] = {};
  memset(basicAccepted, 0, sizeof(Digest));
  memset(basicRejected, 0, sizeof(Digest));
  saveSessions();
  whisli::net::closeWebSockets();
}

String cookieValue(const String& header) {
  const String prefix = String(kCookieName) + "=";
  int start = header.startsWith(prefix) ? 0 : header.indexOf("; " + prefix);
  if (start < 0) return String();
  if (start > 0) start += 2;
  start += prefix.length();
  const int end = header.indexOf(';', start);
  return header.substring(start, end < 0 ? header.length() : end);
}

int findSession(const String& cookie) {
  const String token = cookieValue(cookie);
  if (token.length() != 64) return -1;
  Digest digest;
  sha256(token, digest);
  const uint32_t time = now();
  for (size_t index = 0; index < kMaxSessions; ++index) {
    Session& session = sessions[index];
    if (!session.used || !equal(session.token, digest, sizeof(Digest))) continue;
    if (session.expiresAt && time && time > session.expiresAt) {
      session = {};
      saveSessions();
      return -1;
    }
    return index;
  }
  return -1;
}

void recordFailure(const char* how) {
  whisli::system::write(LogLevel::Warning, "system", (String("Wrong password (") + how + ")").c_str());
}

bool attempt(const String& password, const char* how, LoginResult& result) {
  if (passwordMatches(password)) return true;
  recordFailure(how);
  result.status = 401;
  return false;
}

bool basicAuthorized(const String& header) {
  if (!header.startsWith("Basic ")) return false;
  Digest digest;
  sha256(header, digest);
  if (!empty(basicAccepted) && equal(digest, basicAccepted, sizeof(Digest))) return true;
  if (!empty(basicRejected) && equal(digest, basicRejected, sizeof(Digest))) return false;
  unsigned char decoded[160] = {};
  size_t length = 0;
  const String encoded = header.substring(6);
  if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &length, reinterpret_cast<const unsigned char*>(encoded.c_str()), encoded.length()) != 0) return false;
  const String credentials(reinterpret_cast<const char*>(decoded));
  const int colon = credentials.indexOf(':');
  if (colon < 0) return false;
  if (passwordMatches(credentials.substring(colon + 1))) {
    memcpy(basicAccepted, digest, sizeof(Digest));
    return true;
  }
  memcpy(basicRejected, digest, sizeof(Digest));
  recordFailure("HTTP Basic");
  return false;
}

String sessionCookie(const String& token, bool remember) {
  String cookie = String(kCookieName) + "=" + token + "; Path=/; HttpOnly; SameSite=Strict";
  if (remember) cookie += "; Max-Age=" + String(kRememberSeconds);
  return cookie;
}

void resetToDefault() {
  Lock lock;
  preferences.remove(kSaltKey);
  preferences.remove(kHashKey);
  customPassword = false;
  endAllSessions();
}

}  // namespace

void begin() {
  preferences.begin(kNamespace, false);
  customPassword = preferences.getBytes(kSaltKey, salt, sizeof(salt)) == sizeof(salt) &&
                   preferences.getBytes(kHashKey, passwordHash, sizeof(Digest)) == sizeof(Digest);
  if (preferences.getBytesLength(kSessionsKey) == sizeof(sessions)) preferences.getBytes(kSessionsKey, sessions, sizeof(sessions));
  pinMode(kBootButtonPin, INPUT_PULLUP);
  // Wall-clock time for session expiry. Without internet access sessions simply follow the cookie.
  configTime(0, 0, "pool.ntp.org", "time.google.com");
}

void poll() {
  static uint32_t pressedAt = 0;
  static bool done = false;
  if (digitalRead(kBootButtonPin) == HIGH) {
    pressedAt = 0;
    done = false;
    return;
  }
  if (!pressedAt) pressedAt = millis() | 1;
  if (done || millis() - pressedAt < kResetHoldMs) return;
  done = true;
  resetToDefault();
  Serial.println("[AUTH] password reset to default with the BOOT button");
  whisli::system::write(LogLevel::Warning, "system", "Password reset to default with the BOOT button");
}

bool authorized(const String& cookie, const String& authorization) {
  Lock lock;
  return findSession(cookie) >= 0 || basicAuthorized(authorization);
}

LoginResult login(const String& password, bool remember) {
  Lock lock;
  LoginResult result;
  if (!attempt(password, "panel", result)) return result;
  char token[65];
  for (uint8_t index = 0; index < 8; ++index) snprintf(token + index * 8, 9, "%08lx", static_cast<unsigned long>(esp_random()));
  // A free slot, or else the one that expires first.
  size_t slot = 0;
  for (size_t index = 0; index < kMaxSessions; ++index) {
    if (!sessions[index].used) { slot = index; break; }
    if (sessions[index].expiresAt < sessions[slot].expiresAt) slot = index;
  }
  const uint32_t time = now();
  Session& session = sessions[slot];
  sha256(token, session.token);
  session.expiresAt = time ? time + (remember ? kRememberSeconds : kSessionSeconds) : 0;
  session.used = true;
  saveSessions();
  result.status = 200;
  result.setCookie = sessionCookie(token, remember);
  whisli::system::write(LogLevel::Info, "system", remember ? "Panel login (remembered for 30 days)" : "Panel login");
  return result;
}

String logout(const String& cookie) {
  Lock lock;
  const int index = findSession(cookie);
  if (index >= 0) {
    sessions[index] = {};
    saveSessions();
    whisli::net::closeWebSockets();
  }
  return String(kCookieName) + "=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0";
}

LoginResult changePassword(const String& cookie, const String& current, const String& next) {
  Lock lock;
  LoginResult result;
  if (!attempt(current, "password change", result)) return result;
  if (next.length() < kMinPasswordLength || next.length() > kMaxPasswordLength) {
    result.status = 400;
    return result;
  }
  customPassword = next != kDefaultPassword;  // setting the default again counts as "default" in the panel
  if (customPassword) {
    esp_fill_random(salt, sizeof(salt));
    derive(next, salt, passwordHash);
    preferences.putBytes(kSaltKey, salt, sizeof(salt));
    preferences.putBytes(kHashKey, passwordHash, sizeof(Digest));
  } else {
    preferences.remove(kSaltKey);
    preferences.remove(kHashKey);
  }
  endAllSessions(findSession(cookie));
  result.status = 200;
  whisli::system::write(LogLevel::Info, "system", "Password changed; other sessions ended");
  return result;
}

bool usingDefaultPassword() {
  Lock lock;
  return !customPassword;
}

}  // namespace whisli::net::auth
