#include "serial_cli.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace {
constexpr size_t COMMAND_BUFFER_SIZE = 400;
constexpr unsigned long SESSION_DURATION_MS = 8UL * 60UL * 60UL * 1000UL;
constexpr unsigned long RESTART_DELAY_MS = 250;
constexpr uint8_t OFFLINE_UTILIZATION = 255;

AppConfig* appConfig = nullptr;
const uint8_t (*currentUtilizations)[DGX_SPARKS_PER_STRIP] = nullptr;
char commandBuffer[COMMAND_BUFFER_SIZE];
size_t commandLength = 0;
unsigned long sessionExpiresAt = 0;
unsigned long restartAt = 0;
bool authenticated = false;
bool exampleValues = false;
bool restartRequested = false;
bool refreshRequested = false;

char* trim(char* value) {
  // Commands are parsed in place to avoid heap allocation on the serial path.
  while (isspace(static_cast<unsigned char>(*value))) {
    value++;
  }

  char* end = value + strlen(value);
  while (end > value && isspace(static_cast<unsigned char>(*(end - 1)))) {
    *(--end) = '\0';
  }
  return value;
}

bool parseUnsigned(const char* value, unsigned long& parsedValue) {
  if (value == nullptr || *value == '\0' || *value == '-') {
    return false;
  }

  char* end = nullptr;
  parsedValue = strtoul(value, &end, 10);
  return end != value && *end == '\0';
}

bool copyField(char* destination, size_t capacity, const char* source) {
  if (source == nullptr || strlen(source) >= capacity) {
    return false;
  }
  strcpy(destination, source);
  return true;
}

bool sessionIsValid() {
  // Signed subtraction keeps expiry comparisons correct across millis() rollover.
  if (!authenticated || static_cast<int32_t>(millis() - sessionExpiresAt) >= 0) {
    authenticated = false;
    return false;
  }
  return true;
}

bool requireAuthentication() {
  if (sessionIsValid()) {
    return true;
  }
  Serial.println("Authentication required. Use: login <username> <password>");
  return false;
}

void printHelp() {
  Serial.println();
  Serial.println("LED DGX serial control");
  Serial.println("  login <username> <password>");
  Serial.println("  logout");
  Serial.println("  status");
  Serial.println("  interval <seconds>");
  Serial.println("  add <name>|<pin>|<dgx-1-url>|<dgx-2-url>");
  Serial.println("  set <index> <name>|<pin>|<dgx-1-url>|<dgx-2-url>");
  Serial.println("  remove <index>");
  Serial.println("  refresh");
  Serial.println("  factory-reset");
  Serial.println("  reboot");
  Serial.println();
}

void printUtilization(uint8_t utilization) {
  if (utilization == OFFLINE_UTILIZATION) {
    Serial.print("offline");
  } else if (utilization == 0) {
    Serial.print("idle");
  } else {
    Serial.printf("%u%%", utilization);
  }
}

void printStatus() {
  Serial.println();
  Serial.println("LED DGX configuration");
  Serial.printf("Source mode: %s\n", exampleValues ? "example values" : "DGX GPU API");
  Serial.printf("Fetch interval: %u seconds\n", appConfig->fetchIntervalSeconds);
  Serial.printf("LED strips: %u, active zones: %u\n", appConfig->stripCount, appConfig->stripCount * DGX_SPARKS_PER_STRIP);

  for (uint8_t stripIndex = 0; stripIndex < appConfig->stripCount; stripIndex++) {
    const LedStripConfig& strip = appConfig->strips[stripIndex];
    Serial.printf("[%u] %s, data pin %u\n", stripIndex, strip.name, strip.dataPin);
    for (uint8_t dgxIndex = 0; dgxIndex < DGX_SPARKS_PER_STRIP; dgxIndex++) {
      Serial.printf("    DGX Spark %u: %s, ", dgxIndex + 1, strip.dgxUrls[dgxIndex]);
      printUtilization(currentUtilizations[stripIndex][dgxIndex]);
      Serial.println();
    }
  }
  Serial.println();
}

bool readStripDefinition(char* definition, LedStripConfig& strip) {
  // Split the pipe-delimited definition without allocating temporary Strings.
  char* fields[4] = {};
  char* current = definition;
  for (uint8_t fieldIndex = 0; fieldIndex < 4; fieldIndex++) {
    fields[fieldIndex] = current;
    char* separator = strchr(current, '|');
    if (fieldIndex < 3) {
      if (separator == nullptr) {
        return false;
      }
      *separator = '\0';
      current = separator + 1;
    } else if (separator != nullptr) {
      return false;
    }
    fields[fieldIndex] = trim(fields[fieldIndex]);
  }

  unsigned long dataPin = 0;
  if (!parseUnsigned(fields[1], dataPin) || dataPin > 255 || !copyField(strip.name, sizeof(strip.name), fields[0]) ||
      !copyField(strip.dgxUrls[0], sizeof(strip.dgxUrls[0]), fields[2]) || !copyField(strip.dgxUrls[1], sizeof(strip.dgxUrls[1]), fields[3])) {
    return false;
  }
  strip.dataPin = static_cast<uint8_t>(dataPin);
  return true;
}

bool saveUpdatedConfiguration(const AppConfig& updatedConfig, bool restartAfterSave) {
  if (!isValidAppConfig(updatedConfig, true)) {
    Serial.println("Invalid configuration. Check names, pins, URLs, and duplicate pins.");
    return false;
  }
  if (!saveAppConfig(updatedConfig)) {
    Serial.println("Could not save configuration to ESP32 storage.");
    return false;
  }

  // Pin or strip-count changes require FastLED outputs to be rebuilt after reboot.
  *appConfig = updatedConfig;
  if (restartAfterSave) {
    restartRequested = true;
    restartAt = millis() + RESTART_DELAY_MS;
    Serial.println("Configuration saved. Restarting to apply LED output changes.");
  } else {
    Serial.println("Configuration saved.");
  }
  return true;
}

void handleLogin(char* arguments) {
  char* separator = strpbrk(arguments, " \t");
  if (separator == nullptr) {
    Serial.println("Usage: login <username> <password>");
    return;
  }
  *separator = '\0';
  const char* username = trim(arguments);
  const char* password = trim(separator + 1);
  if (strcmp(username, WEBUI_USERNAME) != 0 || strcmp(password, WEBUI_PASSWORD) != 0) {
    Serial.println("Invalid username or password.");
    return;
  }

  authenticated = true;
  sessionExpiresAt = millis() + SESSION_DURATION_MS;
  Serial.println("Authenticated. Use help for commands.");
}

void handleInterval(char* arguments) {
  unsigned long interval = 0;
  if (!parseUnsigned(arguments, interval) || interval == 0 || interval > 3600) {
    Serial.println("Usage: interval <seconds> (1-3600)");
    return;
  }

  AppConfig updatedConfig = *appConfig;
  updatedConfig.fetchIntervalSeconds = static_cast<uint16_t>(interval);
  saveUpdatedConfiguration(updatedConfig, false);
}

void handleAdd(char* arguments) {
  if (appConfig->stripCount >= MAX_LED_STRIPS) {
    Serial.printf("Maximum of %u LED strips reached.\n", MAX_LED_STRIPS);
    return;
  }

  AppConfig updatedConfig = *appConfig;
  LedStripConfig& strip = updatedConfig.strips[updatedConfig.stripCount];
  memset(&strip, 0, sizeof(strip));
  if (!readStripDefinition(arguments, strip)) {
    Serial.println("Usage: add <name>|<pin>|<dgx-1-url>|<dgx-2-url>");
    return;
  }
  updatedConfig.stripCount++;
  saveUpdatedConfiguration(updatedConfig, true);
}

void handleSet(char* arguments) {
  char* separator = strpbrk(arguments, " \t");
  if (separator == nullptr) {
    Serial.println("Usage: set <index> <name>|<pin>|<dgx-1-url>|<dgx-2-url>");
    return;
  }
  *separator = '\0';

  unsigned long index = 0;
  if (!parseUnsigned(arguments, index) || index >= appConfig->stripCount) {
    Serial.println("Invalid LED strip index.");
    return;
  }

  AppConfig updatedConfig = *appConfig;
  LedStripConfig& strip = updatedConfig.strips[index];
  memset(&strip, 0, sizeof(strip));
  if (!readStripDefinition(trim(separator + 1), strip)) {
    Serial.println("Usage: set <index> <name>|<pin>|<dgx-1-url>|<dgx-2-url>");
    return;
  }
  saveUpdatedConfiguration(updatedConfig, true);
}

void handleRemove(char* arguments) {
  unsigned long index = 0;
  if (!parseUnsigned(arguments, index) || index >= appConfig->stripCount) {
    Serial.println("Usage: remove <index>");
    return;
  }

  AppConfig updatedConfig = *appConfig;
  for (uint8_t stripIndex = index; stripIndex + 1 < updatedConfig.stripCount; stripIndex++) {
    updatedConfig.strips[stripIndex] = updatedConfig.strips[stripIndex + 1];
  }
  updatedConfig.stripCount--;
  memset(&updatedConfig.strips[updatedConfig.stripCount], 0, sizeof(LedStripConfig));
  saveUpdatedConfiguration(updatedConfig, true);
}

void handleCommand(char* input) {
  char* command = trim(input);
  if (*command == '\0') {
    return;
  }

  char* separator = strpbrk(command, " \t");
  char* arguments = const_cast<char*>("");
  if (separator != nullptr) {
    *separator = '\0';
    arguments = trim(separator + 1);
  }

  // Help and login remain available before authentication; mutations require a session.
  if (strcmp(command, "help") == 0) {
    printHelp();
    return;
  }
  if (strcmp(command, "login") == 0) {
    handleLogin(arguments);
    return;
  }
  if (strcmp(command, "logout") == 0) {
    authenticated = false;
    sessionExpiresAt = 0;
    Serial.println("Logged out.");
    return;
  }
  if (!requireAuthentication()) {
    return;
  }
  if (strcmp(command, "status") == 0) {
    printStatus();
  } else if (strcmp(command, "interval") == 0) {
    handleInterval(arguments);
  } else if (strcmp(command, "add") == 0) {
    handleAdd(arguments);
  } else if (strcmp(command, "set") == 0) {
    handleSet(arguments);
  } else if (strcmp(command, "remove") == 0) {
    handleRemove(arguments);
  } else if (strcmp(command, "refresh") == 0) {
    refreshRequested = true;
    Serial.println("Refresh requested.");
  } else if (strcmp(command, "factory-reset") == 0) {
    AppConfig updatedConfig;
    createDefaultAppConfig(updatedConfig);
    saveUpdatedConfiguration(updatedConfig, true);
  } else if (strcmp(command, "reboot") == 0) {
    restartRequested = true;
    restartAt = millis() + RESTART_DELAY_MS;
    Serial.println("Restarting.");
  } else {
    Serial.println("Unknown command. Use help.");
  }
}
}

void beginSerialCli(AppConfig& config, const uint8_t (&zoneUtilizations)[MAX_LED_STRIPS][DGX_SPARKS_PER_STRIP], bool exampleValuesEnabled) {
  appConfig = &config;
  currentUtilizations = zoneUtilizations;
  exampleValues = exampleValuesEnabled;
  Serial.println();
  Serial.println("LED DGX serial control ready. Use help.");
}

void handleSerialCli() {
  // Accumulate a line at a time and reject overflowed input before command parsing.
  while (Serial.available() > 0) {
    const char character = static_cast<char>(Serial.read());
    if (character == '\r') {
      continue;
    }
    if (character == '\n') {
      commandBuffer[commandLength] = '\0';
      handleCommand(commandBuffer);
      commandLength = 0;
      continue;
    }
    if (commandLength < COMMAND_BUFFER_SIZE - 1) {
      commandBuffer[commandLength++] = character;
    } else {
      commandLength = 0;
      Serial.println("Command too long.");
    }
  }
}

bool serialCliRestartRequested() {
  return restartRequested && static_cast<int32_t>(millis() - restartAt) >= 0;
}

bool serialCliRefreshRequested() {
  if (!refreshRequested) {
    return false;
  }
  refreshRequested = false;
  return true;
}