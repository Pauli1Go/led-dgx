#include "web_ui.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <WiFi.h>

namespace {
constexpr uint16_t WEB_SERVER_PORT = 80;
constexpr unsigned long SESSION_DURATION_MS = 8UL * 60UL * 60UL * 1000UL;
constexpr unsigned long WIFI_RECONNECT_INTERVAL_MS = 10000;
constexpr unsigned long RESTART_DELAY_MS = 500;
constexpr int HTTP_OK = 200;
constexpr int HTTP_BAD_REQUEST = 400;
constexpr int HTTP_UNAUTHORIZED = 401;
constexpr int HTTP_NOT_FOUND = 404;
constexpr int HTTP_INTERNAL_SERVER_ERROR = 500;

WebServer server(WEB_SERVER_PORT);
AppConfig* appConfig = nullptr;
String sessionToken;
unsigned long sessionExpiresAt = 0;
unsigned long lastWifiReconnect = 0;
unsigned long restartAt = 0;
bool restartRequested = false;
bool wifiWasConnected = false;

void sendJson(const JsonDocument& document, int statusCode = HTTP_OK) {
  String response;
  serializeJson(document, response);
  server.send(statusCode, "application/json", response);
}

void sendError(int statusCode, const char* message) {
  JsonDocument document;
  document["error"] = message;
  sendJson(document, statusCode);
}

bool sessionIsValid() {
  // Signed subtraction keeps expiry comparisons correct when millis() wraps.
  if (sessionToken.isEmpty() || static_cast<int32_t>(millis() - sessionExpiresAt) >= 0) {
    return false;
  }

  const String cookie = server.header("Cookie");
  const String expectedCookie = "session=" + sessionToken;
  const int cookiePosition = cookie.indexOf(expectedCookie);
  if (cookiePosition < 0) {
    return false;
  }

  const int tokenEnd = cookiePosition + expectedCookie.length();
  return tokenEnd == cookie.length() || cookie[tokenEnd] == ';' || cookie[tokenEnd] == ' ';
}

bool requireAuthentication() {
  if (sessionIsValid()) {
    return true;
  }
  sendError(HTTP_UNAUTHORIZED, "Authentication required.");
  return false;
}

String createSessionToken() {
  // Each session receives 128 bits from the ESP32 hardware random source.
  char token[33];
  for (uint8_t index = 0; index < 16; index++) {
    const uint8_t value = static_cast<uint8_t>(esp_random());
    snprintf(token + index * 2, 3, "%02x", value);
  }
  return String(token);
}

void sendConfiguration() {
  JsonDocument document;
  document["fetchIntervalSeconds"] = appConfig->fetchIntervalSeconds;
  document["zonesPerStrip"] = DGX_SPARKS_PER_STRIP;
  document["activeZoneCount"] = appConfig->stripCount * DGX_SPARKS_PER_STRIP;
  JsonArray strips = document["strips"].to<JsonArray>();
  for (uint8_t stripIndex = 0; stripIndex < appConfig->stripCount; stripIndex++) {
    const LedStripConfig& stripConfig = appConfig->strips[stripIndex];
    JsonObject strip = strips.add<JsonObject>();
    strip["name"] = stripConfig.name;
    strip["dataPin"] = stripConfig.dataPin;
    JsonArray dgxUrls = strip["dgxUrls"].to<JsonArray>();
    for (uint8_t dgxIndex = 0; dgxIndex < DGX_SPARKS_PER_STRIP; dgxIndex++) {
      dgxUrls.add(stripConfig.dgxUrls[dgxIndex]);
    }
  }
  sendJson(document);
}

bool readConfiguration(AppConfig& updatedConfig) {
  JsonDocument document;
  const DeserializationError error = deserializeJson(document, server.arg("plain"));
  if (error || !document["fetchIntervalSeconds"].is<uint16_t>() || !document["strips"].is<JsonArray>()) {
    sendError(HTTP_BAD_REQUEST, "Invalid configuration payload.");
    return false;
  }

  JsonArray strips = document["strips"].as<JsonArray>();
  if (strips.size() > MAX_LED_STRIPS) {
    sendError(HTTP_BAD_REQUEST, "Too many LED strips.");
    return false;
  }

  // Rebuild a zeroed config so omitted JSON fields cannot retain old data.
  memset(&updatedConfig, 0, sizeof(updatedConfig));
  updatedConfig.version = APP_CONFIG_VERSION;
  updatedConfig.fetchIntervalSeconds = document["fetchIntervalSeconds"].as<uint16_t>();
  updatedConfig.stripCount = strips.size();

  for (uint8_t stripIndex = 0; stripIndex < updatedConfig.stripCount; stripIndex++) {
    JsonObject strip = strips[stripIndex].as<JsonObject>();
    const char* name = strip["name"] | "";
    const char* dgxUrlOne = strip["dgxUrls"][0] | "";
    const char* dgxUrlTwo = strip["dgxUrls"][1] | "";
    const uint8_t dataPin = strip["dataPin"] | 255;
    snprintf(updatedConfig.strips[stripIndex].name, sizeof(updatedConfig.strips[stripIndex].name), "%s", name);
    updatedConfig.strips[stripIndex].dataPin = dataPin;
    snprintf(updatedConfig.strips[stripIndex].dgxUrls[0], sizeof(updatedConfig.strips[stripIndex].dgxUrls[0]), "%s", dgxUrlOne);
    snprintf(updatedConfig.strips[stripIndex].dgxUrls[1], sizeof(updatedConfig.strips[stripIndex].dgxUrls[1]), "%s", dgxUrlTwo);
  }

  if (!isValidAppConfig(updatedConfig, true)) {
    sendError(HTTP_BAD_REQUEST, "Configuration contains invalid values.");
    return false;
  }
  return true;
}

void handleLogin() {
  JsonDocument document;
  const DeserializationError error = deserializeJson(document, server.arg("plain"));
  const char* username = error ? "" : document["username"] | "";
  const char* password = error ? "" : document["password"] | "";
  if (strcmp(username, WEBUI_USERNAME) != 0 || strcmp(password, WEBUI_PASSWORD) != 0) {
    sendError(HTTP_UNAUTHORIZED, "Invalid username or password.");
    return;
  }

  sessionToken = createSessionToken();
  sessionExpiresAt = millis() + SESSION_DURATION_MS;
  server.sendHeader("Set-Cookie", "session=" + sessionToken + "; Path=/; HttpOnly; SameSite=Strict");
  JsonDocument response;
  response["authenticated"] = true;
  sendJson(response);
}

void handleLogout() {
  if (!requireAuthentication()) {
    return;
  }
  sessionToken = "";
  sessionExpiresAt = 0;
  server.sendHeader("Set-Cookie", "session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
  JsonDocument response;
  response["authenticated"] = false;
  sendJson(response);
}

void handleConfigUpdate() {
  if (!requireAuthentication()) {
    return;
  }

  AppConfig updatedConfig;
  if (!readConfiguration(updatedConfig)) {
    return;
  }
  if (!saveAppConfig(updatedConfig)) {
    sendError(HTTP_INTERNAL_SERVER_ERROR, "Could not save configuration.");
    return;
  }

  // GPIO controller changes take effect after this response is sent and the device restarts.
  *appConfig = updatedConfig;
  restartRequested = true;
  restartAt = millis() + RESTART_DELAY_MS;
  JsonDocument response;
  response["saved"] = true;
  response["restarting"] = true;
  sendJson(response);
}

void serveIndex() {
  File file = LittleFS.open("/index.html", "r");
  if (!file) {
    sendError(HTTP_INTERNAL_SERVER_ERROR, "Web UI was not uploaded.");
    return;
  }
  server.streamFile(file, "text/html");
  file.close();
}

void configureRoutes() {
  // Cookie collection is opt-in for WebServer and is required by sessionIsValid().
  const char* headers[] = {"Cookie"};
  server.collectHeaders(headers, 1);

  server.on("/", HTTP_GET, serveIndex);
  server.on("/api/login", HTTP_POST, handleLogin);
  server.on("/api/session", HTTP_GET, []() {
    if (!requireAuthentication()) {
      return;
    }
    JsonDocument response;
    response["authenticated"] = true;
    sendJson(response);
  });
  server.on("/api/logout", HTTP_POST, handleLogout);
  server.on("/api/config", HTTP_GET, []() {
    if (requireAuthentication()) {
      sendConfiguration();
    }
  });
  server.on("/api/config", HTTP_POST, handleConfigUpdate);
  server.on("/api/status", HTTP_GET, []() {
    if (!requireAuthentication()) {
      return;
    }
    JsonDocument response;
    response["wifiConnected"] = WiFi.status() == WL_CONNECTED;
    response["ipAddress"] = WiFi.localIP().toString();
    response["activeZoneCount"] = appConfig->stripCount * DGX_SPARKS_PER_STRIP;
    sendJson(response);
  });
  server.onNotFound([]() {
    if (server.uri().startsWith("/api/")) {
      sendError(HTTP_NOT_FOUND, "API endpoint not found.");
    } else {
      server.send(HTTP_NOT_FOUND, "text/plain", "Not found.");
    }
  });
}
}

void beginWebUi(AppConfig& config) {
  appConfig = &config;
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
  }

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  configureRoutes();
  server.begin();
  Serial.println("Web server started");
}

void handleWebUi() {
  server.handleClient();
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiWasConnected) {
      Serial.printf("WiFi connected. Web UI: http://%s/\n", WiFi.localIP().toString().c_str());
      wifiWasConnected = true;
    }
    return;
  }

  wifiWasConnected = false;
  // Avoid repeated connection attempts while the access point is unavailable.
  if (millis() - lastWifiReconnect < WIFI_RECONNECT_INTERVAL_MS) {
    return;
  }
  lastWifiReconnect = millis();
  WiFi.reconnect();
}

bool webUiRestartRequested() {
  return restartRequested && static_cast<int32_t>(millis() - restartAt) >= 0;
}