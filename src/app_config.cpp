#include "app_config.h"

#include <Preferences.h>
#include <cstring>

namespace {
constexpr char PREFERENCES_NAMESPACE[] = "led-dgx";
constexpr char CONFIG_KEY[] = "config";

// Stored strings must be terminated before later string operations can safely use them.
bool isTerminated(const char* value, size_t capacity) {
  return strnlen(value, capacity) < capacity;
}
}

void createDefaultAppConfig(AppConfig& config) {
  // A valid empty configuration supports first-time setup through either control surface.
  memset(&config, 0, sizeof(config));
  config.version = APP_CONFIG_VERSION;
  config.fetchIntervalSeconds = 5;
  config.stripCount = 0;
}

bool isValidDataPin(uint8_t pin) {
  switch (pin) {
    case 0 ... 5:
    case 12 ... 19:
    case 21 ... 23:
    case 25 ... 27:
    case 32:
    case 33:
      return true;
    default:
      return false;
  }
}

bool isValidDgxUrl(const char* url) {
  return url != nullptr && (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0);
}

bool isValidAppConfig(const AppConfig& config, bool requireDgxEndpoints) {
  if (config.version != APP_CONFIG_VERSION || config.fetchIntervalSeconds == 0 || config.fetchIntervalSeconds > 3600 || config.stripCount > MAX_LED_STRIPS) {
    return false;
  }

  for (uint8_t stripIndex = 0; stripIndex < config.stripCount; stripIndex++) {
    const LedStripConfig& strip = config.strips[stripIndex];
    if (!isTerminated(strip.name, sizeof(strip.name)) || strip.name[0] == '\0' || !isValidDataPin(strip.dataPin)) {
      return false;
    }

    // Multiple FastLED controllers cannot share one GPIO output.
    for (uint8_t previousIndex = 0; previousIndex < stripIndex; previousIndex++) {
      if (config.strips[previousIndex].dataPin == strip.dataPin) {
        return false;
      }
    }

    for (uint8_t dgxIndex = 0; dgxIndex < DGX_SPARKS_PER_STRIP; dgxIndex++) {
      const char* url = strip.dgxUrls[dgxIndex];
      if (!isTerminated(url, sizeof(strip.dgxUrls[dgxIndex])) ||
          (requireDgxEndpoints && url[0] != '\0' && !isValidDgxUrl(url))) {
        return false;
      }
    }
  }

  return true;
}

bool saveAppConfig(const AppConfig& config) {
  if (!isValidAppConfig(config, false)) {
    return false;
  }

  // Persist the whole fixed-size structure as one configuration payload.
  Preferences preferences;
  if (!preferences.begin(PREFERENCES_NAMESPACE, false)) {
    return false;
  }

  const size_t bytesWritten = preferences.putBytes(CONFIG_KEY, &config, sizeof(config));
  preferences.end();
  return bytesWritten == sizeof(config);
}

void loadAppConfig(AppConfig& config) {
  Preferences preferences;
  const bool storageOpened = preferences.begin(PREFERENCES_NAMESPACE, true);
  const size_t storedLength = storageOpened ? preferences.getBytesLength(CONFIG_KEY) : 0;
  const size_t bytesRead = storedLength == sizeof(config) ? preferences.getBytes(CONFIG_KEY, &config, sizeof(config)) : 0;
  if (storageOpened) {
    preferences.end();
  }

  // Missing, incompatible, or corrupt storage becomes a known valid configuration.
  if (bytesRead != sizeof(config) || !isValidAppConfig(config, false)) {
    createDefaultAppConfig(config);
    saveAppConfig(config);
  }
}