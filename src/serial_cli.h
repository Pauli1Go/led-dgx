#pragma once

#include "app_config.h"

void beginSerialCli(AppConfig& config, const uint8_t (&zoneUtilizations)[MAX_LED_STRIPS][DGX_SPARKS_PER_STRIP], bool exampleValuesEnabled);
void handleSerialCli();
bool serialCliRestartRequested();
bool serialCliRefreshRequested();