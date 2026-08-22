#include "ui/capture-controls-localization.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

namespace obs_sync_replay {

CaptureControlsLabels ResolveCaptureControlsLabels() {
    return ResolveCaptureControlsLabels(
        [](const char *key) { return obs_frontend_get_locale_string(key); },
        [](const char *key) { return obs_module_text(key); });
}

} // namespace obs_sync_replay
