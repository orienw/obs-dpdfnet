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

DpdfnetModelBundle prepare_dpdfnet_model(const std::string &path) {
  DpdfnetModelBundle bundle;
  bundle.model = std::make_unique<DpdfnetModel>(path);
  std::fill_n(bundle.model->input_spectrum(), bundle.model->spectrum_size(),
              0.0f);
  bundle.model->enhance();
  bundle.model->reset();
  bundle.stft = std::make_unique<StreamingStft>(bundle.model->n_fft(),
                                                bundle.model->hop_size());
  return bundle;
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

DpdfnetModelBundle DpdfnetProcessor::replace_model(DpdfnetModelBundle bundle) {
  DpdfnetModelBundle old{std::move(model_), std::move(stft_)};
  model_ = std::move(bundle.model);
  stft_ = std::move(bundle.stft);
  resize_model_buffers();
  processing_disabled_ = false;
  consecutive_failures_ = 0;
  process_error_reported_ = false;
  last_error_.fill(0);
  reset_audio_state();
  return old;
}

DpdfnetResamplers
DpdfnetProcessor::replace_resamplers(DpdfnetResamplers resamplers) {
  if (!resamplers_ && !resamplers) {
    resamplers_valid_ = true;
    return {};
  }
  DpdfnetResamplers old = std::move(resamplers_);
  resamplers_ = std::move(resamplers);
  resamplers_valid_ = true;
  rate_warning_reported_ = false;
  reset_audio_state();
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
  resize_channel_buffers();
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
  processing_disabled_ = false;
  consecutive_failures_ = 0;
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

void DpdfnetProcessor::resize_channel_buffers() {
  input_mono_.reserve(RING_RESERVE_SAMPLES);
  output_mono_.reserve(RING_RESERVE_SAMPLES);
  info_queue_.reserve(INFO_RESERVE);

  dry_buffers_.assign(channels_, {});
  for (auto &buffer : dry_buffers_)
    buffer.reserve(RING_RESERVE_SAMPLES);

  output_storage_.assign(channels_, {});
  for (auto &storage : output_storage_)
    storage.reserve(RING_RESERVE_SAMPLES);

  mono_scratch_.reserve(RING_RESERVE_SAMPLES);
  dry_scratch_.reserve(RING_RESERVE_SAMPLES);
  enhanced_scratch_.reserve(RING_RESERVE_SAMPLES);
  zero_scratch_.assign(RING_RESERVE_SAMPLES, 0.0f);
}

void DpdfnetProcessor::resize_model_buffers() {
  if (!model_) {
    frame_.clear();
    enhanced_hop_.clear();
    return;
  }
  frame_.assign(static_cast<size_t>(model_->n_fft()), 0.0f);
  enhanced_hop_.assign(static_cast<size_t>(model_->hop_size()), 0.0f);
}

void DpdfnetProcessor::reset_audio_state() {
  recompute_path();
  recompute_latency();
  input_mono_.clear();
  output_mono_.clear();
  for (auto &buffer : dry_buffers_)
    buffer.clear();
  info_queue_.clear();
  last_timestamp_ = 0;
  expected_timestamp_ = 0;
  have_timestamp_ = false;

  if (resample_path_ && resamplers_.prefill_frames_) {
    for (auto &buffer : dry_buffers_)
      buffer.push(zero_scratch_.data(), resamplers_.prefill_frames_);
  }
  if (model_)
    model_->reset();
  if (stft_)
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
  if (info_queue_.size() + 1 > info_queue_.capacity())
    return false;
  if (resample_path_ && frames > MAX_RESAMPLE_INPUT_FRAMES)
    return false;
  const size_t input_gain = resample_path_ ? to_model_frames(frames) : frames;
  if (input_mono_.size() + input_gain > input_mono_.capacity())
    return false;
  const size_t synthesis_gain =
      resample_path_ ? to_native_frames(input_mono_.size() + input_gain)
                     : input_mono_.size() + frames;
  if (output_mono_.size() + synthesis_gain > output_mono_.capacity())
    return false;
  for (const auto &buffer : dry_buffers_) {
    if (buffer.size() + frames > buffer.capacity())
      return false;
  }
  return true;
}

bool DpdfnetProcessor::dry_buffers_have_frames(uint32_t frames) const {
  if (dry_buffers_.size() < channels_)
    return false;
  for (const auto &buffer : dry_buffers_) {
    if (buffer.size() < frames)
      return false;
  }
  return true;
}

bool DpdfnetProcessor::push_input(const DpdfnetAudioPacket &audio) {
  const float *ch0 = audio.data[0];
  for (size_t channel = 0; channel < channels_; ++channel) {
    const float *data = audio.data[channel];
    if (data)
      dry_buffers_[channel].push(data, audio.frames);
    else if (ch0)
      dry_buffers_[channel].push(ch0, audio.frames);
    else
      dry_buffers_[channel].push(zero_scratch_.data(), audio.frames);
  }

  const float *mono = nullptr;
  if (controls_.input_channel >= 0 &&
      static_cast<size_t>(controls_.input_channel) < channels_) {
    const float *selected =
        audio.data[static_cast<size_t>(controls_.input_channel)];
    mono = selected ? selected : (ch0 ? ch0 : zero_scratch_.data());
  } else {
    mono_scratch_.resize(audio.frames);
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
      mono_scratch_[frame] = mixed_channels
                                 ? mixed / static_cast<float>(mixed_channels)
                                 : fallback;
    }
    mono = mono_scratch_.data();
  }

  if (!resample_path_) {
    input_mono_.push(mono, audio.frames);
    return true;
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
    input_mono_.push(reinterpret_cast<const float *>(out[0]), out_frames);
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

  while (input_mono_.size() >= window_size) {
    input_mono_.peek(frame_.data(), window_size);
    stft_->analysis(frame_, noisy_spec);
    model_->enhance();
    for (size_t i = 0; i < spec_n; ++i)
      enhanced_spec[i] = alpha * noisy_spec[i] + beta * enhanced_spec[i];
    stft_->synthesis(enhanced_spec, enhanced_hop_);

    if (resample_path_) {
      uint8_t *out[DPDFNET_MAX_AUDIO_PLANES] = {};
      uint32_t out_frames = 0;
      uint64_t ts_offset = 0;
      const uint8_t *input[DPDFNET_MAX_AUDIO_PLANES] = {
          reinterpret_cast<const uint8_t *>(enhanced_hop_.data())};
      if (!audio_resampler_resample(resamplers_.output_, out, &out_frames,
                                    &ts_offset, input,
                                    static_cast<uint32_t>(hop_size)))
        throw std::runtime_error("output resampling failed");
      if (out_frames)
        output_mono_.push(reinterpret_cast<const float *>(out[0]), out_frames);
    } else {
      output_mono_.push(enhanced_hop_.data(), hop_size);
    }
    input_mono_.pop(hop_size);
    ++processed_hops;
  }
  return processed_hops;
}

DpdfnetProcessResult
DpdfnetProcessor::pop_output_packet(const PacketInfo &info,
                                    size_t processed_hops) {
  enhanced_scratch_.resize(info.frames);
  output_mono_.peek(enhanced_scratch_.data(), info.frames);
  output_mono_.pop(info.frames);

  DpdfnetProcessResult result;
  result.disposition = DpdfnetDisposition::Processed;
  result.frames = info.frames;
  result.timestamp = info.timestamp > output_latency_ns_
                         ? info.timestamp - output_latency_ns_
                         : 0;
  result.processed_hops = processed_hops;

  const bool need_dry = controls_.bypass || dry_gain_ != 0.0f;
  if (need_dry)
    dry_scratch_.resize(info.frames);

  for (size_t channel = 0; channel < channels_; ++channel) {
    output_storage_[channel].resize(info.frames);
    if (need_dry) {
      dry_buffers_[channel].peek(dry_scratch_.data(), info.frames);
      dry_buffers_[channel].pop(info.frames);
    } else {
      dry_buffers_[channel].pop(info.frames);
    }

    if (controls_.bypass) {
      std::copy(dry_scratch_.begin(), dry_scratch_.end(),
                output_storage_[channel].begin());
    } else if (need_dry) {
      for (uint32_t frame = 0; frame < info.frames; ++frame) {
        output_storage_[channel][frame] = enhanced_scratch_[frame] * wet_gain_ +
                                          dry_scratch_[frame] * dry_gain_;
      }
    } else if (wet_gain_ == 1.0f) {
      std::copy(enhanced_scratch_.begin(), enhanced_scratch_.end(),
                output_storage_[channel].begin());
    } else {
      for (uint32_t frame = 0; frame < info.frames; ++frame)
        output_storage_[channel][frame] = enhanced_scratch_[frame] * wet_gain_;
    }
    result.data[channel] = output_storage_[channel].data();
  }

  info_queue_.pop(1);
  return result;
}

DpdfnetProcessResult DpdfnetProcessor::failure_result(const char *message) {
  ++consecutive_failures_;
  std::snprintf(last_error_.data(), last_error_.size(), "%s", message);
  const bool report = !process_error_reported_;
  process_error_reported_ = true;
  const bool opened = consecutive_failures_ >= MAX_CONSECUTIVE_FAILURES;
  processing_disabled_ = opened;
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

DpdfnetProcessResult
DpdfnetProcessor::process(const DpdfnetAudioPacket &audio) {
  DenormalModeGuard denormal_guard;
  DpdfnetProcessResult result;
  if (!audio.frames || !model_ || !stft_ || processing_disabled_)
    return result;

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
    reset_audio_state();
    return result;
  }

  info_queue_.push(PacketInfo{audio.frames, audio.timestamp});
  if (!push_input(audio))
    return failure_result("input resampling failed");

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

  if (info_queue_.empty()) {
    result.disposition = DpdfnetDisposition::Pending;
    return result;
  }

  const PacketInfo info = info_queue_.front();
  if (output_mono_.size() < info.frames ||
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
  result.processing_disabled = processing_disabled_;
  result.resampler_refresh_required = model_ && !resamplers_valid_;
  result.sample_rate = sample_rate_;
  result.channels = channels_;
  result.consecutive_failures = consecutive_failures_;
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
  result.resampler_refresh_required = current.resampler_refresh_required;
  result.sample_rate = current.sample_rate;
  result.channels = current.channels;
  result.model_rate = current.model_rate;
  result.n_fft = current.n_fft;
  result.hop_size = current.hop_size;
  result.consecutive_failures = current.consecutive_failures;
  result.last_error = current.last_error.data();
  if (model_) {
    result.model_path = model_->path().string();
    result.model_name = model_->name();
  }
  return result;
}
