#pragma once

#include <Arduino.h>
#include <esp_http_server.h>

#include <functional>
#include <utility>
#include <vector>

namespace whisli::net {

enum class Method : uint8_t { Get, Post };

// One HTTP exchange. Arguments come from the query string and, for form-encoded POSTs, the body.
class Request {
 public:
  explicit Request(httpd_req_t* raw) : raw_(raw) {}

  Method method() const { return raw_->method == HTTP_POST ? Method::Post : Method::Get; }
  bool hasArg(const char* name) const;
  String arg(const char* name) const;
  String header(const char* name) const;

  // Extra response headers; they must be set before send().
  void setHeader(const char* name, const String& value);
  void send(int status, const char* contentType, const char* body, size_t length);
  void sendJson(int status, const String& body) { send(status, "application/json", body.c_str(), body.length()); }
  void redirect(const char* location);
  bool responded() const { return responded_; }

  // Reads the request body in chunks (firmware upload). Returns bytes read, 0 at the end, -1 on error.
  int read(uint8_t* buffer, size_t capacity);
  size_t contentLength() const { return raw_->content_len; }

  bool parseArguments();
  // While buffering, send() only records the response; flush() puts it on the wire. Locked routes use
  // this so the network send happens after the state lock is released.
  void startBuffering() { buffering_ = true; }
  void flush();

 private:
  httpd_req_t* raw_;
  bool responded_ = false;
  bool buffering_ = false;
  int bufferedStatus_ = 0;
  String bufferedType_;
  String bufferedBody_;
  std::vector<std::pair<String, String>> args_;
  std::vector<std::pair<String, String>> headers_;  // kept alive until the response is sent
};

using Handler = std::function<void(Request&)>;

// Starts the server. Routes require a login (see whisli/net/auth.hpp) unless registered with onPublic().
void begin();

// Whether a route touches application state (lights, devices, actions). Routes that do run with the
// state lock held, which the button/light task also takes, so keep them short. Others never block it.
enum class StateAccess : uint8_t { Locked, None };

// Regular route: arguments parsed, then the handler runs.
void on(const char* uri, Method method, Handler handler, StateAccess access = StateAccess::Locked);
// Route reachable without logging in: the panel's files and the login itself. Never takes the state lock.
void onPublic(const char* uri, Method method, Handler handler);
// Streamed route (firmware upload): reads its own body with Request::read() and runs without the
// state lock, so buttons and lights keep working during a long upload.
void onStream(const char* uri, Method method, Handler handler);

// Sends a text frame to every client connected to the /api/v1/ws WebSocket. Safe from any task.
void broadcast(const String& text);
// Disconnects every WebSocket client and changes the token; pages that are still logged in reconnect.
void closeWebSockets();

}  // namespace whisli::net
