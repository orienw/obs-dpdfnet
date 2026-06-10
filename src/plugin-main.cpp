// SPDX-License-Identifier: GPL-2.0-or-later

#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

extern struct obs_source_info dpdfnet_filter_info;

bool obs_module_load(void) {
  obs_register_source(&dpdfnet_filter_info);
  blog(LOG_INFO, "[obs-dpdfnet] loaded %s %s", PLUGIN_NAME, PLUGIN_VERSION);
  return true;
}

void obs_module_unload(void) {
  blog(LOG_INFO, "[obs-dpdfnet] unloaded %s", PLUGIN_NAME);
}
