#define RUN_WITH_EXAMPLE_VALUES false

#include <Arduino.h>
#include <HTTPClient.h>
#include <FastLED.h>
#include <WiFi.h>
#include <cstdlib>
#include <cstring>

#include "app_config.h"
#include "serial_cli.h"
#include "web_ui.h"

constexpr uint8_t OFFLINE_UTILIZATION = 255;
constexpr uint8_t IDLE_UTILIZATION_MAX = 5;
constexpr uint16_t HTTP_TIMEOUT_MS = 4000;
constexpr char GPU_UTILIZATION_METRIC[] = "DCGM_FI_DEV_GPU_UTIL";
constexpr uint16_t RUNNING_BASE_SPEED = 60;
constexpr uint16_t RUNNING_TOP_SPEED = 120;
constexpr uint32_t ANIMATION_PHASES_PER_CYCLE = 65536UL;
constexpr uint32_t MILLISECONDS_PER_MINUTE = 60000UL;

// Runtime state for each configured strip and its two DGX Spark zones.
AppConfig appConfig;
CRGB stripPixels[MAX_LED_STRIPS][LEDS_PER_STRIP] = {};
bool configuredOutputs[MAX_LED_STRIPS] = {};
uint8_t zoneUtilizations[MAX_LED_STRIPS][DGX_SPARKS_PER_STRIP] = {};
uint16_t runningZonePhases[MAX_LED_STRIPS][DGX_SPARKS_PER_STRIP] = {};
uint8_t configuredStripCount = 0;
unsigned long lastStatusRefresh = 0;
unsigned long lastAnimationPhaseUpdate = 0;
bool statusRefreshDue = true;
portMUX_TYPE zoneUtilizationsMux = portMUX_INITIALIZER_UNLOCKED;

bool configureOutputForPin(uint8_t dataPin, CRGB* pixels) {
    // FastLED requires compile-time pin values, so supported GPIOs are enumerated here.
    switch (dataPin) {
        case 0: FastLED.addLeds<WS2815, 0, GRB>(pixels, LEDS_PER_STRIP); return true;
        case 1: FastLED.addLeds<WS2815, 1, GRB>(pixels, LEDS_PER_STRIP); return true;
        case 2: FastLED.addLeds<WS2815, 2, GRB>(pixels, LEDS_PER_STRIP); return true;
        case 3: FastLED.addLeds<WS2815, 3, GRB>(pixels, LEDS_PER_STRIP); return true;
        case 4: FastLED.addLeds<WS2815, 4, GRB>(pixels, LEDS_PER_STRIP); return true;
        case 5: FastLED.addLeds<WS2815, 5, GRB>(pixels, LEDS_PER_STRIP); return true;
        case 12: FastLED.addLeds<WS2815, 12, GRB>(pixels, LEDS_PER_STRIP); return true;
        case 13: FastLED.addLeds<WS2815, 13, GRB>(pixels, LEDS_PER_STRIP); return true;
        case 14: FastLED.addLeds<WS2815, 14, GRB>(pixels, LEDS_PER_STRIP); return true;
        case 15: FastLED.addLeds<WS2815, 15, GRB>(pixels, LEDS_PER_STRIP); return true;
        case 16: FastLED.addLeds<WS2815, 16, GRB>(pixels, LEDS_PER_STRIP); return true;
        case 17: FastLED.addLeds<WS2815, 17, GRB>(pixels, LEDS_PER_STRIP); return true;
        case 18: FastLED.addLeds<WS2815, 18, GRB>(pixels, LEDS_PER_STRIP); return true;
        case 19: FastLED.addLeds<WS2815, 19, GRB>(pixels, LEDS_PER_STRIP); return true;
        case 21: FastLED.addLeds<WS2815, 21, GRB>(pixels, LEDS_PER_STRIP); return true;
        case 22: FastLED.addLeds<WS2815, 22, GRB>(pixels, LEDS_PER_STRIP); return true;
        case 23: FastLED.addLeds<WS2815, 23, GRB>(pixels, LEDS_PER_STRIP); return true;
        case 25: FastLED.addLeds<WS2815, 25, GRB>(pixels, LEDS_PER_STRIP); return true;
        case 26: FastLED.addLeds<WS2815, 26, GRB>(pixels, LEDS_PER_STRIP); return true;
        case 27: FastLED.addLeds<WS2815, 27, GRB>(pixels, LEDS_PER_STRIP); return true;
        case 32: FastLED.addLeds<WS2815, 32, GRB>(pixels, LEDS_PER_STRIP); return true;
        case 33: FastLED.addLeds<WS2815, 33, GRB>(pixels, LEDS_PER_STRIP); return true;
        default: return false;
    }
}

void beginStripOutputs() {
    for (uint8_t stripIndex = 0; stripIndex < appConfig.stripCount; stripIndex++) {
        configuredOutputs[stripIndex] = configureOutputForPin(appConfig.strips[stripIndex].dataPin, stripPixels[stripIndex]);
        if (!configuredOutputs[stripIndex]) {
            Serial.printf("Could not initialize LED output %u\n", stripIndex);
        }
    }
    FastLED.clear(true);
    configuredStripCount = appConfig.stripCount;
}

uint16_t zoneOffset(uint8_t zoneIndex) {
    const bool hasCenterLed = LEDS_PER_STRIP % 2 != 0;
    return (zoneIndex * LEDS_PER_ZONE) + (hasCenterLed && zoneIndex > 0 ? 1 : 0);
}

void fillZone(CRGB* pixels, uint8_t zoneIndex, const CRGB& color) {
    const uint16_t offset = zoneOffset(zoneIndex);
    for (uint16_t pixelIndex = 0; pixelIndex < LEDS_PER_ZONE; pixelIndex++) {
        pixels[offset + pixelIndex] = color;
    }
}

void clearStripCenterLed(CRGB* pixels) {
    if (LEDS_PER_STRIP % 2 != 0) {
        pixels[LEDS_PER_STRIP / 2] = CRGB::Black;
    }
}

void renderIdleZone(CRGB* pixels, uint8_t zoneIndex) {
    const uint8_t idlePulse = beatsin8(20, 5, 255);
    fillZone(pixels, zoneIndex, CRGB::Blue);
    const uint16_t offset = zoneOffset(zoneIndex);
    for (uint16_t pixelIndex = 0; pixelIndex < LEDS_PER_ZONE; pixelIndex++) {
        pixels[offset + pixelIndex].nscale8_video(idlePulse);
    }
}

uint16_t runningZoneSpeed(uint8_t utilization) {
    return static_cast<uint16_t>(map(constrain(utilization, 1, 100), 1, 100, RUNNING_BASE_SPEED, RUNNING_TOP_SPEED));
}

void updateRunningZonePhases(unsigned long now, const uint8_t utilizations[MAX_LED_STRIPS][DGX_SPARKS_PER_STRIP]) {
    // Advance only active zones; the uint16_t phase intentionally wraps every cycle.
    const unsigned long elapsedMs = now - lastAnimationPhaseUpdate;
    lastAnimationPhaseUpdate = now;
    if (elapsedMs == 0) {
        return;
    }

    for (uint8_t stripIndex = 0; stripIndex < configuredStripCount; stripIndex++) {
        for (uint8_t zoneIndex = 0; zoneIndex < DGX_SPARKS_PER_STRIP; zoneIndex++) {
            const uint8_t utilization = utilizations[stripIndex][zoneIndex];
            if (utilization == OFFLINE_UTILIZATION || utilization == 0) {
                continue;
            }

            const uint16_t speed = runningZoneSpeed(utilization);
            const uint16_t phaseIncrement = static_cast<uint16_t>(
                    (static_cast<uint64_t>(speed) * elapsedMs * ANIMATION_PHASES_PER_CYCLE) /
                    MILLISECONDS_PER_MINUTE);
            runningZonePhases[stripIndex][zoneIndex] += phaseIncrement;
        }
    }
}

void renderRunningZone(CRGB* pixels, uint8_t zoneIndex, uint8_t utilization, uint16_t phase) {
    // Higher utilization shifts the moving indicator from green toward red.
    const uint8_t clampedUtilization = constrain(utilization, 1, 100);
    const CRGB activeColor(
            static_cast<uint8_t>(map(clampedUtilization, 1, 100, 0, 255)),
            static_cast<uint8_t>(map(clampedUtilization, 1, 100, 255, 0)),
            0);
    CRGB backgroundColor = activeColor;
    backgroundColor.nscale8_video(10);
    fillZone(pixels, zoneIndex, backgroundColor);

    const uint8_t position = triwave8(static_cast<uint8_t>(phase >> 8));
    const uint8_t ledPosition = map(position, 0, 255, 0, LEDS_PER_ZONE - 1);
    const uint16_t offset = zoneOffset(zoneIndex);

    for (uint16_t pixelIndex = 0; pixelIndex < LEDS_PER_ZONE; pixelIndex++) {
        const uint16_t distance = pixelIndex > ledPosition ? pixelIndex - ledPosition : ledPosition - pixelIndex;
        if (distance == 0) {
            pixels[offset + pixelIndex] = activeColor;
        } else if (distance == 1) {
            pixels[offset + pixelIndex] = activeColor;
            pixels[offset + pixelIndex].nscale8_video(140);
        } else if (distance == 2) {
            pixels[offset + pixelIndex] = activeColor;
            pixels[offset + pixelIndex].nscale8_video(50);
        }
    }
}

void renderLedOutputs(const uint8_t utilizations[MAX_LED_STRIPS][DGX_SPARKS_PER_STRIP]) {
    for (uint8_t stripIndex = 0; stripIndex < configuredStripCount; stripIndex++) {
        if (!configuredOutputs[stripIndex]) {
            continue;
        }

        for (uint8_t zoneIndex = 0; zoneIndex < DGX_SPARKS_PER_STRIP; zoneIndex++) {
            const uint8_t utilization = utilizations[stripIndex][zoneIndex];
            if (utilization == OFFLINE_UTILIZATION) {
                fillZone(stripPixels[stripIndex], zoneIndex, CRGB::Black);
            } else if (utilization <= IDLE_UTILIZATION_MAX) {
                renderIdleZone(stripPixels[stripIndex], zoneIndex);
            } else {
                renderRunningZone(stripPixels[stripIndex], zoneIndex, utilization, runningZonePhases[stripIndex][zoneIndex]);
            }
        }
        clearStripCenterLed(stripPixels[stripIndex]);
    }
    FastLED.show();
}

bool fetchDgxUtilization(const char* url, uint8_t& utilization) {
    if (!isValidDgxUrl(url) || WiFi.status() != WL_CONNECTED) {
        return false;
    }

    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    if (!http.begin(url)) {
        return false;
    }

    const int responseCode = http.GET();
    if (responseCode != HTTP_CODE_OK) {
        http.end();
        return false;
    }

    const String metrics = http.getString();
    http.end();

    // Parse Prometheus samples directly and average all GPUs exposed by a DGX.
    float utilizationSum = 0.0f;
    uint16_t gpuCount = 0;
    size_t lineStart = 0;
    while (lineStart < metrics.length()) {
        size_t lineEnd = metrics.indexOf('\n', lineStart);
        if (lineEnd == static_cast<size_t>(-1)) {
            lineEnd = metrics.length();
        }

        const size_t metricNameLength = sizeof(GPU_UTILIZATION_METRIC) - 1;
        if (lineEnd > lineStart + metricNameLength &&
            strncmp(metrics.c_str() + lineStart, GPU_UTILIZATION_METRIC, metricNameLength) == 0) {
            const char separator = metrics[lineStart + metricNameLength];
            const int closingBrace = separator == '{'
                ? metrics.indexOf('}', lineStart + metricNameLength)
                : -1;
            size_t valueStart = closingBrace >= 0
                ? closingBrace + 1
                : lineStart + metricNameLength;

            if ((separator == '{' || separator == ' ' || separator == '\t') &&
                (separator != '{' || closingBrace >= 0) && valueStart < lineEnd) {
                while (valueStart < lineEnd && (metrics[valueStart] == ' ' || metrics[valueStart] == '\t')) {
                    valueStart++;
                }

                String value = metrics.substring(valueStart, lineEnd);
                value.trim();
                char* valueEnd = nullptr;
                const float reportedUtilization = strtof(value.c_str(), &valueEnd);
                if (valueEnd != value.c_str() && *valueEnd == '\0' &&
                    reportedUtilization >= 0.0f && reportedUtilization <= 100.0f) {
                    utilizationSum += reportedUtilization;
                    gpuCount++;
                }
            }
        }

        lineStart = lineEnd + 1;
    }

    if (gpuCount == 0) {
        return false;
    }

    utilization = static_cast<uint8_t>((utilizationSum / gpuCount) + 0.5f);
    return true;
}

uint8_t createExampleUtilization() {
    switch (random(3)) {
        case 0: return OFFLINE_UTILIZATION;
        case 1: return 0;
        default: return random(6, 101);
    }
}

void refreshZoneUtilizations() {
    uint8_t refreshedUtilizations[MAX_LED_STRIPS][DGX_SPARKS_PER_STRIP] = {};
    if (RUN_WITH_EXAMPLE_VALUES) {
        // Keep unconfigured zones offline while exercising each display state.
        for (uint8_t stripIndex = 0; stripIndex < appConfig.stripCount; stripIndex++) {
            for (uint8_t dgxIndex = 0; dgxIndex < DGX_SPARKS_PER_STRIP; dgxIndex++) {
                const char* url = appConfig.strips[stripIndex].dgxUrls[dgxIndex];
                refreshedUtilizations[stripIndex][dgxIndex] = url[0] == '\0' ? OFFLINE_UTILIZATION : createExampleUtilization();
            }
        }
    } else {
        for (uint8_t stripIndex = 0; stripIndex < appConfig.stripCount; stripIndex++) {
            for (uint8_t dgxIndex = 0; dgxIndex < DGX_SPARKS_PER_STRIP; dgxIndex++) {
                uint8_t utilization = 0;
                const char* url = appConfig.strips[stripIndex].dgxUrls[dgxIndex];
                refreshedUtilizations[stripIndex][dgxIndex] = url[0] != '\0' && fetchDgxUtilization(url, utilization)
                    ? utilization
                    : OFFLINE_UTILIZATION;
            }
        }
    }

    portENTER_CRITICAL(&zoneUtilizationsMux);
    memcpy(zoneUtilizations, refreshedUtilizations, sizeof(zoneUtilizations));
    portEXIT_CRITICAL(&zoneUtilizationsMux);
}

void runNetworkTask(void*) {
    beginWebUi(appConfig);
    beginSerialCli(appConfig, zoneUtilizations, RUN_WITH_EXAMPLE_VALUES);

    for (;;) {
        handleWebUi();
        handleSerialCli();
        if (webUiRestartRequested() || serialCliRestartRequested()) {
            ESP.restart();
        }

        if (serialCliRefreshRequested()) {
            statusRefreshDue = true;
        }

        const unsigned long now = millis();
        const unsigned long refreshIntervalMs = static_cast<unsigned long>(appConfig.fetchIntervalSeconds) * 1000UL;
        if (statusRefreshDue || now - lastStatusRefresh >= refreshIntervalMs) {
            refreshZoneUtilizations();
            lastStatusRefresh = now;
            statusRefreshDue = false;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void setup() {
    Serial.begin(115200);
    randomSeed(esp_random());
    loadAppConfig(appConfig);
    beginStripOutputs();
    lastAnimationPhaseUpdate = millis();
    xTaskCreatePinnedToCore(runNetworkTask, "Network", 8192, nullptr, 1, nullptr, 0);
}

void loop() {
    uint8_t animationUtilizations[MAX_LED_STRIPS][DGX_SPARKS_PER_STRIP];
    portENTER_CRITICAL(&zoneUtilizationsMux);
    memcpy(animationUtilizations, zoneUtilizations, sizeof(animationUtilizations));
    portEXIT_CRITICAL(&zoneUtilizationsMux);

    updateRunningZonePhases(millis(), animationUtilizations);
    renderLedOutputs(animationUtilizations);
    delay(10);
}
