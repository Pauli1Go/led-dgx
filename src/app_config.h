#pragma once

#include <Arduino.h>

constexpr uint8_t MAX_LED_STRIPS = 8;
constexpr uint8_t DGX_SPARKS_PER_STRIP = 2;
constexpr uint16_t LEDS_PER_STRIP = 23;
constexpr uint16_t LEDS_PER_ZONE = LEDS_PER_STRIP / DGX_SPARKS_PER_STRIP;
constexpr uint16_t APP_CONFIG_VERSION = 1;
constexpr size_t STRIP_NAME_LENGTH = 32;
constexpr size_t DGX_URL_LENGTH = 160;

struct LedStripConfig {
  char name[STRIP_NAME_LENGTH + 1];
  uint8_t dataPin;
  char dgxUrls[DGX_SPARKS_PER_STRIP][DGX_URL_LENGTH + 1];
};

struct AppConfig {
  uint16_t version;
  uint16_t fetchIntervalSeconds;
  uint8_t stripCount;
  LedStripConfig strips[MAX_LED_STRIPS];
};

void loadAppConfig(AppConfig& config);
bool saveAppConfig(const AppConfig& config);
void createDefaultAppConfig(AppConfig& config);
bool isValidAppConfig(const AppConfig& config, bool requireDgxEndpoints);
bool isValidDataPin(uint8_t pin);
bool isValidDgxUrl(const char* url);