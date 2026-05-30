// SPDX-License-Identifier: GPL-2.0-or-later

#include "dpdfnet-model.hpp"

#include <algorithm>
#include <array>
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
  reset();
}

void DpdfnetModel::reset() { state_ = initial_state_; }

void DpdfnetModel::enhance_spectrum(const std::vector<float> &spec,
                                    std::vector<float> &enhanced_spec) {
  const size_t expected_spec = static_cast<size_t>(freq_bins_) * 2;
  if (spec.size() != expected_spec)
    throw std::runtime_error(
        "DPDFNet spectrum size does not match model metadata");
  if (state_.size() != initial_state_.size())
    throw std::runtime_error("DPDFNet state was corrupted");

  auto memory_info =
      Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  auto spec_input = Ort::Value::CreateTensor<float>(
      memory_info, const_cast<float *>(spec.data()), spec.size(),
      spec_shape_.data(), spec_shape_.size());
  auto state_input =
      Ort::Value::CreateTensor<float>(memory_info, state_.data(), state_.size(),
                                      state_shape_.data(), state_shape_.size());

  std::array<const char *, 2> input_names = {in_spec_name_.c_str(),
                                             in_state_name_.c_str()};
  std::array<const char *, 2> output_names = {out_spec_name_.c_str(),
                                              out_state_name_.c_str()};
  std::array<Ort::Value, 2> inputs = {std::move(spec_input),
                                      std::move(state_input)};

  auto outputs =
      session_->Run(Ort::RunOptions{nullptr}, input_names.data(), inputs.data(),
                    inputs.size(), output_names.data(), output_names.size());
  if (outputs.size() != 2)
    throw std::runtime_error(
        "DPDFNet ONNX inference returned an unexpected output count");

  auto spec_info = outputs[0].GetTensorTypeAndShapeInfo();
  const size_t spec_count = spec_info.GetElementCount();
  if (spec_count != expected_spec)
    throw std::runtime_error(
        "DPDFNet enhanced spectrum shape does not match input spectrum");

  const float *spec_out = outputs[0].GetTensorData<float>();
  enhanced_spec.assign(spec_out, spec_out + spec_count);

  auto state_info = outputs[1].GetTensorTypeAndShapeInfo();
  const size_t state_count = state_info.GetElementCount();
  if (state_count != state_.size())
    throw std::runtime_error(
        "DPDFNet state output shape does not match input state");

  const float *state_out = outputs[1].GetTensorData<float>();
  std::copy(state_out, state_out + state_count, state_.begin());
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
