// SPDX-License-Identifier: GPL-2.0-or-later

#include "dpdfnet-model.hpp"

#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

extern struct obs_source_info dpdfnet_filter_info;

namespace {
void ORT_API_CALL log_onnxruntime(void *, OrtLoggingLevel severity,
                                  const char *, const char *, const char *,
                                  const char *message) {
  const int level = severity >= ORT_LOGGING_LEVEL_ERROR     ? LOG_ERROR
                    : severity == ORT_LOGGING_LEVEL_WARNING ? LOG_WARNING
                                                            : LOG_INFO;
  blog(level, "[obs-dpdfnet] ONNX Runtime: %s", message);
}
} // namespace

bool obs_module_load(void) {
  DpdfnetModel::set_logger(log_onnxruntime);
  obs_register_source(&dpdfnet_filter_info);
  blog(LOG_INFO, "[obs-dpdfnet] loaded %s %s", PLUGIN_NAME, PLUGIN_VERSION);
  return true;
}

void obs_module_unload(void) {
  blog(LOG_INFO, "[obs-dpdfnet] unloaded %s", PLUGIN_NAME);
}
