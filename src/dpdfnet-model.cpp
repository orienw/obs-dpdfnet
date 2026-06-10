// SPDX-License-Identifier: GPL-2.0-or-later

#include "dpdfnet-model.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <stdexcept>
#include <string_view>
#include <system_error>

Ort::Env &DpdfnetModel::env() {
  static Ort::Env instance(ORT_LOGGING_LEVEL_WARNING, "obs-dpdfnet");
  return instance;
}

DpdfnetModel::DpdfnetModel(const std::filesystem::path &model_path)
    : model_path_(model_path) {
  if (!std::filesystem::is_regular_file(model_path_))
    throw std::runtime_error("ONNX model file does not exist: " +
                             model_path_.string());

  name_ = model_path_.stem().string();
  if (name_.empty())
    name_ = model_path_.filename().string();

  session_options_.SetIntraOpNumThreads(1);
  session_options_.SetInterOpNumThreads(1);
  session_options_.SetGraphOptimizationLevel(
      GraphOptimizationLevel::ORT_ENABLE_ALL);

  session_ = std::make_unique<Ort::Session>(env(), model_path_.c_str(),
                                            session_options_);

  if (session_->GetInputCount() < 2 || session_->GetOutputCount() < 2)
    throw std::runtime_error(
        "DPDFNet ONNX model must expose two inputs and two outputs");

  auto input_spec = session_->GetInputNameAllocated(0, allocator_);
  auto input_state = session_->GetInputNameAllocated(1, allocator_);
  auto output_spec = session_->GetOutputNameAllocated(0, allocator_);
  auto output_state = session_->GetOutputNameAllocated(1, allocator_);

  in_spec_name_ = input_spec.get();
  in_state_name_ = input_state.get();
  out_spec_name_ = output_spec.get();
  out_state_name_ = output_state.get();

  auto metadata = session_->GetModelMetadata();
  profile_ = metadata_value(metadata, "profile");
  sample_rate_ =
      parse_int(metadata_value(metadata, "sample_rate"), "sample_rate");
  n_fft_ = parse_int(metadata_value(metadata, "n_fft"), "n_fft");
  hop_size_ = parse_int(metadata_value(metadata, "hop_length"), "hop_length");
  freq_bins_ = parse_int(metadata_value(metadata, "freq_bins"), "freq_bins");

  const int state_size =
      parse_int(metadata_value(metadata, "state_size"), "state_size");
  const int erb_norm_state_size = parse_int(
      metadata_value(metadata, "erb_norm_state_size"), "erb_norm_state_size");
  const int spec_norm_state_size = parse_int(
      metadata_value(metadata, "spec_norm_state_size"), "spec_norm_state_size");

  if (n_fft_ <= 0 || hop_size_ <= 0 || freq_bins_ != (n_fft_ / 2 + 1))
    throw std::runtime_error(
        "DPDFNet model metadata has inconsistent FFT dimensions");
  if (state_size <= 0 || erb_norm_state_size < 0 || spec_norm_state_size < 0 ||
      erb_norm_state_size + spec_norm_state_size > state_size) {
    throw std::runtime_error(
        "DPDFNet model metadata has inconsistent state dimensions");
  }
  if (sample_rate_ < 8000 || sample_rate_ > 384000)
    throw std::runtime_error(
        "DPDFNet model metadata sample_rate is out of supported range");
  if (n_fft_ < 64 || n_fft_ > 8192)
    throw std::runtime_error(
        "DPDFNet model metadata n_fft is out of supported range");
  if (hop_size_ > n_fft_)
    throw std::runtime_error(
        "DPDFNet model metadata hop_length is larger than n_fft");
  if (state_size > 8'000'000)
    throw std::runtime_error(
        "DPDFNet model metadata state_size is out of supported range");

  initial_state_.assign(static_cast<size_t>(state_size), 0.0f);

  auto erb_norm = parse_float_list(metadata_value(metadata, "erb_norm_init"),
                                   static_cast<size_t>(erb_norm_state_size),
                                   "erb_norm_init");
  auto spec_norm = parse_float_list(metadata_value(metadata, "spec_norm_init"),
                                    static_cast<size_t>(spec_norm_state_size),
                                    "spec_norm_init");

  std::copy(erb_norm.begin(), erb_norm.end(), initial_state_.begin());
  std::copy(spec_norm.begin(), spec_norm.end(),
            initial_state_.begin() + erb_norm_state_size);

  spec_shape_ = {1, 1, freq_bins_, 2};
  state_shape_ = {state_size};

  memory_info_ = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

  const size_t spec_n = static_cast<size_t>(freq_bins_) * 2;
  in_spec_.assign(spec_n, 0.0f);
  out_spec_.assign(spec_n, 0.0f);
  state_a_ = initial_state_;
  state_b_ = initial_state_;

  spec_in_val_ = Ort::Value::CreateTensor<float>(
      memory_info_, in_spec_.data(), in_spec_.size(), spec_shape_.data(),
      spec_shape_.size());
  spec_out_val_ = Ort::Value::CreateTensor<float>(
      memory_info_, out_spec_.data(), out_spec_.size(), spec_shape_.data(),
      spec_shape_.size());
  state_a_val_ = Ort::Value::CreateTensor<float>(
      memory_info_, state_a_.data(), state_a_.size(), state_shape_.data(),
      state_shape_.size());
  state_b_val_ = Ort::Value::CreateTensor<float>(
      memory_info_, state_b_.data(), state_b_.size(), state_shape_.data(),
      state_shape_.size());

  // Two static bindings: enhance() alternates between them so the recurrent
  // state output of one hop is the state input of the next without a copy.
  binding_a_.emplace(*session_);
  binding_a_->BindInput(in_spec_name_.c_str(), spec_in_val_);
  binding_a_->BindInput(in_state_name_.c_str(), state_a_val_);
  binding_a_->BindOutput(out_spec_name_.c_str(), spec_out_val_);
  binding_a_->BindOutput(out_state_name_.c_str(), state_b_val_);

  binding_b_.emplace(*session_);
  binding_b_->BindInput(in_spec_name_.c_str(), spec_in_val_);
  binding_b_->BindInput(in_state_name_.c_str(), state_b_val_);
  binding_b_->BindOutput(out_spec_name_.c_str(), spec_out_val_);
  binding_b_->BindOutput(out_state_name_.c_str(), state_a_val_);

  reset();
}

void DpdfnetModel::reset() {
  std::copy(initial_state_.begin(), initial_state_.end(), state_a_.begin());
  parity_ = 0;
}

void DpdfnetModel::enhance() {
  Ort::IoBinding &binding = parity_ == 0 ? *binding_a_ : *binding_b_;
  session_->Run(Ort::RunOptions{nullptr}, binding);
  parity_ ^= 1;
}

std::string DpdfnetModel::metadata_value(Ort::ModelMetadata &metadata,
                                         const char *key) const {
  auto value = metadata.LookupCustomMetadataMapAllocated(key, allocator_);
  if (!value)
    throw std::runtime_error(
        std::string("DPDFNet ONNX model is missing metadata key: ") + key);
  return value.get();
}

int DpdfnetModel::parse_int(const std::string &value, const char *key) {
  int out = 0;
  const char *begin = value.data();
  const char *end = value.data() + value.size();
  const auto result = std::from_chars(begin, end, out);
  if (result.ec != std::errc() || result.ptr != end)
    throw std::runtime_error(
        std::string("DPDFNet metadata key is not an integer: ") + key);
  return out;
}

std::vector<float> DpdfnetModel::parse_float_list(const std::string &value,
                                                  size_t expected,
                                                  const char *key) {
  std::vector<float> out;
  out.reserve(expected);

  size_t start = 0;
  while (start < value.size()) {
    const size_t comma = value.find(',', start);
    const size_t stop = comma == std::string::npos ? value.size() : comma;
    const std::string_view token(value.data() + start, stop - start);

    if (!token.empty()) {
      float number = 0.0f;
      const auto *begin = token.data();
      const auto *end = token.data() + token.size();
      const auto result = std::from_chars(begin, end, number);
      if (result.ec != std::errc() || result.ptr != end)
        throw std::runtime_error(
            std::string("DPDFNet metadata key contains an invalid float: ") +
            key);
      out.push_back(number);
    }

    if (comma == std::string::npos)
      break;
    start = comma + 1;
  }

  if (out.size() != expected) {
    throw std::runtime_error(
        std::string("DPDFNet metadata key has unexpected value count: ") + key);
  }

  return out;
}
