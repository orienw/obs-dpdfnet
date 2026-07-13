// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

constexpr const char *DPDFNET_MODEL_QUALITY = "quality";
constexpr const char *DPDFNET_MODEL_LOW_CPU = "low_cpu";
constexpr const char *DPDFNET_MODEL_CUSTOM = "custom";

bool dpdfnet_paths_equivalent(const std::string &left,
                              const std::string &right);
bool dpdfnet_known_model_selection(const std::string &selection);
std::string dpdfnet_classify_model_selection(bool has_selection,
                                             const std::string &selection,
                                             bool has_legacy_path,
                                             const std::string &legacy_path,
                                             const std::string &quality_path,
                                             const std::string &low_cpu_path);
