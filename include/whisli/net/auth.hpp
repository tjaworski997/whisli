#pragma once

#include <Arduino.h>

namespace whisli::net::auth {

// Single-user access to the controller: one password, no user name.
//
// The panel logs in with POST /api/v1/auth/login and then carries a session cookie (HttpOnly,
// SameSite=Strict). Scripts and curl may keep using HTTP Basic with any user name and the same
// password. The password is stored as a salted PBKDF2 hash; until it is changed the default one
// applies. Holding the BOOT button for 10 s brings the default back and ends every session.

constexpr char kDefaultPassword[] = "whisli_gogo";
constexpr size_t kMinPasswordLength = 1;  // any password, just not an empty one
constexpr size_t kMaxPasswordLength = 64;

void begin();
// Watches the BOOT button; called from loop().
void poll();

// Whether a request with these Cookie and Authorization headers may proceed.
bool authorized(const String& cookie, const String& authorization);

struct LoginResult {
  int status = 401;            // 200 logged in, 401 wrong password
  String setCookie;            // Set-Cookie value on success
};
LoginResult login(const String& password, bool remember);

// Ends the session named in `cookie`; returns the Set-Cookie value that removes it.
String logout(const String& cookie);

// 200 changed, 400 new password not accepted, 401 wrong current password.
// Every other session ends; the one in `cookie` stays.
LoginResult changePassword(const String& cookie, const String& current, const String& next);

bool usingDefaultPassword();

}  // namespace whisli::net::auth
