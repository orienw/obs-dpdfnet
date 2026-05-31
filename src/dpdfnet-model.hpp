// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <onnxruntime_cxx_api.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class DpdfnetModel {
public:
  explicit DpdfnetModel(const std::filesystem::path &model_path);

  void reset();

  // Run one hop. The caller fills input_spectrum() with the noisy spectrum
  // (interleaved [r,i]), calls enhance(), then reads the enhanced spectrum from
  // output_spectrum(). Both buffers are bound as the ONNX tensors via IoBinding,
  // so there is no per-hop tensor allocation or output copy; the recurrent state
  // ping-pongs between two preallocated buffers instead of being copied back.
  void enhance();

  float *input_spectrum() { return in_spec_.data(); }
  float *output_spectrum() { return out_spec_.data(); }
  size_t spectrum_size() const { return in_spec_.size(); }

  int sample_rate() const { return sample_rate_; }
  int n_fft() const { return n_fft_; }
  int hop_size() const { return hop_size_; }
  int freq_bins() const { return freq_bins_; }
  const std::string &name() const { return name_; }
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
  Ort::MemoryInfo memory_info_{nullptr};

  std::string in_spec_name_;
  std::string in_state_name_;
  std::string out_spec_name_;
  std::string out_state_name_;

  std::vector<float> initial_state_;
  std::vector<float> in_spec_;
  std::vector<float> out_spec_;
  std::vector<float> state_a_;
  std::vector<float> state_b_;
  std::vector<int64_t> spec_shape_;
  std::vector<int64_t> state_shape_;

  Ort::Value spec_in_val_{nullptr};
  Ort::Value spec_out_val_{nullptr};
  Ort::Value state_a_val_{nullptr};
  Ort::Value state_b_val_{nullptr};
  std::optional<Ort::IoBinding> binding_a_;
  std::optional<Ort::IoBinding> binding_b_;
  int parity_ = 0;

  int sample_rate_ = 48000;
  int n_fft_ = 960;
  int hop_size_ = 480;
  int freq_bins_ = 481;
  std::string name_;
  std::string profile_;
};
