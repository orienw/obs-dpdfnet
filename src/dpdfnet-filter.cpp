// SPDX-License-Identifier: GPL-2.0-or-later

#include "dpdfnet-model.hpp"
#include "stft.hpp"

#include <obs-module.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {
constexpr const char *SETTING_MODEL_PATH = "model_path";
constexpr const char *SETTING_INPUT_CHANNEL = "input_channel";
constexpr const char *SETTING_ATTENUATION_LIMIT_DB = "attenuation_limit_db";
constexpr const char *SETTING_WET_MIX = "wet_mix";
constexpr const char *SETTING_OUTPUT_GAIN_DB = "output_gain_db";
constexpr const char *SETTING_BYPASS = "bypass";

constexpr uint64_t NS_PER_SECOND = 1000000000ULL;

// Generous fixed capacity (~340 ms at 48 kHz) so the audio-thread ring buffers
// never reallocate in steady state; the grow path only fires for a pathological
// oversized packet.
constexpr size_t RING_RESERVE_SAMPLES = 16384;
constexpr size_t INFO_RESERVE = 256;

struct PacketInfo {
  uint32_t frames = 0;
  uint64_t timestamp = 0;
};

// Fixed-capacity FIFO over a contiguous buffer. Unlike std::deque it allocates
// and frees no node blocks under steady push/pop, so it is safe to drive from
// the OBS audio callback. T must be trivially copyable (float, PacketInfo).
template <typename T> class Ring {
  static_assert(std::is_trivially_copyable_v<T>,
                "Ring requires a trivially copyable element type");

public:
  void reserve(size_t cap) {
    if (cap <= buf_.size())
      return;
    std::vector<T> next(cap);
    if (count_) {
      const size_t cur = buf_.size();
      const size_t first = std::min(count_, cur - head_);
      std::memcpy(next.data(), &buf_[head_], first * sizeof(T));
      if (count_ > first)
        std::memcpy(next.data() + first, buf_.data(),
                    (count_ - first) * sizeof(T));
    }
    buf_.swap(next);
    head_ = 0;
  }

  void clear() {
    head_ = 0;
    count_ = 0;
  }

  size_t size() const { return count_; }
  size_t capacity() const { return buf_.size(); }
  bool empty() const { return count_ == 0; }
  const T &front() const { return buf_[head_]; }

  void push(const T *src, size_t n) {
    if (!n)
      return;
    ensure(count_ + n);
    const size_t cap = buf_.size();
    const size_t tail = (head_ + count_) % cap;
    const size_t first = std::min(n, cap - tail);
    std::memcpy(&buf_[tail], src, first * sizeof(T));
    if (n > first)
      std::memcpy(buf_.data(), src + first, (n - first) * sizeof(T));
    count_ += n;
  }

  void push(const T &value) { push(&value, 1); }

  void peek(T *dst, size_t n) const {
    const size_t cap = buf_.size();
    const size_t first = std::min(n, cap - head_);
    std::memcpy(dst, &buf_[head_], first * sizeof(T));
    if (n > first)
      std::memcpy(dst + first, buf_.data(), (n - first) * sizeof(T));
  }

  void pop(size_t n) {
    head_ = (head_ + n) % buf_.size();
    count_ -= n;
  }

private:
  void ensure(size_t need) {
    if (need > buf_.size())
      reserve(std::max(need, buf_.size() * 2 + 64));
  }

  std::vector<T> buf_;
  size_t head_ = 0;
  size_t count_ = 0;
};

std::string default_model_path() {
  char *path = obs_module_file("models/dpdfnet8_48khz_hr.onnx");
  if (!path)
    return {};

  std::string out(path);
  bfree(path);
  return out;
}

float db_to_amp(double db) {
  return static_cast<float>(std::pow(10.0, db / 20.0));
}

class DpdfnetFilter {
public:
  explicit DpdfnetFilter(obs_source_t *) {}

  void update(obs_data_t *settings) {
    // Serialize updates with each other (never with the audio thread, which
    // only takes mutex_) so a slow model build cannot be clobbered by a newer
    // update that finishes first.
    std::lock_guard<std::mutex> update_lock(update_mutex_);

    const char *model_path = obs_data_get_string(settings, SETTING_MODEL_PATH);
    std::string new_model_path = model_path ? model_path : "";
    if (new_model_path.empty())
      new_model_path = default_model_path();

    const double atten =
        obs_data_get_double(settings, SETTING_ATTENUATION_LIMIT_DB);
    const int channel =
        static_cast<int>(obs_data_get_int(settings, SETTING_INPUT_CHANNEL));
    const double wet = std::clamp(
        obs_data_get_double(settings, SETTING_WET_MIX) / 100.0, 0.0, 1.0);
    const float gain =
        db_to_amp(obs_data_get_double(settings, SETTING_OUTPUT_GAIN_DB));
    const bool bypass = obs_data_get_bool(settings, SETTING_BYPASS);

    const uint32_t obs_rate = audio_output_get_sample_rate(obs_get_audio());
    const size_t obs_channels = std::clamp<size_t>(
        audio_output_get_channels(obs_get_audio()), 1, MAX_AV_PLANES);

    bool need_load;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      need_load = new_model_path != loaded_model_path_;
    }

    // Build the new session OFF the audio lock: constructing an Ort::Session
    // for a multi-MB model is slow, and holding mutex_ through it would stall
    // the OBS audio callback for the entire load on every settings change.
    std::unique_ptr<DpdfnetModel> new_model;
    std::unique_ptr<StreamingStft> new_stft;
    bool load_attempted = false;
    if (need_load && !new_model_path.empty()) {
      load_attempted = true;
      try {
        new_model = std::make_unique<DpdfnetModel>(new_model_path);
        new_stft = std::make_unique<StreamingStft>(new_model->n_fft(),
                                                   new_model->hop_size());
      } catch (const std::exception &ex) {
        new_model.reset();
        new_stft.reset();
        blog(LOG_ERROR, "[obs-dpdfnet] failed to load model '%s': %s",
             new_model_path.c_str(), ex.what());
      }
    }

    // Old session objects are moved out under the lock and destroyed AFTER it
    // releases: ~Ort::Session / IoBinding teardown is not instant, and running
    // it under mutex_ would stall the audio callback the same way the load did.
    std::unique_ptr<DpdfnetModel> old_model;
    std::unique_ptr<StreamingStft> old_stft;

    {
      std::lock_guard<std::mutex> lock(mutex_);

      attenuation_limit_db_ = atten;
      input_channel_ = channel;
      wet_mix_ = wet;
      output_gain_ = gain;
      bypass_ = bypass;
      recompute_mix_locked();

      if (obs_rate != sample_rate_ || obs_channels != channels_ ||
          dry_buffers_.size() != obs_channels ||
          output_storage_.size() != obs_channels) {
        sample_rate_ = obs_rate;
        channels_ = obs_channels;
        resize_channel_buffers();
        reset_stream_locked();
        rate_warning_logged_ = false;
      }

      if (load_attempted) {
        if (new_model) {
          old_model = std::move(model_);
          old_stft = std::move(stft_);
          model_ = std::move(new_model);
          stft_ = std::move(new_stft);
          loaded_model_path_ = new_model_path;
          resize_model_buffers_locked();
          recompute_latency_locked();
          reset_stream_locked();
          blog(LOG_INFO,
               "[obs-dpdfnet] loaded %s (model %s, metadata profile %s, %d Hz, "
               "hop %d)",
               loaded_model_path_.c_str(), model_->name().c_str(),
               model_->profile().c_str(), model_->sample_rate(),
               model_->hop_size());
        } else {
          blog(LOG_WARNING,
               "[obs-dpdfnet] keeping previously loaded model after failed load: "
               "%s",
               loaded_model_path_.empty() ? "(none)" : loaded_model_path_.c_str());
        }
      }

      blog(LOG_INFO,
           "[obs-dpdfnet] settings: input=%d max_suppression=%.1f dB "
           "wet=%.0f%% gain=%.2f bypass=%s",
           input_channel_, attenuation_limit_db_, wet_mix_ * 100.0,
           output_gain_, bypass_ ? "true" : "false");
    }
  }

  struct obs_audio_data *filter_audio(struct obs_audio_data *audio) {
    if (!audio || !audio->frames)
      return audio;

    std::lock_guard<std::mutex> lock(mutex_);

    if (bypass_ || !model_ || !stft_)
      return audio;

    if (sample_rate_ != static_cast<uint32_t>(model_->sample_rate())) {
      if (!rate_warning_logged_) {
        blog(LOG_WARNING,
             "[obs-dpdfnet] OBS sample rate is %u Hz but the loaded model is "
             "%d Hz; bypassing",
             sample_rate_, model_->sample_rate());
        rate_warning_logged_ = true;
      }
      return audio;
    }

    if (timestamp_jump(audio->timestamp))
      reset_stream_locked();
    last_timestamp_ = audio->timestamp;

    // Never grow a ring on the audio thread. If this packet would not fit the
    // preallocated capacity (a pathological multi-hundred-ms packet, or a
    // backlog that never drained), drop the buffered state and pass it through
    // dry rather than allocate inside the callback.
    if (!buffers_can_accept(audio->frames)) {
      reset_stream_locked();
      return audio;
    }

    info_queue_.push(PacketInfo{audio->frames, audio->timestamp});
    push_input(audio);

    try {
      process_available_hops();
    } catch (const std::exception &ex) {
      blog(LOG_ERROR, "[obs-dpdfnet] processing failed: %s", ex.what());
      reset_stream_locked();
      return audio;
    }

    if (info_queue_.empty())
      return nullptr;

    const PacketInfo info = info_queue_.front();
    if (output_mono_.size() < info.frames ||
        !dry_buffers_have_frames(info.frames))
      return nullptr;

    return pop_output_packet(info);
  }

  void reset_state() {
    std::lock_guard<std::mutex> lock(mutex_);
    reset_stream_locked();
  }

private:
  bool timestamp_jump(uint64_t timestamp) const {
    if (!last_timestamp_)
      return false;

    const uint64_t diff = timestamp > last_timestamp_
                              ? timestamp - last_timestamp_
                              : last_timestamp_ - timestamp;
    return diff > NS_PER_SECOND;
  }

  void recompute_mix_locked() {
    const double limit = std::clamp(attenuation_limit_db_, 0.0, 60.0);
    attenuation_alpha_ = db_to_amp(-limit);
    dry_gain_ = static_cast<float>((1.0 - wet_mix_) * output_gain_);
    wet_gain_ = static_cast<float>(wet_mix_ * output_gain_);
  }

  void recompute_latency_locked() {
    if (model_)
      hop_latency_ns_ = static_cast<uint64_t>(
          static_cast<double>(model_->hop_size()) /
          static_cast<double>(model_->sample_rate()) * NS_PER_SECOND);
  }

  void resize_channel_buffers() {
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

    output_audio_ = {};
  }

  void resize_model_buffers_locked() {
    if (!model_)
      return;
    frame_.assign(static_cast<size_t>(model_->n_fft()), 0.0f);
    enhanced_hop_.assign(static_cast<size_t>(model_->hop_size()), 0.0f);
  }

  void reset_stream_locked() {
    input_mono_.clear();
    output_mono_.clear();
    for (auto &buffer : dry_buffers_)
      buffer.clear();
    info_queue_.clear();

    if (model_)
      model_->reset();
    if (stft_)
      stft_->reset();
  }

  bool dry_buffers_have_frames(uint32_t frames) const {
    if (dry_buffers_.size() < channels_)
      return false;

    for (size_t channel = 0; channel < channels_; ++channel) {
      if (dry_buffers_[channel].size() < frames)
        return false;
    }

    return true;
  }

  // True if buffering this packet stays within every ring's preallocated
  // capacity, so no push() reallocates on the audio thread.
  bool buffers_can_accept(uint32_t frames) const {
    if (info_queue_.size() + 1 > info_queue_.capacity())
      return false;
    if (input_mono_.size() + frames > input_mono_.capacity())
      return false;
    // Upper bound on what synthesis can append before this packet drains.
    if (output_mono_.size() + input_mono_.size() + frames >
        output_mono_.capacity())
      return false;
    for (size_t channel = 0; channel < channels_; ++channel)
      if (dry_buffers_[channel].size() + frames >
          dry_buffers_[channel].capacity())
        return false;
    return true;
  }

  void push_input(struct obs_audio_data *audio) {
    const uint32_t frames = audio->frames;
    const float *ch0 = reinterpret_cast<const float *>(audio->data[0]);

    if (zero_scratch_.size() < frames)
      zero_scratch_.assign(frames, 0.0f);

    for (size_t channel = 0; channel < channels_; ++channel) {
      const float *data =
          reinterpret_cast<const float *>(audio->data[channel]);
      if (data)
        dry_buffers_[channel].push(data, frames);
      else if (ch0)
        dry_buffers_[channel].push(ch0, frames);
      else
        dry_buffers_[channel].push(zero_scratch_.data(), frames);
    }

    if (input_channel_ >= 0 &&
        static_cast<size_t>(input_channel_) < channels_) {
      const auto selected_channel = static_cast<size_t>(input_channel_);
      const float *selected =
          reinterpret_cast<const float *>(audio->data[selected_channel]);
      if (selected)
        input_mono_.push(selected, frames);
      else if (ch0)
        input_mono_.push(ch0, frames);
      else
        input_mono_.push(zero_scratch_.data(), frames);
      return;
    }

    mono_scratch_.resize(frames);
    for (uint32_t frame = 0; frame < frames; ++frame) {
      const float fallback = ch0 ? ch0[frame] : 0.0f;
      float mixed = 0.0f;
      size_t mixed_channels = 0;

      for (size_t channel = 0; channel < channels_; ++channel) {
        const float *data =
            reinterpret_cast<const float *>(audio->data[channel]);
        const float sample = data ? data[frame] : fallback;
        if (data) {
          mixed += sample;
          ++mixed_channels;
        }
      }

      mono_scratch_[frame] =
          mixed_channels ? mixed / static_cast<float>(mixed_channels) : fallback;
    }

    input_mono_.push(mono_scratch_.data(), frames);
  }

  void process_available_hops() {
    const size_t hop_size = static_cast<size_t>(model_->hop_size());
    const size_t window_size = static_cast<size_t>(model_->n_fft());
    const size_t spec_n = model_->spectrum_size();
    float *noisy_spec = model_->input_spectrum();
    float *enhanced_spec = model_->output_spectrum();
    const float alpha = attenuation_alpha_;
    const float beta = 1.0f - alpha;

    while (input_mono_.size() >= window_size) {
      input_mono_.peek(frame_.data(), window_size);

      stft_->analysis(frame_, noisy_spec);
      model_->enhance();
      for (size_t i = 0; i < spec_n; ++i)
        enhanced_spec[i] = alpha * noisy_spec[i] + beta * enhanced_spec[i];
      stft_->synthesis(enhanced_spec, enhanced_hop_);

      output_mono_.push(enhanced_hop_.data(), hop_size);
      input_mono_.pop(hop_size);
    }
  }

  struct obs_audio_data *pop_output_packet(const PacketInfo &info) {
    enhanced_scratch_.resize(info.frames);
    output_mono_.peek(enhanced_scratch_.data(), info.frames);
    output_mono_.pop(info.frames);

    output_audio_ = {};
    const bool mix_dry = dry_gain_ != 0.0f;
    if (mix_dry)
      dry_scratch_.resize(info.frames);

    for (size_t channel = 0; channel < channels_; ++channel) {
      output_storage_[channel].resize(info.frames);

      if (mix_dry) {
        dry_buffers_[channel].peek(dry_scratch_.data(), info.frames);
        dry_buffers_[channel].pop(info.frames);

        for (uint32_t frame = 0; frame < info.frames; ++frame) {
          output_storage_[channel][frame] = enhanced_scratch_[frame] * wet_gain_ +
                                           dry_scratch_[frame] * dry_gain_;
        }
      } else {
        dry_buffers_[channel].pop(info.frames);
        if (wet_gain_ == 1.0f) {
          std::copy(enhanced_scratch_.begin(), enhanced_scratch_.end(),
                    output_storage_[channel].begin());
        } else {
          for (uint32_t frame = 0; frame < info.frames; ++frame)
            output_storage_[channel][frame] =
                enhanced_scratch_[frame] * wet_gain_;
        }
      }

      output_audio_.data[channel] =
          reinterpret_cast<uint8_t *>(output_storage_[channel].data());
    }

    output_audio_.frames = info.frames;
    // Guard against unsigned underflow if a source stamps timestamps near zero.
    output_audio_.timestamp = info.timestamp > hop_latency_ns_
                                  ? info.timestamp - hop_latency_ns_
                                  : 0;

    info_queue_.pop(1);
    return &output_audio_;
  }

  std::mutex mutex_;
  std::mutex update_mutex_;

  std::string loaded_model_path_;
  std::unique_ptr<DpdfnetModel> model_;
  std::unique_ptr<StreamingStft> stft_;

  uint32_t sample_rate_ = 0;
  size_t channels_ = 0;
  uint64_t last_timestamp_ = 0;
  uint64_t hop_latency_ns_ = 0;

  double attenuation_limit_db_ = 24.0;
  int input_channel_ = 0;
  double wet_mix_ = 1.0;
  float output_gain_ = 1.0f;
  bool bypass_ = false;
  bool rate_warning_logged_ = false;

  float attenuation_alpha_ = 0.0f;
  float dry_gain_ = 0.0f;
  float wet_gain_ = 1.0f;

  Ring<PacketInfo> info_queue_;
  Ring<float> input_mono_;
  Ring<float> output_mono_;
  std::vector<Ring<float>> dry_buffers_;
  std::vector<std::vector<float>> output_storage_;

  std::vector<float> frame_;
  std::vector<float> enhanced_hop_;
  std::vector<float> mono_scratch_;
  std::vector<float> dry_scratch_;
  std::vector<float> enhanced_scratch_;
  std::vector<float> zero_scratch_;

  struct obs_audio_data output_audio_ = {};
};

const char *filter_name(void *) { return obs_module_text("DPDFNet"); }

void *filter_create(obs_data_t *settings, obs_source_t *source) {
  auto *filter = new DpdfnetFilter(source);
  filter->update(settings);
  return filter;
}

void filter_destroy(void *data) { delete static_cast<DpdfnetFilter *>(data); }

void filter_update(void *data, obs_data_t *settings) {
  static_cast<DpdfnetFilter *>(data)->update(settings);
}

struct obs_audio_data *filter_audio(void *data, struct obs_audio_data *audio) {
  return static_cast<DpdfnetFilter *>(data)->filter_audio(audio);
}

void filter_defaults(obs_data_t *settings) {
  obs_data_set_default_string(settings, SETTING_MODEL_PATH,
                              default_model_path().c_str());
  obs_data_set_default_int(settings, SETTING_INPUT_CHANNEL, 0);
  obs_data_set_default_double(settings, SETTING_ATTENUATION_LIMIT_DB, 24.0);
  obs_data_set_default_double(settings, SETTING_WET_MIX, 100.0);
  obs_data_set_default_double(settings, SETTING_OUTPUT_GAIN_DB, 0.0);
  obs_data_set_default_bool(settings, SETTING_BYPASS, false);
}

bool reset_clicked(obs_properties_t *, obs_property_t *, void *data) {
  if (data)
    static_cast<DpdfnetFilter *>(data)->reset_state();
  return false;
}

obs_properties_t *filter_properties(void *data) {
  obs_properties_t *props = obs_properties_create();
  obs_properties_add_text(props, "info", obs_module_text("DPDFNet.Info"),
                          OBS_TEXT_INFO);

  obs_properties_add_path(props, SETTING_MODEL_PATH,
                          obs_module_text("DPDFNet.ModelPath"), OBS_PATH_FILE,
                          "ONNX model (*.onnx);;All files (*.*)", nullptr);

  obs_property_t *input_channel = obs_properties_add_list(
      props, SETTING_INPUT_CHANNEL, obs_module_text("DPDFNet.InputChannel"),
      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
  obs_property_list_add_int(input_channel,
                            obs_module_text("DPDFNet.InputChannel.Input1"), 0);
  obs_property_list_add_int(input_channel,
                            obs_module_text("DPDFNet.InputChannel.Input2"), 1);
  obs_property_list_add_int(input_channel,
                            obs_module_text("DPDFNet.InputChannel.Mix"), -1);

  obs_property_t *attenuation = obs_properties_add_float_slider(
      props, SETTING_ATTENUATION_LIMIT_DB,
      obs_module_text("DPDFNet.AttenuationLimit"), 0.0, 60.0, 0.5);
  obs_property_float_set_suffix(attenuation, " dB");

  obs_property_t *wet = obs_properties_add_float_slider(
      props, SETTING_WET_MIX, obs_module_text("DPDFNet.WetMix"), 0.0, 100.0,
      1.0);
  obs_property_float_set_suffix(wet, "%");

  obs_property_t *gain = obs_properties_add_float_slider(
      props, SETTING_OUTPUT_GAIN_DB, obs_module_text("DPDFNet.OutputGain"),
      -12.0, 12.0, 0.1);
  obs_property_float_set_suffix(gain, " dB");

  obs_properties_add_bool(props, SETTING_BYPASS,
                          obs_module_text("DPDFNet.Bypass"));
  obs_properties_add_button2(props, "reset_state",
                             obs_module_text("DPDFNet.ResetState"),
                             reset_clicked, data);

  return props;
}
} // namespace

struct obs_source_info dpdfnet_filter_info = [] {
  struct obs_source_info info = {};
  info.id = "obs_dpdfnet_filter";
  info.type = OBS_SOURCE_TYPE_FILTER;
  info.output_flags = OBS_SOURCE_AUDIO;
  info.get_name = filter_name;
  info.create = filter_create;
  info.destroy = filter_destroy;
  info.update = filter_update;
  info.filter_audio = filter_audio;
  info.get_defaults = filter_defaults;
  info.get_properties = filter_properties;
  return info;
}();
