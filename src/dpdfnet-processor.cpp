// SPDX-License-Identifier: GPL-2.0-or-later

#include "dpdfnet-processor.hpp"

#if defined(_M_X64) || defined(__x86_64__)
#include <pmmintrin.h>
#include <xmmintrin.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {
constexpr uint32_t MIN_SUPPORTED_SAMPLE_RATE = 8000;
constexpr uint32_t MAX_SUPPORTED_SAMPLE_RATE = 384000;
constexpr size_t MAX_SUPPORTED_NFFT = 8192;
constexpr size_t MAX_RESAMPLER_PREFILL_FRAMES = 8192;
constexpr size_t RESAMPLE_BOUND_SLACK = 256;

#if defined(_M_X64) || defined(__x86_64__)
constexpr unsigned int MXCSR_FLUSH_ZERO = 0x8000;
constexpr unsigned int MXCSR_DENORMALS_ZERO = 0x0040;

struct DenormalModeGuard {
  DenormalModeGuard() : mxcsr_(_mm_getcsr()) {
    _mm_setcsr(mxcsr_ | MXCSR_FLUSH_ZERO | MXCSR_DENORMALS_ZERO);
  }
  ~DenormalModeGuard() { _mm_setcsr(mxcsr_); }

private:
  unsigned int mxcsr_;
};
#else
struct DenormalModeGuard {};
#endif

float db_to_amp(double db) {
  return static_cast<float>(std::pow(10.0, db / 20.0));
}

size_t scale_frames_ceil(size_t frames, uint32_t numerator,
                         uint32_t denominator) {
  return (frames * static_cast<size_t>(numerator) + denominator - 1) /
         denominator;
}

void include_capacity_for_rate(DpdfnetRealtimeCapacity &capacity,
                               uint32_t native_rate, uint32_t model_rate,
                               size_t n_fft) {
  const bool resampling = native_rate != model_rate;
  const size_t input_gain =
      resampling ? scale_frames_ceil(DPDFNET_MAX_REALTIME_PACKET_FRAMES,
                                     model_rate, native_rate) +
                       RESAMPLE_BOUND_SLACK
                 : DPDFNET_MAX_REALTIME_PACKET_FRAMES;
  const size_t input_samples = n_fft + input_gain;
  const size_t synthesized_samples =
      resampling ? scale_frames_ceil(input_samples, native_rate, model_rate) +
                       RESAMPLE_BOUND_SLACK
                 : input_samples;
  const size_t output_samples =
      DPDFNET_MAX_REALTIME_PACKET_FRAMES + synthesized_samples;
  const size_t dry_samples = MAX_RESAMPLER_PREFILL_FRAMES +
                             DPDFNET_MAX_REALTIME_PACKET_FRAMES +
                             output_samples;

  capacity.input_samples = std::max(capacity.input_samples, input_samples);
  capacity.output_samples = std::max(capacity.output_samples, output_samples);
  capacity.dry_samples = std::max(capacity.dry_samples, dry_samples);
  capacity.packet_infos = std::max(capacity.packet_infos, dry_samples);
}

audio_resampler_t *create_mono_resampler(uint32_t src_rate, uint32_t dst_rate) {
  struct resample_info src = {};
  src.samples_per_sec = src_rate;
  src.format = AUDIO_FORMAT_FLOAT;
  src.speakers = SPEAKERS_MONO;

  struct resample_info dst = src;
  dst.samples_per_sec = dst_rate;
  return audio_resampler_create(&dst, &src);
}

struct ResampleProbe {
  size_t delay_frames = 0;
  uint64_t delay_ns = 0;
  bool ok = false;
};

ResampleProbe probe_resampler_pair(audio_resampler_t *in,
                                   audio_resampler_t *out, uint32_t native_rate,
                                   uint32_t model_rate, int model_hop) {
  constexpr size_t MIN_PRIME_FRAMES = 16384;
  constexpr size_t FLUSH_FRAMES = 4096;
  constexpr size_t MAX_PLAUSIBLE_DELAY = 4096;
  constexpr uint64_t NS_PER_SECOND = 1000000000ULL;

  const size_t hop = model_hop > 0 ? static_cast<size_t>(model_hop) : 0;
  const size_t hop_native =
      (2 * hop * native_rate + model_rate - 1) / model_rate;
  const size_t prime_frames = std::max(MIN_PRIME_FRAMES, hop_native);

  std::vector<float> block(prime_frames, 0.0f);
  std::vector<float> collected;
  collected.reserve(prime_frames + 2 * FLUSH_FRAMES);

  const auto feed = [&](const float *data, uint32_t frames) {
    uint8_t *mid[DPDFNET_MAX_AUDIO_PLANES] = {};
    uint32_t mid_frames = 0;
    uint64_t ts_offset = 0;
    const uint8_t *input[DPDFNET_MAX_AUDIO_PLANES] = {
        reinterpret_cast<const uint8_t *>(data)};
    if (!audio_resampler_resample(in, mid, &mid_frames, &ts_offset, input,
                                  frames))
      return false;
    if (!mid_frames)
      return true;

    uint8_t *fin[DPDFNET_MAX_AUDIO_PLANES] = {};
    uint32_t fin_frames = 0;
    const uint8_t *mid_input[DPDFNET_MAX_AUDIO_PLANES] = {mid[0]};
    if (!audio_resampler_resample(out, fin, &fin_frames, &ts_offset, mid_input,
                                  mid_frames))
      return false;

    const float *samples = reinterpret_cast<const float *>(fin[0]);
    collected.insert(collected.end(), samples, samples + fin_frames);
    return true;
  };

  block[0] = 1.0f;
  if (!feed(block.data(), static_cast<uint32_t>(prime_frames)))
    return {};
  block[0] = 0.0f;
  if (!feed(block.data(), FLUSH_FRAMES))
    return {};

  const size_t fed = prime_frames + FLUSH_FRAMES;
  if (collected.size() > fed)
    return {};
  const size_t holdback = fed - collected.size();

  size_t peak = 0;
  float magnitude = 0.0f;
  for (size_t i = 0; i < collected.size(); ++i) {
    const float value = std::fabs(collected[i]);
    if (value > magnitude) {
      magnitude = value;
      peak = i;
    }
  }

  const float passband = std::min(1.0f, static_cast<float>(model_rate) /
                                            static_cast<float>(native_rate));
  if (magnitude < 0.5f * passband || peak > MAX_PLAUSIBLE_DELAY ||
      holdback > MAX_PLAUSIBLE_DELAY)
    return {};

  float tail = 0.0f;
  for (size_t i = peak + 512; i < collected.size(); ++i)
    tail = std::max(tail, std::fabs(collected[i]));
  if (tail > 1e-3f)
    return {};

  ResampleProbe probe;
  probe.delay_frames = holdback + peak;
  probe.delay_ns =
      static_cast<uint64_t>(static_cast<double>(probe.delay_frames) /
                                static_cast<double>(native_rate) *
                                static_cast<double>(NS_PER_SECOND) +
                            0.5);
  probe.ok = true;
  return probe;
}
} // namespace

DpdfnetRealtimeCapacity plan_dpdfnet_realtime_capacity(int model_sample_rate,
                                                       int n_fft) {
  DpdfnetRealtimeCapacity capacity{16384, 16384, 16384, 256};
  if (model_sample_rate < static_cast<int>(MIN_SUPPORTED_SAMPLE_RATE) ||
      model_sample_rate > static_cast<int>(MAX_SUPPORTED_SAMPLE_RATE) ||
      n_fft <= 0 || n_fft > static_cast<int>(MAX_SUPPORTED_NFFT))
    return capacity;

  const auto model_rate = static_cast<uint32_t>(model_sample_rate);
  include_capacity_for_rate(capacity, MIN_SUPPORTED_SAMPLE_RATE, model_rate,
                            static_cast<size_t>(n_fft));
  include_capacity_for_rate(capacity, model_rate, model_rate,
                            static_cast<size_t>(n_fft));
  include_capacity_for_rate(capacity, MAX_SUPPORTED_SAMPLE_RATE, model_rate,
                            static_cast<size_t>(n_fft));
  return capacity;
}

DpdfnetModelBundle prepare_dpdfnet_model(const std::string &path) {
  DpdfnetModelBundle bundle;
  bundle.model = std::make_unique<DpdfnetModel>(path);
  std::fill_n(bundle.model->input_spectrum(), bundle.model->spectrum_size(),
              0.0f);
  bundle.model->enhance();
  bundle.model->reset();
  bundle.stft = std::make_unique<StreamingStft>(bundle.model->n_fft(),
                                                bundle.model->hop_size());

  const DpdfnetRealtimeCapacity capacity = plan_dpdfnet_realtime_capacity(
      bundle.model->sample_rate(), bundle.model->n_fft());
  bundle.realtime.input_mono.reserve(capacity.input_samples);
  bundle.realtime.output_mono.reserve(capacity.output_samples);
  bundle.realtime.info_queue.reserve(capacity.packet_infos);
  for (auto &buffer : bundle.realtime.dry_buffers)
    buffer.reserve(capacity.dry_samples);
  bundle.realtime.mono_scratch.reserve(DPDFNET_MAX_REALTIME_PACKET_FRAMES);
  bundle.realtime.dry_scratch.reserve(DPDFNET_MAX_REALTIME_PACKET_FRAMES);
  bundle.realtime.enhanced_scratch.reserve(DPDFNET_MAX_REALTIME_PACKET_FRAMES);
  bundle.realtime.zero_scratch.assign(
      std::max<size_t>(DPDFNET_MAX_REALTIME_PACKET_FRAMES,
                       MAX_RESAMPLER_PREFILL_FRAMES),
      0.0f);
  bundle.realtime.frame.assign(static_cast<size_t>(bundle.model->n_fft()),
                               0.0f);
  bundle.realtime.enhanced_hop.assign(
      static_cast<size_t>(bundle.model->hop_size()), 0.0f);
  return bundle;
}

void prefill_dpdfnet_model_bundle(DpdfnetModelBundle &bundle, size_t channels,
                                  size_t frames) {
  if (!frames)
    return;
  channels = std::clamp<size_t>(channels, 1, DPDFNET_MAX_AUDIO_PLANES);
  if (frames > bundle.realtime.zero_scratch.size())
    bundle.realtime.zero_scratch.resize(frames, 0.0f);
  for (size_t channel = 0; channel < channels; ++channel) {
    bundle.realtime.dry_buffers[channel].reserve(
        bundle.realtime.dry_buffers[channel].capacity() + frames);
    if (!bundle.realtime.dry_buffers[channel].try_push(
            bundle.realtime.zero_scratch.data(), frames))
      throw std::runtime_error("resampler prefill exceeds dry buffer capacity");
  }
}

uint64_t DpdfnetTimestampFloor::apply(uint64_t timestamp, uint32_t frames,
                                      uint32_t sample_rate) {
  timestamp = std::max(timestamp, next_timestamp_);
  const uint64_t duration =
      sample_rate ? static_cast<uint64_t>(static_cast<double>(frames) /
                                          static_cast<double>(sample_rate) *
                                          1000000000.0)
                  : 0;
  next_timestamp_ = timestamp > std::numeric_limits<uint64_t>::max() - duration
                        ? std::numeric_limits<uint64_t>::max()
                        : timestamp + duration;
  return timestamp;
}

void DpdfnetTimestampFloor::observe_input(uint64_t timestamp) {
  if (have_input_timestamp_ && timestamp < last_input_timestamp_)
    next_timestamp_ = 0;
  last_input_timestamp_ = timestamp;
  have_input_timestamp_ = true;
}

void DpdfnetTimestampFloor::reset() {
  next_timestamp_ = 0;
  last_input_timestamp_ = 0;
  have_input_timestamp_ = false;
}

DpdfnetResamplers::~DpdfnetResamplers() { clear(); }

DpdfnetResamplers::DpdfnetResamplers(DpdfnetResamplers &&other) noexcept {
  *this = std::move(other);
}

DpdfnetResamplers &
DpdfnetResamplers::operator=(DpdfnetResamplers &&other) noexcept {
  if (this == &other)
    return *this;
  clear();
  input_ = std::exchange(other.input_, nullptr);
  output_ = std::exchange(other.output_, nullptr);
  native_rate_ = std::exchange(other.native_rate_, 0);
  model_rate_ = std::exchange(other.model_rate_, 0);
  delay_ns_ = std::exchange(other.delay_ns_, 0);
  prefill_frames_ = std::exchange(other.prefill_frames_, 0);
  return *this;
}

void DpdfnetResamplers::clear() {
  audio_resampler_destroy(input_);
  audio_resampler_destroy(output_);
  input_ = nullptr;
  output_ = nullptr;
  native_rate_ = 0;
  model_rate_ = 0;
  delay_ns_ = 0;
  prefill_frames_ = 0;
}

DpdfnetResamplers prepare_dpdfnet_resamplers(uint32_t native_rate,
                                             int model_rate, int model_hop) {
  DpdfnetResamplers result;
  if (!native_rate || model_rate <= 0 ||
      native_rate == static_cast<uint32_t>(model_rate))
    return result;

  result.input_ =
      create_mono_resampler(native_rate, static_cast<uint32_t>(model_rate));
  result.output_ =
      create_mono_resampler(static_cast<uint32_t>(model_rate), native_rate);
  if (!result.input_ || !result.output_)
    throw std::runtime_error("failed to create audio resamplers");

  const ResampleProbe probe =
      probe_resampler_pair(result.input_, result.output_, native_rate,
                           static_cast<uint32_t>(model_rate), model_hop);
  if (!probe.ok)
    throw std::runtime_error("audio resampler alignment probe failed");

  result.native_rate_ = native_rate;
  result.model_rate_ = model_rate;
  result.delay_ns_ = probe.delay_ns;
  result.prefill_frames_ = probe.delay_frames;
  return result;
}

DpdfnetProcessor::DpdfnetProcessor() {
  for (auto &storage : output_storage_)
    storage.reserve(MAX_AUDIO_PACKET_FRAMES);
}

DpdfnetModelBundle DpdfnetProcessor::replace_model(DpdfnetModelBundle bundle) {
  DpdfnetModelBundle old{std::move(model_), std::move(stft_),
                         std::move(realtime_)};
  model_ = std::move(bundle.model);
  stft_ = std::move(bundle.stft);
  realtime_ = std::move(bundle.realtime);
  disable_reason_ = DpdfnetDisableReason::None;
  capacity_recovery_pending_ = false;
  consecutive_failures_ = 0;
  oversized_packets_ = 0;
  capacity_failures_ = 0;
  process_error_reported_ = false;
  last_error_.fill(0);
  recompute_path();
  recompute_latency();
  last_timestamp_ = 0;
  expected_timestamp_ = 0;
  have_timestamp_ = false;
  return old;
}

DpdfnetResamplers
DpdfnetProcessor::replace_resamplers(DpdfnetResamplers resamplers,
                                     bool reset_model) {
  if (!resamplers_ && !resamplers) {
    resamplers_valid_ = true;
    return {};
  }
  DpdfnetResamplers old = std::move(resamplers_);
  resamplers_ = std::move(resamplers);
  resamplers_valid_ = true;
  rate_warning_reported_ = false;
  if (reset_model)
    reset_audio_state();
  else {
    recompute_path();
    recompute_latency();
  }
  return old;
}

DpdfnetResamplers DpdfnetProcessor::release_invalid_resamplers() {
  if (resamplers_valid_ ||
      (model_ && sample_rate_ != static_cast<uint32_t>(model_->sample_rate())))
    return {};
  DpdfnetResamplers old = std::move(resamplers_);
  resamplers_valid_ = true;
  rate_warning_reported_ = false;
  recompute_path();
  recompute_latency();
  return old;
}

void DpdfnetProcessor::set_format(uint32_t sample_rate, size_t channels) {
  channels = std::clamp<size_t>(channels, 1, DPDFNET_MAX_AUDIO_PLANES);
  if (sample_rate == sample_rate_ && channels == channels_)
    return;
  if (model_ && (sample_rate != static_cast<uint32_t>(model_->sample_rate()) ||
                 resamplers_))
    resamplers_valid_ = false;
  sample_rate_ = sample_rate;
  channels_ = channels;
  rate_warning_reported_ = false;
  reset_audio_state();
}

bool DpdfnetProcessor::set_controls(const DpdfnetControls &controls) {
  const bool stream_boundary =
      controls.input_channel != controls_.input_channel;
  controls_ = controls;
  controls_.wet_mix = std::clamp(controls_.wet_mix, 0.0, 1.0);
  controls_.attenuation_limit_db =
      std::clamp(controls_.attenuation_limit_db, 0.0, 60.0);
  recompute_mix();
  if (stream_boundary)
    reset_audio_state();
  return stream_boundary;
}

void DpdfnetProcessor::reset_state() {
  disable_reason_ = DpdfnetDisableReason::None;
  capacity_recovery_pending_ = false;
  consecutive_failures_ = 0;
  oversized_packets_ = 0;
  capacity_failures_ = 0;
  process_error_reported_ = false;
  last_error_.fill(0);
  reset_audio_state();
}

void DpdfnetProcessor::reset_stream() { reset_audio_state(); }

bool DpdfnetProcessor::resampler_matches(uint32_t native_rate,
                                         int model_rate) const {
  return resamplers_valid_ && resamplers_ &&
         resamplers_.native_rate_ == native_rate &&
         resamplers_.model_rate_ == model_rate;
}

void DpdfnetProcessor::reset_audio_state(bool reset_model) {
  recompute_path();
  recompute_latency();
  realtime_.input_mono.clear();
  realtime_.output_mono.clear();
  for (auto &buffer : realtime_.dry_buffers)
    buffer.clear();
  realtime_.info_queue.clear();
  last_timestamp_ = 0;
  expected_timestamp_ = 0;
  have_timestamp_ = false;

  if (resample_path_ && resamplers_.prefill_frames_) {
    for (size_t channel = 0; channel < channels_; ++channel)
      realtime_.dry_buffers[channel].try_push(realtime_.zero_scratch.data(),
                                              resamplers_.prefill_frames_);
  }
  if (reset_model && model_)
    model_->reset();
  if (reset_model && stft_)
    stft_->reset();
}

void DpdfnetProcessor::recompute_mix() {
  attenuation_alpha_ = db_to_amp(-controls_.attenuation_limit_db);
  dry_gain_ =
      static_cast<float>((1.0 - controls_.wet_mix) * controls_.output_gain);
  wet_gain_ = static_cast<float>(controls_.wet_mix * controls_.output_gain);
}

void DpdfnetProcessor::recompute_path() {
  resample_path_ = model_ && resamplers_valid_ && resamplers_ &&
                   resamplers_.native_rate_ == sample_rate_ &&
                   resamplers_.model_rate_ == model_->sample_rate();
}

void DpdfnetProcessor::recompute_latency() {
  output_latency_ns_ = 0;
  if (model_) {
    output_latency_ns_ = static_cast<uint64_t>(
        static_cast<double>(model_->hop_size()) /
        static_cast<double>(model_->sample_rate()) * NS_PER_SECOND);
  }
  if (resample_path_)
    output_latency_ns_ += resamplers_.delay_ns_;
}

bool DpdfnetProcessor::timestamp_jump(uint64_t timestamp) const {
  if (!have_timestamp_)
    return false;
  if (timestamp < last_timestamp_)
    return true;
  if (!expected_timestamp_)
    return false;
  const uint64_t diff = timestamp > expected_timestamp_
                            ? timestamp - expected_timestamp_
                            : expected_timestamp_ - timestamp;
  return diff > MAX_TIMESTAMP_DEVIATION_NS;
}

size_t DpdfnetProcessor::to_model_frames(size_t native_frames) const {
  const auto num = native_frames * static_cast<size_t>(resamplers_.model_rate_);
  const auto den = static_cast<size_t>(resamplers_.native_rate_);
  return (num + den - 1) / den + RESAMPLE_BOUND_SLACK;
}

size_t DpdfnetProcessor::to_native_frames(size_t model_frames) const {
  const auto num = model_frames * static_cast<size_t>(resamplers_.native_rate_);
  const auto den = static_cast<size_t>(resamplers_.model_rate_);
  return (num + den - 1) / den + RESAMPLE_BOUND_SLACK;
}

bool DpdfnetProcessor::buffers_can_accept(uint32_t frames) const {
  if (realtime_.info_queue.size() + 1 > realtime_.info_queue.capacity())
    return false;
  const size_t input_gain = resample_path_ ? to_model_frames(frames) : frames;
  if (realtime_.input_mono.size() + input_gain >
      realtime_.input_mono.capacity())
    return false;
  const size_t synthesis_gain =
      resample_path_
          ? to_native_frames(realtime_.input_mono.size() + input_gain)
          : realtime_.input_mono.size() + frames;
  if (realtime_.output_mono.size() + synthesis_gain >
      realtime_.output_mono.capacity())
    return false;
  for (size_t channel = 0; channel < channels_; ++channel) {
    if (realtime_.dry_buffers[channel].size() + frames >
        realtime_.dry_buffers[channel].capacity())
      return false;
  }
  return true;
}

bool DpdfnetProcessor::dry_buffers_have_frames(uint32_t frames) const {
  for (size_t channel = 0; channel < channels_; ++channel) {
    if (realtime_.dry_buffers[channel].size() < frames)
      return false;
  }
  return true;
}

bool DpdfnetProcessor::push_input(const DpdfnetAudioPacket &audio) {
  const float *ch0 = audio.data[0];
  for (size_t channel = 0; channel < channels_; ++channel) {
    const float *data = audio.data[channel];
    const float *source =
        data ? data : (ch0 ? ch0 : realtime_.zero_scratch.data());
    if (!realtime_.dry_buffers[channel].try_push(source, audio.frames))
      return false;
  }

  const float *mono = nullptr;
  if (controls_.input_channel >= 0 &&
      static_cast<size_t>(controls_.input_channel) < channels_) {
    const float *selected =
        audio.data[static_cast<size_t>(controls_.input_channel)];
    mono = selected ? selected : (ch0 ? ch0 : realtime_.zero_scratch.data());
  } else {
    realtime_.mono_scratch.resize(audio.frames);
    for (uint32_t frame = 0; frame < audio.frames; ++frame) {
      const float fallback = ch0 ? ch0[frame] : 0.0f;
      float mixed = 0.0f;
      size_t mixed_channels = 0;
      for (size_t channel = 0; channel < channels_; ++channel) {
        const float *data = audio.data[channel];
        if (data) {
          mixed += data[frame];
          ++mixed_channels;
        }
      }
      realtime_.mono_scratch[frame] =
          mixed_channels ? mixed / static_cast<float>(mixed_channels)
                         : fallback;
    }
    mono = realtime_.mono_scratch.data();
  }

  if (!resample_path_) {
    return realtime_.input_mono.try_push(mono, audio.frames);
  }

  uint8_t *out[DPDFNET_MAX_AUDIO_PLANES] = {};
  uint32_t out_frames = 0;
  uint64_t ts_offset = 0;
  const uint8_t *input[DPDFNET_MAX_AUDIO_PLANES] = {
      reinterpret_cast<const uint8_t *>(mono)};
  if (!audio_resampler_resample(resamplers_.input_, out, &out_frames,
                                &ts_offset, input, audio.frames))
    return false;
  if (out_frames)
    return realtime_.input_mono.try_push(
        reinterpret_cast<const float *>(out[0]), out_frames);
  return true;
}

size_t DpdfnetProcessor::process_available_hops() {
  const size_t hop_size = static_cast<size_t>(model_->hop_size());
  const size_t window_size = static_cast<size_t>(model_->n_fft());
  const size_t spec_n = model_->spectrum_size();
  float *noisy_spec = model_->input_spectrum();
  float *enhanced_spec = model_->output_spectrum();
  const float alpha = attenuation_alpha_;
  const float beta = 1.0f - alpha;
  size_t processed_hops = 0;

  while (realtime_.input_mono.size() >= window_size) {
    realtime_.input_mono.peek(realtime_.frame.data(), window_size);
    stft_->analysis(realtime_.frame, noisy_spec);
    model_->enhance();
    for (size_t i = 0; i < spec_n; ++i)
      enhanced_spec[i] = alpha * noisy_spec[i] + beta * enhanced_spec[i];
    stft_->synthesis(enhanced_spec, realtime_.enhanced_hop);

    if (resample_path_) {
      uint8_t *out[DPDFNET_MAX_AUDIO_PLANES] = {};
      uint32_t out_frames = 0;
      uint64_t ts_offset = 0;
      const uint8_t *input[DPDFNET_MAX_AUDIO_PLANES] = {
          reinterpret_cast<const uint8_t *>(realtime_.enhanced_hop.data())};
      if (!audio_resampler_resample(resamplers_.output_, out, &out_frames,
                                    &ts_offset, input,
                                    static_cast<uint32_t>(hop_size)))
        throw std::runtime_error("output resampling failed");
      if (out_frames &&
          !realtime_.output_mono.try_push(
              reinterpret_cast<const float *>(out[0]), out_frames))
        throw std::runtime_error("output buffer capacity exceeded");
    } else {
      if (!realtime_.output_mono.try_push(realtime_.enhanced_hop.data(),
                                          hop_size))
        throw std::runtime_error("output buffer capacity exceeded");
    }
    realtime_.input_mono.pop(hop_size);
    ++processed_hops;
  }
  return processed_hops;
}

DpdfnetProcessResult
DpdfnetProcessor::pop_output_packet(const DpdfnetPacketInfo &info,
                                    size_t processed_hops) {
  realtime_.enhanced_scratch.resize(info.frames);
  realtime_.output_mono.peek(realtime_.enhanced_scratch.data(), info.frames);
  realtime_.output_mono.pop(info.frames);

  DpdfnetProcessResult result;
  result.disposition = DpdfnetDisposition::Processed;
  result.frames = info.frames;
  result.timestamp = info.timestamp > output_latency_ns_
                         ? info.timestamp - output_latency_ns_
                         : 0;
  result.processed_hops = processed_hops;

  const bool need_dry = controls_.bypass || dry_gain_ != 0.0f;
  if (need_dry)
    realtime_.dry_scratch.resize(info.frames);

  for (size_t channel = 0; channel < channels_; ++channel) {
    output_storage_[channel].resize(info.frames);
    if (need_dry) {
      realtime_.dry_buffers[channel].peek(realtime_.dry_scratch.data(),
                                          info.frames);
      realtime_.dry_buffers[channel].pop(info.frames);
    } else {
      realtime_.dry_buffers[channel].pop(info.frames);
    }

    if (controls_.bypass) {
      std::copy(realtime_.dry_scratch.begin(), realtime_.dry_scratch.end(),
                output_storage_[channel].begin());
    } else if (need_dry) {
      for (uint32_t frame = 0; frame < info.frames; ++frame) {
        output_storage_[channel][frame] =
            realtime_.enhanced_scratch[frame] * wet_gain_ +
            realtime_.dry_scratch[frame] * dry_gain_;
      }
    } else if (wet_gain_ == 1.0f) {
      std::copy(realtime_.enhanced_scratch.begin(),
                realtime_.enhanced_scratch.end(),
                output_storage_[channel].begin());
    } else {
      for (uint32_t frame = 0; frame < info.frames; ++frame)
        output_storage_[channel][frame] =
            realtime_.enhanced_scratch[frame] * wet_gain_;
    }
    result.data[channel] = output_storage_[channel].data();
  }

  realtime_.info_queue.pop(1);
  return result;
}

DpdfnetProcessResult DpdfnetProcessor::failure_result(const char *message) {
  ++consecutive_failures_;
  std::snprintf(last_error_.data(), last_error_.size(), "%s", message);
  const bool report = !process_error_reported_;
  process_error_reported_ = true;
  const bool opened = consecutive_failures_ >= MAX_CONSECUTIVE_FAILURES;
  if (opened)
    disable_reason_ = DpdfnetDisableReason::RepeatedProcessingFailures;
  const bool needs_fresh_resamplers = resample_path_;
  if (needs_fresh_resamplers)
    resamplers_valid_ = false;
  reset_audio_state();

  DpdfnetProcessResult result;
  result.disposition = DpdfnetDisposition::Passthrough;
  result.event =
      opened ? DpdfnetEvent::CircuitOpened
             : (report ? DpdfnetEvent::ProcessingFailure : DpdfnetEvent::None);
  result.resampler_refresh_needed = needs_fresh_resamplers && !opened;
  if (result.event != DpdfnetEvent::None)
    std::snprintf(result.message.data(), result.message.size(), "%s", message);
  return result;
}

DpdfnetProcessResult DpdfnetProcessor::capacity_failure_result(uint32_t frames,
                                                               bool oversized) {
  uint64_t &counter = oversized ? oversized_packets_ : capacity_failures_;
  const bool report = counter == 0;
  if (counter != std::numeric_limits<uint64_t>::max())
    ++counter;

  const bool needs_fresh_resamplers =
      !capacity_recovery_pending_ && resample_path_;
  if (needs_fresh_resamplers) {
    resamplers_valid_ = false;
    rate_warning_reported_ = true;
  }
  if (!capacity_recovery_pending_)
    reset_audio_state();
  capacity_recovery_pending_ = true;

  DpdfnetProcessResult result;
  result.resampler_refresh_needed = needs_fresh_resamplers;
  if (!report)
    return result;

  result.event = oversized ? DpdfnetEvent::OversizedPacket
                           : DpdfnetEvent::CapacityInvariantFailure;
  if (oversized) {
    std::snprintf(result.message.data(), result.message.size(),
                  "incoming audio packet has %u frames; the realtime limit is "
                  "%u",
                  frames, MAX_AUDIO_PACKET_FRAMES);
  } else {
    std::snprintf(result.message.data(), result.message.size(),
                  "realtime buffers could not accept a %u-frame audio packet",
                  frames);
  }
  return result;
}

bool DpdfnetProcessor::disable_for_realtime_overload(const char *message) {
  if (disable_reason_ != DpdfnetDisableReason::None)
    return false;

  disable_reason_ = DpdfnetDisableReason::RealtimeOverload;
  consecutive_failures_ = 0;
  process_error_reported_ = false;
  std::snprintf(last_error_.data(), last_error_.size(), "%s", message);
  if (resample_path_)
    resamplers_valid_ = false;
  reset_audio_state();
  return true;
}

DpdfnetProcessResult
DpdfnetProcessor::process(const DpdfnetAudioPacket &audio) {
  DenormalModeGuard denormal_guard;
  DpdfnetProcessResult result;
  if (!audio.frames || !model_ || !stft_ ||
      disable_reason_ != DpdfnetDisableReason::None)
    return result;

  if (audio.frames > MAX_AUDIO_PACKET_FRAMES)
    return capacity_failure_result(audio.frames, true);

  if (sample_rate_ != static_cast<uint32_t>(model_->sample_rate()) &&
      !resample_path_) {
    if (!rate_warning_reported_) {
      result.event = DpdfnetEvent::RateMismatch;
      std::snprintf(result.message.data(), result.message.size(),
                    "OBS sample rate is %u Hz but the model is %d Hz",
                    sample_rate_, model_->sample_rate());
      rate_warning_reported_ = true;
    }
    return result;
  }

  if (timestamp_jump(audio.timestamp)) {
    const bool needs_fresh_resamplers = resample_path_;
    if (needs_fresh_resamplers)
      resamplers_valid_ = false;
    reset_audio_state();
    if (needs_fresh_resamplers) {
      rate_warning_reported_ = true;
      result.event = DpdfnetEvent::ResamplerRefreshNeeded;
      std::snprintf(result.message.data(), result.message.size(),
                    "audio timestamp discontinuity invalidated the stateful "
                    "resamplers");
      result.resampler_refresh_needed = true;
      return result;
    }
  }
  last_timestamp_ = audio.timestamp;
  have_timestamp_ = true;
  expected_timestamp_ =
      audio.timestamp +
      static_cast<uint64_t>(static_cast<double>(audio.frames) /
                            static_cast<double>(sample_rate_) * NS_PER_SECOND);

  if (!buffers_can_accept(audio.frames)) {
    return capacity_failure_result(audio.frames, false);
  }

  if (!realtime_.info_queue.try_push(
          DpdfnetPacketInfo{audio.frames, audio.timestamp})) {
    return capacity_failure_result(audio.frames, false);
  }
  if (!push_input(audio))
    return failure_result("input resampling failed");
  capacity_recovery_pending_ = false;

  size_t processed_hops = 0;
  try {
    processed_hops = process_available_hops();
    if (processed_hops) {
      consecutive_failures_ = 0;
      process_error_reported_ = false;
    }
  } catch (const std::exception &ex) {
    return failure_result(ex.what());
  }

  if (realtime_.info_queue.empty()) {
    result.disposition = DpdfnetDisposition::Pending;
    return result;
  }

  const DpdfnetPacketInfo info = realtime_.info_queue.front();
  if (realtime_.output_mono.size() < info.frames ||
      !dry_buffers_have_frames(info.frames)) {
    result.disposition = DpdfnetDisposition::Pending;
    result.processed_hops = processed_hops;
    return result;
  }
  return pop_output_packet(info, processed_hops);
}

DpdfnetProcessorState DpdfnetProcessor::state() const {
  DpdfnetProcessorState result;
  result.has_model = static_cast<bool>(model_);
  result.resampling = resample_path_;
  result.bypass = controls_.bypass;
  result.disable_reason = disable_reason_;
  result.processing_disabled = disable_reason_ != DpdfnetDisableReason::None;
  result.resampler_refresh_required = model_ && !resamplers_valid_;
  result.capacity_recovery_pending = capacity_recovery_pending_;
  result.sample_rate = sample_rate_;
  result.channels = channels_;
  result.consecutive_failures = consecutive_failures_;
  result.oversized_packets = oversized_packets_;
  result.capacity_failures = capacity_failures_;
  result.last_error = last_error_;
  if (model_) {
    result.model_rate = model_->sample_rate();
    result.n_fft = model_->n_fft();
    result.hop_size = model_->hop_size();
  }
  return result;
}

DpdfnetProcessorSnapshot DpdfnetProcessor::snapshot() const {
  const DpdfnetProcessorState current = state();
  DpdfnetProcessorSnapshot result;
  result.has_model = current.has_model;
  result.resampling = current.resampling;
  result.bypass = current.bypass;
  result.processing_disabled = current.processing_disabled;
  result.disable_reason = current.disable_reason;
  result.resampler_refresh_required = current.resampler_refresh_required;
  result.capacity_recovery_pending = current.capacity_recovery_pending;
  result.sample_rate = current.sample_rate;
  result.channels = current.channels;
  result.model_rate = current.model_rate;
  result.n_fft = current.n_fft;
  result.hop_size = current.hop_size;
  result.consecutive_failures = current.consecutive_failures;
  result.oversized_packets = current.oversized_packets;
  result.capacity_failures = current.capacity_failures;
  result.last_error = current.last_error.data();
  if (model_) {
    result.model_path = model_->path().string();
    result.model_name = model_->name();
  }
  return result;
}
