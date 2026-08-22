#pragma once

#include <obs-module.h>

// Keep the plugin identifier stable while retaining a structured component in
// the message. Every plugin-owned log must pass through this boundary.
#define OBS_SYNC_REPLAY_LOG(level, component, ...) \
    blog(level, "[obs-sync-replay] " component ": " __VA_ARGS__)
