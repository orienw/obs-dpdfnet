// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <onnxruntime_cxx_api.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

class DpdfnetModel {
public:
  explicit DpdfnetModel(const std::filesystem::path &model_path);

  void reset();
  void enhance_spectrum(const std::vector<float> &spec,
                        std::vector<float> &enhanced_spec);

  int sample_rate() const { return sample_rate_; }
  int n_fft() const { return n_fft_; }
  int hop_size() const { return hop_size_; }
  int freq_bins() const { return freq_bins_; }
  const std::string &profile() const { return profile_; }
  const std::filesystem::path &path() const { return model_path_; }

private:
  static Ort::Env &env();

  std::string metadata_value(Ort::ModelMetadata &metadata,
                             const char *key) const;
  static int parse_int(const std::string &value, const char *key);
  static std::vector<float> parse_float_list(const std::string &value,
                                             size_t expected, const char *key);

  std::filesystem::path model_path_;
  Ort::AllocatorWithDefaultOptions allocator_;
  Ort::SessionOptions session_options_;
  std::unique_ptr<Ort::Session> session_;

  std::string in_spec_name_;
  std::string in_state_name_;
  std::string out_spec_name_;
  std::string out_state_name_;

  std::vector<float> initial_state_;
  std::vector<float> state_;
  std::vector<int64_t> spec_shape_;
  std::vector<int64_t> state_shape_;

  int sample_rate_ = 48000;
  int n_fft_ = 960;
  int hop_size_ = 480;
  int freq_bins_ = 481;
  std::string profile_;
};
