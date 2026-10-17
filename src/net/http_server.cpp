#include "whisli/net/http_server.hpp"

#include <esp_random.h>
#include <sys/socket.h>

#include <memory>

#include "whisli/net/auth.hpp"
#include "whisli/system/log.hpp"
#include "whisli/system/state_lock.hpp"
#include "whisli/system/task_priorities.hpp"

namespace whisli::net {
namespace {

constexpr size_t kMaxFormBody = 4096;
constexpr size_t kMaxClients = 10;  // + 3 internal sockets, within CONFIG_LWIP_MAX_SOCKETS (16)
constexpr char kRealm[] = "Basic realm=\"Login Required\"";

httpd_handle_t server = nullptr;

// WebSocket authentication. This SDK build gives the handler no hook during the upgrade request, so
// the Basic credentials cannot be checked there. Instead an authenticated GET /api/v1/ws/token
// returns a secret that the client sends as its first message; only sockets that sent it receive
// pushes. An entry is removed when its socket closes, so a reused descriptor starts unauthorized.
// The token changes whenever a session ends, so a logged-out page cannot listen on.
char webSocketToken[33] = {};
portMUX_TYPE webSocketMux = portMUX_INITIALIZER_UNLOCKED;

void newWebSocketToken() {
  char token[33];
  for (uint8_t index = 0; index < 4; ++index) snprintf(token + index * 8, 9, "%08lx", static_cast<unsigned long>(esp_random()));
  portENTER_CRITICAL(&webSocketMux);
  memcpy(webSocketToken, token, sizeof(token));
  portEXIT_CRITICAL(&webSocketMux);
}

String currentWebSocketToken() {
  char token[33];
  portENTER_CRITICAL(&webSocketMux);
  memcpy(token, webSocketToken, sizeof(token));
  portEXIT_CRITICAL(&webSocketMux);
  return String(token);
}
int authorizedSockets[kMaxClients];
size_t authorizedCount = 0;

void addAuthorizedSocket(int fd) {
  portENTER_CRITICAL(&webSocketMux);
  if (authorizedCount < kMaxClients) authorizedSockets[authorizedCount++] = fd;
  portEXIT_CRITICAL(&webSocketMux);
}

void removeAuthorizedSocket(int fd) {
  portENTER_CRITICAL(&webSocketMux);
  for (size_t index = 0; index < authorizedCount; ++index) {
    if (authorizedSockets[index] != fd) continue;
    authorizedSockets[index] = authorizedSockets[--authorizedCount];
    break;
  }
  portEXIT_CRITICAL(&webSocketMux);
}

bool isAuthorizedSocket(int fd) {
  portENTER_CRITICAL(&webSocketMux);
  bool found = false;
  for (size_t index = 0; index < authorizedCount && !found; ++index) found = authorizedSockets[index] == fd;
  portEXIT_CRITICAL(&webSocketMux);
  return found;
}

void closeSocket(httpd_handle_t, int fd) {
  removeAuthorizedSocket(fd);
  close(fd);
}

struct Route {
  Handler handler;
  bool stream;
  bool locked;
  bool open;  // reachable without logging in
};

const char* statusLine(int status) {
  switch (status) {
    case 200: return "200 OK";
    case 201: return "201 Created";
    case 202: return "202 Accepted";
    case 302: return "302 Found";
    case 400: return "400 Bad Request";
    case 401: return "401 Unauthorized";
    case 404: return "404 Not Found";
    case 409: return "409 Conflict";
    case 413: return "413 Payload Too Large";
    case 503: return "503 Service Unavailable";
    default: return "500 Internal Server Error";
  }
}

int hexValue(char character) {
  if (character >= '0' && character <= '9') return character - '0';
  if (character >= 'a' && character <= 'f') return character - 'a' + 10;
  if (character >= 'A' && character <= 'F') return character - 'A' + 10;
  return -1;
}

String urlDecode(const char* value, size_t length) {
  String decoded;
  decoded.reserve(length);
  for (size_t index = 0; index < length; ++index) {
    const char character = value[index];
    if (character == '+') {
      decoded += ' ';
    } else if (character == '%' && index + 2 < length && hexValue(value[index + 1]) >= 0 && hexValue(value[index + 2]) >= 0) {
      decoded += static_cast<char>(hexValue(value[index + 1]) * 16 + hexValue(value[index + 2]));
      index += 2;
    } else {
      decoded += character;
    }
  }
  return decoded;
}

// Parses "a=1&b=two" into decoded key/value pairs.
void parsePairs(const char* data, size_t length, std::vector<std::pair<String, String>>& output) {
  size_t start = 0;
  while (start < length) {
    size_t end = start;
    while (end < length && data[end] != '&') ++end;
    size_t equals = start;
    while (equals < end && data[equals] != '=') ++equals;
    if (end > start) {
      output.emplace_back(urlDecode(data + start, equals - start),
                          equals < end ? urlDecode(data + equals + 1, end - equals - 1) : String());
    }
    start = end + 1;
  }
}

String requestHeader(httpd_req_t* req, const char* name) {
  const size_t length = httpd_req_get_hdr_value_len(req, name);
  if (length == 0) return String();
  std::unique_ptr<char[]> value(new char[length + 1]);
  if (httpd_req_get_hdr_value_str(req, name, value.get(), length + 1) != ESP_OK) return String();
  return String(value.get());
}

bool authorized(httpd_req_t* req) { return auth::authorized(requestHeader(req, "Cookie"), requestHeader(req, "Authorization")); }

// The API answers 401 with JSON, so the panel shows its own login screen. Other pages (the /setup
// fallback) still ask the browser for HTTP Basic.
void requestAuthentication(httpd_req_t* req) {
  httpd_resp_set_status(req, statusLine(401));
  if (strncmp(req->uri, "/api/", 5) == 0) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"message\":\"Zaloguj się.\"}");
    return;
  }
  httpd_resp_set_hdr(req, "WWW-Authenticate", kRealm);
  httpd_resp_send(req, nullptr, 0);
}

esp_err_t dispatch(httpd_req_t* req) {
  const auto* route = static_cast<const Route*>(req->user_ctx);
  if (!route->open && !authorized(req)) {
    requestAuthentication(req);
    return ESP_OK;
  }
  Request request(req);
  if (route->stream) {
    route->handler(request);
  } else if (!request.parseArguments()) {
    request.sendJson(413, "{\"message\":\"Nieprawidłowe lub zbyt duże dane formularza.\"}");
  } else if (route->locked) {
    request.startBuffering();
    {
      whisli::system::StateLock lock;
      route->handler(request);
    }
    // The response goes on the wire below, outside the lock: buttons and lights never wait for a slow client.
  } else {
    route->handler(request);
  }
  if (!request.responded()) request.sendJson(500, "{\"message\":\"Brak odpowiedzi serwera.\"}");
  request.flush();
  return ESP_OK;
}

esp_err_t notFound(httpd_req_t* req, httpd_err_code_t) {
  httpd_resp_set_status(req, statusLine(404));
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"message\":\"Nie znaleziono zasobu.\"}");
  return ESP_OK;
}

esp_err_t webSocketHandler(httpd_req_t* req) {
  if (req->method == HTTP_GET) return ESP_OK;
  httpd_ws_frame_t frame = {};
  if (httpd_ws_recv_frame(req, &frame, 0) != ESP_OK) return ESP_FAIL;
  if (frame.len == 0 || frame.len > 128) return ESP_OK;
  std::unique_ptr<uint8_t[]> payload(new uint8_t[frame.len + 1]());
  frame.payload = payload.get();
  if (httpd_ws_recv_frame(req, &frame, frame.len) != ESP_OK) return ESP_FAIL;
  // The only message a client sends is the token; anything else closes the socket.
  const int fd = httpd_req_to_sockfd(req);
  if (frame.type != HTTPD_WS_TYPE_TEXT || currentWebSocketToken() != reinterpret_cast<const char*>(payload.get())) {
    httpd_sess_trigger_close(server, fd);
    return ESP_OK;
  }
  if (!isAuthorizedSocket(fd)) addAuthorizedSocket(fd);
  return ESP_OK;
}

void broadcastWork(void* argument) {
  std::unique_ptr<String> text(static_cast<String*>(argument));
  size_t count = kMaxClients;
  int clients[kMaxClients];
  if (httpd_get_client_list(server, &count, clients) != ESP_OK) return;
  httpd_ws_frame_t frame = {};
  frame.final = true;
  frame.type = HTTPD_WS_TYPE_TEXT;
  frame.payload = reinterpret_cast<uint8_t*>(const_cast<char*>(text->c_str()));
  frame.len = text->length();
  for (size_t index = 0; index < count; ++index) {
    if (isAuthorizedSocket(clients[index]) && httpd_ws_get_fd_info(server, clients[index]) == HTTPD_WS_CLIENT_WEBSOCKET) {
      httpd_ws_send_frame_async(server, clients[index], &frame);
    }
  }
}

void registerRoute(const char* uri, Method method, Handler handler, bool stream, bool locked, bool open = false) {
  if (!server) return;
  httpd_uri_t route = {};
  route.uri = strdup(uri);  // httpd keeps the pointer; routes live for the whole runtime
  route.method = method == Method::Post ? HTTP_POST : HTTP_GET;
  route.handler = dispatch;
  route.user_ctx = new Route{std::move(handler), stream, locked, open};
  if (httpd_register_uri_handler(server, &route) != ESP_OK) {
    Serial.printf("[HTTP] could not register %s\n", uri);
  }
}

}  // namespace

bool Request::hasArg(const char* name) const {
  for (const auto& entry : args_) if (entry.first == name) return true;
  return false;
}

String Request::arg(const char* name) const {
  for (const auto& entry : args_) if (entry.first == name) return entry.second;
  return String();
}

String Request::header(const char* name) const { return requestHeader(raw_, name); }

void Request::setHeader(const char* name, const String& value) { headers_.emplace_back(name, value); }

void Request::send(int status, const char* contentType, const char* body, size_t length) {
  if (buffering_) {
    bufferedStatus_ = status;
    bufferedType_ = contentType;
    bufferedBody_ = String();
    bufferedBody_.concat(body, length);
    responded_ = true;
    return;
  }
  httpd_resp_set_status(raw_, statusLine(status));
  httpd_resp_set_type(raw_, contentType);
  for (const auto& entry : headers_) httpd_resp_set_hdr(raw_, entry.first.c_str(), entry.second.c_str());
  httpd_resp_send(raw_, body, length);
  responded_ = true;
}

void Request::flush() {
  if (!buffering_ || bufferedStatus_ == 0) return;
  buffering_ = false;
  send(bufferedStatus_, bufferedType_.c_str(), bufferedBody_.c_str(), bufferedBody_.length());
}

void Request::redirect(const char* location) {
  setHeader("Location", location);
  send(302, "text/plain", "", 0);
}

int Request::read(uint8_t* buffer, size_t capacity) {
  // Flash erases during OTA can stall the sender for a while; retry timeouts instead of failing.
  for (uint8_t attempt = 0; attempt < 5; ++attempt) {
    const int received = httpd_req_recv(raw_, reinterpret_cast<char*>(buffer), capacity);
    if (received != HTTPD_SOCK_ERR_TIMEOUT) return received < 0 ? -1 : received;
  }
  return -1;
}

bool Request::parseArguments() {
  const size_t queryLength = httpd_req_get_url_query_len(raw_);
  if (queryLength > 0) {
    std::unique_ptr<char[]> query(new char[queryLength + 1]);
    if (httpd_req_get_url_query_str(raw_, query.get(), queryLength + 1) == ESP_OK) parsePairs(query.get(), queryLength, args_);
  }
  if (raw_->method != HTTP_POST || raw_->content_len == 0) return true;
  if (!header("Content-Type").startsWith("application/x-www-form-urlencoded")) return true;  // body ignored
  if (raw_->content_len > kMaxFormBody) return false;
  std::unique_ptr<char[]> body(new char[raw_->content_len]);
  size_t received = 0;
  while (received < raw_->content_len) {
    const int chunk = read(reinterpret_cast<uint8_t*>(body.get() + received), raw_->content_len - received);
    if (chunk <= 0) return false;
    received += chunk;
  }
  parsePairs(body.get(), received, args_);
  return true;
}

void begin() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.max_uri_handlers = 96;
  config.max_open_sockets = kMaxClients;
  config.lru_purge_enable = true;  // browsers keep idle connections; drop the oldest when full
  config.stack_size = 12288;       // handlers run here and build their JSON responses
  config.core_id = 1;              // core 0 belongs to Wi-Fi, Ethernet and lwIP
  config.task_priority = whisli::system::kHttpTaskPriority;  // below buttons, lights, Modbus and DMX
  config.recv_wait_timeout = 10;
  config.send_wait_timeout = 10;
  config.close_fn = closeSocket;
  if (httpd_start(&server, &config) != ESP_OK) {
    server = nullptr;
    Serial.println("[HTTP] server failed to start");
    whisli::system::write(whisli::system::LogLevel::Error, "system", "HTTP server failed to start");
    return;
  }
  httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, notFound);

  httpd_uri_t webSocket = {};
  webSocket.uri = "/api/v1/ws";
  webSocket.method = HTTP_GET;
  webSocket.handler = webSocketHandler;
  webSocket.is_websocket = true;
  httpd_register_uri_handler(server, &webSocket);
  newWebSocketToken();
  on("/api/v1/ws/token", Method::Get, [](Request& request) { request.sendJson(200, "{\"token\":\"" + currentWebSocketToken() + "\"}"); }, StateAccess::None);
  Serial.println("[HTTP] server started on port 80");
}

void on(const char* uri, Method method, Handler handler, StateAccess access) {
  registerRoute(uri, method, std::move(handler), false, access == StateAccess::Locked);
}

void onPublic(const char* uri, Method method, Handler handler) { registerRoute(uri, method, std::move(handler), false, false, true); }

void onStream(const char* uri, Method method, Handler handler) { registerRoute(uri, method, std::move(handler), true, false); }

void closeWebSockets() {
  newWebSocketToken();
  if (!server) return;
  int sockets[kMaxClients];
  portENTER_CRITICAL(&webSocketMux);
  const size_t count = authorizedCount;
  memcpy(sockets, authorizedSockets, count * sizeof(int));
  authorizedCount = 0;
  portEXIT_CRITICAL(&webSocketMux);
  for (size_t index = 0; index < count; ++index) httpd_sess_trigger_close(server, sockets[index]);
}

void broadcast(const String& text) {
  if (!server) return;
  auto* copy = new String(text);
  if (httpd_queue_work(server, broadcastWork, copy) != ESP_OK) delete copy;
}

}  // namespace whisli::net
