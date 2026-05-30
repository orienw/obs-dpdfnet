// SPDX-License-Identifier: GPL-2.0-or-later

#include "dpdfnet-model.hpp"
#include "stft.hpp"

#include <obs-module.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr const char *SETTING_MODEL_PATH = "model_path";
constexpr const char *SETTING_INPUT_CHANNEL = "input_channel";
constexpr const char *SETTING_ATTENUATION_LIMIT_DB = "attenuation_limit_db";
constexpr const char *SETTING_WET_MIX = "wet_mix";
constexpr const char *SETTING_OUTPUT_GAIN_DB = "output_gain_db";
constexpr const char *SETTING_BYPASS = "bypass";

constexpr uint64_t NS_PER_SECOND = 1000000000ULL;
constexpr size_t MAX_DIAGNOSTIC_PACKETS = 8;
constexpr size_t MAX_DIAGNOSTIC_WINDOWS = 60;

struct PacketInfo {
  uint32_t frames = 0;
  uint64_t timestamp = 0;
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

void clear_deque(std::deque<float> &queue) {
  std::deque<float> empty;
  queue.swap(empty);
}

void clear_packet_queue(std::deque<PacketInfo> &queue) {
  std::deque<PacketInfo> empty;
  queue.swap(empty);
}

class DpdfnetFilter {
public:
  explicit DpdfnetFilter(obs_source_t *source) : source_(source) {}

  void update(obs_data_t *settings) {
    std::lock_guard<std::mutex> lock(mutex_);

    const char *model_path = obs_data_get_string(settings, SETTING_MODEL_PATH);
    model_path_ = model_path ? model_path : "";
    if (model_path_.empty())
      model_path_ = default_model_path();

    attenuation_limit_db_ =
        obs_data_get_double(settings, SETTING_ATTENUATION_LIMIT_DB);
    input_channel_ = static_cast<int>(
        obs_data_get_int(settings, SETTING_INPUT_CHANNEL));
    wet_mix_ = std::clamp(
        obs_data_get_double(settings, SETTING_WET_MIX) / 100.0, 0.0, 1.0);
    output_gain_ =
        db_to_amp(obs_data_get_double(settings, SETTING_OUTPUT_GAIN_DB));
    bypass_ = obs_data_get_bool(settings, SETTING_BYPASS);

    const uint32_t obs_rate = audio_output_get_sample_rate(obs_get_audio());
    const size_t obs_channels = std::clamp<size_t>(
        audio_output_get_channels(obs_get_audio()), 1, MAX_AV_PLANES);
    if (obs_rate != sample_rate_ || obs_channels != channels_ ||
        dry_buffers_.size() != obs_channels ||
        output_storage_.size() != obs_channels) {
      sample_rate_ = obs_rate;
      channels_ = obs_channels;
      resize_channel_buffers();
      reset_stream_locked();
      rate_warning_logged_ = false;
    }

    if (model_path_ != loaded_model_path_) {
      load_model_locked();
    }

    blog(LOG_INFO,
         "[obs-dpdfnet] settings: input=%d max_suppression=%.1f dB "
         "wet=%.0f%% gain=%.2f bypass=%s",
         input_channel_, attenuation_limit_db_, wet_mix_ * 100.0,
         output_gain_, bypass_ ? "true" : "false");
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

    info_queue_.push_back(PacketInfo{audio->frames, audio->timestamp});
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

    const PacketInfo &info = info_queue_.front();
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

  void resize_channel_buffers() {
    dry_buffers_.assign(channels_, {});
    output_storage_.assign(channels_, {});
    output_audio_ = {};
  }

  void reset_stream_locked() {
    clear_deque(input_mono_);
    clear_deque(output_mono_);
    for (auto &buffer : dry_buffers_)
      clear_deque(buffer);
    clear_packet_queue(info_queue_);
    diagnostic_packets_logged_ = 0;
    reset_diagnostic_window();

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

  void load_model_locked() {
    model_.reset();
    stft_.reset();
    loaded_model_path_.clear();

    try {
      model_ = std::make_unique<DpdfnetModel>(model_path_);
      stft_ =
          std::make_unique<StreamingStft>(model_->n_fft(), model_->hop_size());
      loaded_model_path_ = model_path_;
      reset_stream_locked();

      blog(LOG_INFO, "[obs-dpdfnet] loaded %s (%s, %d Hz, hop %d)",
           loaded_model_path_.c_str(), model_->profile().c_str(),
           model_->sample_rate(), model_->hop_size());
    } catch (const std::exception &ex) {
      model_.reset();
      stft_.reset();
      loaded_model_path_.clear();
      blog(LOG_ERROR, "[obs-dpdfnet] failed to load model '%s': %s",
           model_path_.c_str(), ex.what());
    }
  }

  void push_input(struct obs_audio_data *audio) {
    for (uint32_t frame = 0; frame < audio->frames; ++frame) {
      float mixed = 0.0f;
      size_t mixed_channels = 0;
      float fallback_sample = 0.0f;
      if (audio->data[0])
        fallback_sample = reinterpret_cast<const float *>(audio->data[0])[frame];

      float selected_sample = fallback_sample;
      bool have_selected = false;
      const size_t selected_channel =
          input_channel_ < 0 ? channels_ : static_cast<size_t>(input_channel_);

      for (size_t channel = 0; channel < channels_; ++channel) {
        const float *channel_data =
            reinterpret_cast<const float *>(audio->data[channel]);
        const float sample = channel_data ? channel_data[frame] : fallback_sample;
        dry_buffers_[channel].push_back(sample);
        if (channel == selected_channel) {
          selected_sample = sample;
          have_selected = true;
        }
        if (channel_data) {
          mixed += sample;
          ++mixed_channels;
        }
      }

      if (input_channel_ >= 0 && have_selected)
        input_mono_.push_back(selected_sample);
      else
        input_mono_.push_back(
            mixed_channels ? mixed / static_cast<float>(mixed_channels)
                           : fallback_sample);
    }
  }

  void process_available_hops() {
    const size_t hop_size = static_cast<size_t>(model_->hop_size());
    const size_t window_size = static_cast<size_t>(model_->n_fft());
    while (input_mono_.size() >= window_size) {
      std::vector<float> frame(window_size);
      auto it = input_mono_.begin();
      for (size_t i = 0; i < window_size; ++i, ++it)
        frame[i] = *it;

      std::vector<float> noisy_spec;
      std::vector<float> enhanced_spec;
      std::vector<float> enhanced_hop;

      stft_->analysis_frame(frame, noisy_spec);
      model_->enhance_spectrum(noisy_spec, enhanced_spec);
      apply_attenuation_limit(noisy_spec, enhanced_spec);
      stft_->synthesis(enhanced_spec, enhanced_hop);

      for (float sample : enhanced_hop)
        output_mono_.push_back(sample);
      for (size_t i = 0; i < hop_size; ++i)
        input_mono_.pop_front();
    }
  }

  void apply_attenuation_limit(const std::vector<float> &noisy_spec,
                               std::vector<float> &enhanced_spec) const {
    const double limit = std::clamp(attenuation_limit_db_, 0.0, 60.0);
    const float alpha = db_to_amp(-limit);

    for (size_t i = 0; i < enhanced_spec.size(); ++i)
      enhanced_spec[i] =
          alpha * noisy_spec[i] + (1.0f - alpha) * enhanced_spec[i];
  }

  struct obs_audio_data *pop_output_packet(const PacketInfo &info) {
    std::vector<float> enhanced(info.frames);
    for (uint32_t i = 0; i < info.frames; ++i) {
      enhanced[i] = output_mono_.front();
      output_mono_.pop_front();
    }

    output_audio_ = {};

    log_packet_stats(info, enhanced);
    accumulate_window_stats(info, enhanced);

    for (size_t channel = 0; channel < channels_; ++channel) {
      output_storage_[channel].resize(info.frames);
      for (uint32_t frame = 0; frame < info.frames; ++frame) {
        const float dry = dry_buffers_[channel].front();
        dry_buffers_[channel].pop_front();

        const float wet = enhanced[frame];
        output_storage_[channel][frame] = static_cast<float>(
            ((1.0 - wet_mix_) * dry + wet_mix_ * wet) * output_gain_);
      }
      output_audio_.data[channel] =
          reinterpret_cast<uint8_t *>(output_storage_[channel].data());
    }

    output_audio_.frames = info.frames;
    output_audio_.timestamp =
        info.timestamp - static_cast<uint64_t>(
                             (static_cast<double>(model_->hop_size()) /
                              static_cast<double>(model_->sample_rate())) *
                             static_cast<double>(NS_PER_SECOND));

    info_queue_.pop_front();
    return &output_audio_;
  }

  void log_packet_stats(const PacketInfo &info,
                        const std::vector<float> &enhanced) {
    if (diagnostic_packets_logged_ >= MAX_DIAGNOSTIC_PACKETS || channels_ == 0)
      return;

    double input_energy = 0.0;
    double output_energy = 0.0;
    const bool use_selected =
        input_channel_ >= 0 && static_cast<size_t>(input_channel_) < channels_;

    for (uint32_t frame = 0; frame < info.frames; ++frame) {
      float input = 0.0f;

      if (use_selected) {
        input = dry_buffers_[static_cast<size_t>(input_channel_)][frame];
      } else {
        for (size_t channel = 0; channel < channels_; ++channel)
          input += dry_buffers_[channel][frame];
        input /= static_cast<float>(channels_);
      }

      const float output = enhanced[frame];
      input_energy += static_cast<double>(input) * input;
      output_energy += static_cast<double>(output) * output;
    }

    const double frames = static_cast<double>(std::max<uint32_t>(info.frames, 1));
    const double input_rms = std::sqrt(input_energy / frames);
    const double output_rms = std::sqrt(output_energy / frames);
    const double change_db =
        20.0 * std::log10((output_rms + 1.0e-12) / (input_rms + 1.0e-12));

    blog(LOG_INFO,
         "[obs-dpdfnet] output packet %zu: frames=%u input_rms=%.6f "
         "output_rms=%.6f change=%.1f dB input=%d max_suppression=%.1f "
         "wet=%.0f%% queued_hops=%zu",
         diagnostic_packets_logged_ + 1, info.frames, input_rms, output_rms,
         change_db, input_channel_, attenuation_limit_db_, wet_mix_ * 100.0,
         output_mono_.size() / static_cast<size_t>(model_->hop_size()));

    ++diagnostic_packets_logged_;
  }

  void reset_diagnostic_window() {
    diagnostic_window_frames_ = 0;
    diagnostic_window_input_energy_ = 0.0;
    diagnostic_window_output_energy_ = 0.0;
    diagnostic_window_channel_energy_.assign(channels_, 0.0);
    diagnostic_windows_logged_ = 0;
  }

  void accumulate_window_stats(const PacketInfo &info,
                               const std::vector<float> &enhanced) {
    if (diagnostic_windows_logged_ >= MAX_DIAGNOSTIC_WINDOWS ||
        channels_ == 0 || sample_rate_ == 0)
      return;

    if (diagnostic_window_channel_energy_.size() != channels_)
      diagnostic_window_channel_energy_.assign(channels_, 0.0);

    const bool use_selected =
        input_channel_ >= 0 && static_cast<size_t>(input_channel_) < channels_;

    for (uint32_t frame = 0; frame < info.frames; ++frame) {
      float input = 0.0f;

      if (use_selected) {
        input = dry_buffers_[static_cast<size_t>(input_channel_)][frame];
      } else {
        for (size_t channel = 0; channel < channels_; ++channel)
          input += dry_buffers_[channel][frame];
        input /= static_cast<float>(channels_);
      }

      for (size_t channel = 0; channel < channels_; ++channel) {
        const float sample = dry_buffers_[channel][frame];
        diagnostic_window_channel_energy_[channel] +=
            static_cast<double>(sample) * sample;
      }

      const float output = enhanced[frame];
      diagnostic_window_input_energy_ += static_cast<double>(input) * input;
      diagnostic_window_output_energy_ += static_cast<double>(output) * output;
      ++diagnostic_window_frames_;
    }

    if (diagnostic_window_frames_ < sample_rate_)
      return;

    const double frames = static_cast<double>(diagnostic_window_frames_);
    const double input_rms = std::sqrt(diagnostic_window_input_energy_ / frames);
    const double output_rms =
        std::sqrt(diagnostic_window_output_energy_ / frames);
    const double change_db =
        20.0 * std::log10((output_rms + 1.0e-12) / (input_rms + 1.0e-12));
    const double ch1_rms =
        channels_ > 0 ? std::sqrt(diagnostic_window_channel_energy_[0] / frames)
                      : 0.0;
    const double ch2_rms =
        channels_ > 1 ? std::sqrt(diagnostic_window_channel_energy_[1] / frames)
                      : 0.0;

    blog(LOG_INFO,
         "[obs-dpdfnet] live window %zu: frames=%zu selected_rms=%.6f "
         "output_rms=%.6f change=%.1f dB ch1_rms=%.6f ch2_rms=%.6f "
         "input=%d max_suppression=%.1f wet=%.0f%%",
         diagnostic_windows_logged_ + 1, diagnostic_window_frames_, input_rms,
         output_rms, change_db, ch1_rms, ch2_rms, input_channel_,
         attenuation_limit_db_, wet_mix_ * 100.0);

    ++diagnostic_windows_logged_;
    diagnostic_window_frames_ = 0;
    diagnostic_window_input_energy_ = 0.0;
    diagnostic_window_output_energy_ = 0.0;
    std::fill(diagnostic_window_channel_energy_.begin(),
              diagnostic_window_channel_energy_.end(), 0.0);
  }

  obs_source_t *source_ = nullptr;
  std::mutex mutex_;

  std::string model_path_;
  std::string loaded_model_path_;
  std::unique_ptr<DpdfnetModel> model_;
  std::unique_ptr<StreamingStft> stft_;

  uint32_t sample_rate_ = 0;
  size_t channels_ = 0;
  uint64_t last_timestamp_ = 0;

  double attenuation_limit_db_ = 6.0;
  int input_channel_ = 0;
  double wet_mix_ = 1.0;
  float output_gain_ = 1.0f;
  bool bypass_ = false;
  bool rate_warning_logged_ = false;
  size_t diagnostic_packets_logged_ = 0;
  size_t diagnostic_windows_logged_ = 0;
  size_t diagnostic_window_frames_ = 0;
  double diagnostic_window_input_energy_ = 0.0;
  double diagnostic_window_output_energy_ = 0.0;
  std::vector<double> diagnostic_window_channel_energy_;

  std::deque<PacketInfo> info_queue_;
  std::deque<float> input_mono_;
  std::deque<float> output_mono_;
  std::vector<std::deque<float>> dry_buffers_;
  std::vector<std::vector<float>> output_storage_;
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
