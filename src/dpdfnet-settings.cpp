// SPDX-License-Identifier: GPL-2.0-or-later

#include "dpdfnet-settings.hpp"

#include <filesystem>

bool dpdfnet_paths_equivalent(const std::string &left,
                              const std::string &right) {
  if (left.empty() || right.empty())
    return false;
  std::error_code error;
  if (std::filesystem::equivalent(left, right, error))
    return true;
  error.clear();
  const auto normalized_left = std::filesystem::weakly_canonical(left, error);
  if (error)
    return false;
  const auto normalized_right = std::filesystem::weakly_canonical(right, error);
  return !error && normalized_left == normalized_right;
}

bool dpdfnet_known_model_selection(const std::string &selection) {
  return selection == DPDFNET_MODEL_QUALITY ||
         selection == DPDFNET_MODEL_LOW_CPU ||
         selection == DPDFNET_MODEL_CUSTOM;
}

std::string dpdfnet_classify_model_selection(bool has_selection,
                                             const std::string &selection,
                                             bool has_legacy_path,
                                             const std::string &legacy_path,
                                             const std::string &quality_path,
                                             const std::string &low_cpu_path) {
  if (has_selection && dpdfnet_known_model_selection(selection))
    return selection;
  if (has_legacy_path && dpdfnet_paths_equivalent(legacy_path, low_cpu_path))
    return DPDFNET_MODEL_LOW_CPU;
  if (has_legacy_path && !legacy_path.empty() &&
      !dpdfnet_paths_equivalent(legacy_path, quality_path))
    return DPDFNET_MODEL_CUSTOM;
  return DPDFNET_MODEL_QUALITY;
}
