// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "dpdfnet-model.hpp"
#include "ring.hpp"
#include "stft.hpp"

#include <media-io/audio-resampler.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

constexpr size_t DPDFNET_MAX_AUDIO_PLANES = 8;
constexpr uint32_t DPDFNET_MAX_REALTIME_PACKET_FRAMES = 8192;

struct DpdfnetPacketInfo {
  uint32_t frames = 0;
  uint64_t timestamp = 0;
};

struct DpdfnetRealtimeCapacity {
  size_t input_samples = 0;
  size_t output_samples = 0;
  size_t dry_samples = 0;
  size_t packet_infos = 0;
};

struct DpdfnetRealtimeStorage {
  Ring<float> input_mono;
  Ring<float> output_mono;
  Ring<DpdfnetPacketInfo> info_queue;
  std::array<Ring<float>, DPDFNET_MAX_AUDIO_PLANES> dry_buffers;
  std::vector<float> mono_scratch;
  std::vector<float> dry_scratch;
  std::vector<float> enhanced_scratch;
  std::vector<float> zero_scratch;
  std::vector<float> frame;
  std::vector<float> enhanced_hop;
  std::vector<float> noisy_history;
};

struct DpdfnetModelBundle {
  std::unique_ptr<DpdfnetModel> model;
  std::unique_ptr<StreamingStft> stft;
  DpdfnetRealtimeStorage realtime;
};

DpdfnetRealtimeCapacity plan_dpdfnet_realtime_capacity(int model_sample_rate,
                                                       int n_fft,
                                                       int delay_samples = 0);
DpdfnetModelBundle prepare_dpdfnet_model(const std::string &path);
void prefill_dpdfnet_model_bundle(DpdfnetModelBundle &bundle, size_t channels,
                                  size_t frames);

class DpdfnetResamplers {
public:
  DpdfnetResamplers() = default;
  ~DpdfnetResamplers();

  DpdfnetResamplers(const DpdfnetResamplers &) = delete;
  DpdfnetResamplers &operator=(const DpdfnetResamplers &) = delete;
  DpdfnetResamplers(DpdfnetResamplers &&other) noexcept;
  DpdfnetResamplers &operator=(DpdfnetResamplers &&other) noexcept;

  explicit operator bool() const { return input_ && output_; }
  uint32_t native_rate() const { return native_rate_; }
  int model_rate() const { return model_rate_; }
  uint64_t delay_ns() const { return delay_ns_; }
  size_t prefill_frames() const { return prefill_frames_; }

private:
  friend DpdfnetResamplers prepare_dpdfnet_resamplers(uint32_t, int, int);
  friend class DpdfnetProcessor;

  void clear();

  audio_resampler_t *input_ = nullptr;
  audio_resampler_t *output_ = nullptr;
  uint32_t native_rate_ = 0;
  int model_rate_ = 0;
  uint64_t delay_ns_ = 0;
  size_t prefill_frames_ = 0;
};

DpdfnetResamplers prepare_dpdfnet_resamplers(uint32_t native_rate,
                                             int model_rate, int model_hop);

struct DpdfnetControls {
  int input_channel = 0;
  double attenuation_limit_db = 24.0;
  double wet_mix = 1.0;
  float output_gain = 1.0f;
  bool bypass = false;
};

struct DpdfnetAudioPacket {
  std::array<const float *, DPDFNET_MAX_AUDIO_PLANES> data = {};
  uint32_t frames = 0;
  uint64_t timestamp = 0;
};

class DpdfnetTimestampFloor {
public:
  void observe_input(uint64_t timestamp);
  uint64_t apply(uint64_t timestamp, uint32_t frames, uint32_t sample_rate);
  void reset();

private:
  uint64_t next_timestamp_ = 0;
  uint64_t last_input_timestamp_ = 0;
  bool have_input_timestamp_ = false;
};

enum class DpdfnetDisposition { Passthrough, Pending, Processed };
enum class DpdfnetEvent {
  None,
  RateMismatch,
  ResamplerRefreshNeeded,
  ProcessingFailure,
  CircuitOpened,
  OversizedPacket,
  CapacityInvariantFailure,
  RealtimeOverloadCircuitOpened,
  Count
};

enum class DpdfnetDisableReason {
  None,
  RepeatedProcessingFailures,
  RealtimeOverload
};

struct DpdfnetProcessResult {
  DpdfnetDisposition disposition = DpdfnetDisposition::Passthrough;
  DpdfnetEvent event = DpdfnetEvent::None;
  std::array<float *, DPDFNET_MAX_AUDIO_PLANES> data = {};
  uint32_t frames = 0;
  uint64_t timestamp = 0;
  size_t processed_hops = 0;
  bool resampler_refresh_needed = false;
  std::array<char, 256> message = {};

  void fail_open() noexcept {
    disposition = DpdfnetDisposition::Passthrough;
    data.fill(nullptr);
    frames = 0;
    timestamp = 0;
  }
};

struct DpdfnetProcessorSnapshot {
  bool has_model = false;
  bool resampling = false;
  bool bypass = false;
  bool processing_disabled = false;
  DpdfnetDisableReason disable_reason = DpdfnetDisableReason::None;
  bool resampler_refresh_required = false;
  bool capacity_recovery_pending = false;
  uint32_t sample_rate = 0;
  size_t channels = 0;
  int model_rate = 0;
  int n_fft = 0;
  int hop_size = 0;
  unsigned consecutive_failures = 0;
  uint64_t oversized_packets = 0;
  uint64_t capacity_failures = 0;
  std::string model_path;
  std::string model_name;
  std::string last_error;
};

struct DpdfnetProcessorState {
  bool has_model = false;
  bool resampling = false;
  bool bypass = false;
  bool processing_disabled = false;
  DpdfnetDisableReason disable_reason = DpdfnetDisableReason::None;
  bool resampler_refresh_required = false;
  bool capacity_recovery_pending = false;
  uint32_t sample_rate = 0;
  size_t channels = 0;
  int model_rate = 0;
  int n_fft = 0;
  int hop_size = 0;
  unsigned consecutive_failures = 0;
  uint64_t oversized_packets = 0;
  uint64_t capacity_failures = 0;
  std::array<char, 256> last_error = {};
};

class DpdfnetProcessor {
public:
  DpdfnetProcessor();

  DpdfnetModelBundle replace_model(DpdfnetModelBundle model);
  DpdfnetResamplers replace_resamplers(DpdfnetResamplers resamplers,
                                       bool reset_model = true);
  DpdfnetResamplers release_invalid_resamplers();
  void set_format(uint32_t sample_rate, size_t channels);
  bool set_controls(const DpdfnetControls &controls);
  void reset_state();
  void reset_stream();
  bool disable_for_realtime_overload(const char *message);

  DpdfnetProcessResult process(const DpdfnetAudioPacket &audio);
  DpdfnetProcessorState state() const;
  DpdfnetProcessorSnapshot snapshot() const;

  const DpdfnetModel *model() const { return model_.get(); }
  bool resampler_matches(uint32_t native_rate, int model_rate) const;

private:
  static constexpr uint32_t MAX_AUDIO_PACKET_FRAMES =
      DPDFNET_MAX_REALTIME_PACKET_FRAMES;
  static constexpr size_t RESAMPLE_BOUND_SLACK = 256;
  static constexpr unsigned MAX_CONSECUTIVE_FAILURES = 3;
  static constexpr uint64_t NS_PER_SECOND = 1000000000ULL;
  static constexpr uint64_t MAX_TIMESTAMP_DEVIATION_NS = 50000000ULL;

  void reset_audio_state(bool reset_model = true);
  void recompute_mix();
  void recompute_path();
  void recompute_latency();
  bool timestamp_jump(uint64_t timestamp) const;
  bool buffers_can_accept(uint32_t frames) const;
  bool dry_buffers_have_frames(uint32_t frames) const;
  size_t to_model_frames(size_t native_frames) const;
  size_t to_native_frames(size_t model_frames) const;
  bool push_input(const DpdfnetAudioPacket &audio);
  size_t process_available_hops();
  DpdfnetProcessResult pop_output_packet(const DpdfnetPacketInfo &info,
                                         size_t processed_hops);
  DpdfnetProcessResult failure_result(const char *message);
  DpdfnetProcessResult capacity_failure_result(uint32_t frames, bool oversized);

  std::unique_ptr<DpdfnetModel> model_;
  std::unique_ptr<StreamingStft> stft_;
  DpdfnetResamplers resamplers_;

  DpdfnetRealtimeStorage realtime_;
  std::array<std::vector<float>, DPDFNET_MAX_AUDIO_PLANES> output_storage_;

  uint32_t sample_rate_ = 0;
  size_t channels_ = 0;
  uint64_t last_timestamp_ = 0;
  uint64_t expected_timestamp_ = 0;
  bool have_timestamp_ = false;
  uint64_t output_latency_ns_ = 0;
  size_t noisy_history_offset_ = 0;
  int warmup_hops_ = 0;
  bool resample_path_ = false;
  bool resamplers_valid_ = true;
  bool rate_warning_reported_ = false;
  bool process_error_reported_ = false;
  DpdfnetDisableReason disable_reason_ = DpdfnetDisableReason::None;
  bool capacity_recovery_pending_ = false;
  unsigned consecutive_failures_ = 0;
  uint64_t oversized_packets_ = 0;
  uint64_t capacity_failures_ = 0;
  std::array<char, 256> last_error_ = {};

  DpdfnetControls controls_;
  float attenuation_alpha_ = 0.0630957f;
  float dry_gain_ = 0.0f;
  float wet_gain_ = 1.0f;
};
