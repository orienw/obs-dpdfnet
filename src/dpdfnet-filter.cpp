// SPDX-License-Identifier: GPL-2.0-or-later

#include "dpdfnet-processor.hpp"
#include "dpdfnet-realtime-guard.hpp"
#include "dpdfnet-settings.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace {
constexpr const char *SETTING_MODEL_SELECTION = "model_selection";
constexpr const char *SETTING_MODEL_PATH = "model_path";
constexpr const char *SETTING_INPUT_CHANNEL = "input_channel";
constexpr const char *SETTING_ATTENUATION_LIMIT_DB = "attenuation_limit_db";
constexpr const char *SETTING_WET_MIX = "wet_mix";
constexpr const char *SETTING_OUTPUT_GAIN_DB = "output_gain_db";
constexpr const char *SETTING_BYPASS = "bypass";
constexpr const char *SETTING_SHOW_DETAILS = "show_details";

constexpr const char *QUALITY_MODEL_FILE = "models/dpdfnet8_48khz_hr.onnx";
constexpr const char *LOW_CPU_MODEL_FILE = "models/dpdfnet2_48khz_hr.onnx";

// Audio an automatic retry must process on probe thresholds before the
// guard relaxes again and the retry schedule starts over.
constexpr uint64_t OVERLOAD_PROBE_AUDIO_NS = 10'000'000'000ULL;

// Model audio in the recent-load window after which it halves. The half-life
// is half of this, about 2 s, long enough for a steady reading.
constexpr uint64_t LOAD_WINDOW_NS = 4'000'000'000ULL;

std::string module_file(const char *relative_path) {
  char *path = obs_module_file(relative_path);
  if (!path)
    return {};
  std::string result(path);
  bfree(path);
  return result;
}

std::string quality_model_path() { return module_file(QUALITY_MODEL_FILE); }
std::string low_cpu_model_path() { return module_file(LOW_CPU_MODEL_FILE); }

std::string migrate_model_selection(obs_data_t *settings) {
  const bool has_selection =
      obs_data_has_user_value(settings, SETTING_MODEL_SELECTION);
  const bool has_legacy_path =
      obs_data_has_user_value(settings, SETTING_MODEL_PATH);
  const char *raw_selection =
      obs_data_get_string(settings, SETTING_MODEL_SELECTION);
  const char *raw_path = obs_data_get_string(settings, SETTING_MODEL_PATH);
  std::string selection = raw_selection ? raw_selection : "";
  const std::string legacy_path = raw_path ? raw_path : "";

  if (!has_selection || !dpdfnet_known_model_selection(selection)) {
    selection = dpdfnet_classify_model_selection(
        has_selection, selection, has_legacy_path, legacy_path,
        quality_model_path(), low_cpu_model_path());
    obs_data_set_string(settings, SETTING_MODEL_SELECTION, selection.c_str());
  }
  return selection;
}

std::string selected_model_path(obs_data_t *settings,
                                const std::string &selection) {
  if (selection == DPDFNET_MODEL_QUALITY)
    return quality_model_path();
  if (selection == DPDFNET_MODEL_LOW_CPU)
    return low_cpu_model_path();
  const char *path = obs_data_get_string(settings, SETTING_MODEL_PATH);
  return path ? path : "";
}

class TimingHistogram {
public:
  void record(uint64_t nanoseconds) {
    uint64_t microseconds = (nanoseconds + 999) / 1000;
    size_t bin = 0;
    while (microseconds > 1 && bin + 1 < bins_.size()) {
      microseconds = (microseconds + 1) / 2;
      ++bin;
    }
    ++bins_[bin];
    ++count_;
    max_ns_ = std::max(max_ns_, nanoseconds);
  }

  uint64_t percentile_upper_ns(double percentile) const {
    if (!count_)
      return 0;
    const uint64_t target = static_cast<uint64_t>(
        std::ceil(static_cast<double>(count_) * percentile));
    uint64_t seen = 0;
    for (size_t i = 0; i < bins_.size(); ++i) {
      seen += bins_[i];
      if (seen >= target)
        return (uint64_t{1} << i) * 1000;
    }
    return max_ns_;
  }

  uint64_t count() const { return count_; }
  uint64_t max_ns() const { return max_ns_; }

private:
  std::array<uint64_t, 32> bins_ = {};
  uint64_t count_ = 0;
  uint64_t max_ns_ = 0;
};

struct TimingSnapshot {
  uint64_t callbacks = 0;
  uint64_t missed_deadlines = 0;
  uint64_t lock_p99_ns = 0;
  uint64_t process_p99_ns = 0;
  uint64_t total_p99_ns = 0;
  uint64_t total_max_ns = 0;
  uint64_t window_processing_ns = 0;
  uint64_t window_budget_ns = 0;
  uint64_t epoch_processing_ns = 0;
  uint64_t epoch_budget_ns = 0;
  std::array<uint64_t, 4> hop_callbacks = {};
  std::array<uint64_t, 4> hop_process_p99_ns = {};
};

class CallbackTimings {
public:
  void record(uint64_t lock_wait_ns, uint64_t process_ns, uint64_t total_ns,
              uint64_t deadline_ns, size_t processed_hops,
              uint64_t realtime_processing_ns, uint64_t realtime_budget_ns) {
    lock_wait_.record(lock_wait_ns);
    process_.record(process_ns);
    total_.record(total_ns);
    const size_t hop_bucket = std::min<size_t>(processed_hops, 3);
    process_by_hops_[hop_bucket].record(process_ns);
    if (deadline_ns && total_ns > deadline_ns)
      ++missed_deadlines_;
    if (realtime_budget_ns) {
      window_processing_ns_ += realtime_processing_ns;
      window_budget_ns_ += realtime_budget_ns;
      epoch_processing_ns_ += realtime_processing_ns;
      epoch_budget_ns_ += realtime_budget_ns;
      // Halve instead of zeroing, so the meter has no sawtooth and the
      // first window stays a plain average during warm-up. Loop in case a
      // future callback ever covers more than one window.
      while (window_budget_ns_ >= LOAD_WINDOW_NS) {
        window_processing_ns_ = (window_processing_ns_ + 1) / 2;
        window_budget_ns_ = (window_budget_ns_ + 1) / 2;
      }
    }
  }

  TimingSnapshot snapshot() const {
    TimingSnapshot result;
    result.callbacks = total_.count();
    result.missed_deadlines = missed_deadlines_;
    result.lock_p99_ns = lock_wait_.percentile_upper_ns(0.99);
    result.process_p99_ns = process_.percentile_upper_ns(0.99);
    result.total_p99_ns = total_.percentile_upper_ns(0.99);
    result.total_max_ns = total_.max_ns();
    result.window_processing_ns = window_processing_ns_;
    result.window_budget_ns = window_budget_ns_;
    result.epoch_processing_ns = epoch_processing_ns_;
    result.epoch_budget_ns = epoch_budget_ns_;
    for (size_t i = 0; i < process_by_hops_.size(); ++i) {
      result.hop_callbacks[i] = process_by_hops_[i].count();
      result.hop_process_p99_ns[i] =
          process_by_hops_[i].percentile_upper_ns(0.99);
    }
    return result;
  }

  void reset() { *this = CallbackTimings{}; }

private:
  TimingHistogram lock_wait_;
  TimingHistogram process_;
  TimingHistogram total_;
  std::array<TimingHistogram, 4> process_by_hops_;
  uint64_t missed_deadlines_ = 0;
  uint64_t window_processing_ns_ = 0;
  uint64_t window_budget_ns_ = 0;
  uint64_t epoch_processing_ns_ = 0;
  uint64_t epoch_budget_ns_ = 0;
};

enum class StatusSeverity { Normal, Warning, Error };
struct FilterStatus {
  StatusSeverity severity = StatusSeverity::Normal;
  std::string summary;
  std::string text;
};

class DpdfnetFilter {
public:
  explicit DpdfnetFilter(obs_source_t *source)
      : source_(source),
        resampler_worker_([this] { resampler_worker_loop(); }) {
    signal_handler_connect(obs_source_get_signal_handler(source_), "enable",
                           enabled_changed, this);
  }

  DpdfnetFilter(const DpdfnetFilter &) = delete;
  DpdfnetFilter &operator=(const DpdfnetFilter &) = delete;

  ~DpdfnetFilter() {
    signal_handler_disconnect(obs_source_get_signal_handler(source_), "enable",
                              enabled_changed, this);
    {
      std::lock_guard<std::mutex> lock(resampler_request_mutex_);
      stop_resampler_worker_ = true;
    }
    resampler_request_cv_.notify_one();
    resampler_worker_.join();
    const TimingSnapshot timing = timing_snapshot();
    if (timing.callbacks) {
      blog(LOG_INFO,
           "[obs-dpdfnet] active-processing timing epoch: count=%llu "
           "missed=%llu "
           "lock_p99<=%.3f ms process_p99<=%.3f ms total_p99<=%.3f ms "
           "max=%.3f ms",
           static_cast<unsigned long long>(timing.callbacks),
           static_cast<unsigned long long>(timing.missed_deadlines),
           timing.lock_p99_ns / 1e6, timing.process_p99_ns / 1e6,
           timing.total_p99_ns / 1e6, timing.total_max_ns / 1e6);
      if (timing.epoch_budget_ns) {
        blog(LOG_INFO,
             "[obs-dpdfnet] active-processing load epoch: avg=%.1f%% over "
             "%.1f s of audio",
             static_cast<double>(timing.epoch_processing_ns) * 100.0 /
                 static_cast<double>(timing.epoch_budget_ns),
             timing.epoch_budget_ns / 1e9);
      }
      constexpr const char *hop_labels[] = {"0 hops", "1 hop", "2 hops",
                                            "3+ hops"};
      for (size_t hops = 0; hops < timing.hop_callbacks.size(); ++hops) {
        if (timing.hop_callbacks[hops]) {
          blog(LOG_INFO,
               "[obs-dpdfnet] callback processing bucket %s: count=%llu "
               "p99<=%.3f ms",
               hop_labels[hops],
               static_cast<unsigned long long>(timing.hop_callbacks[hops]),
               timing.hop_process_p99_ns[hops] / 1e6);
        }
      }
    }
  }

  void update(obs_data_t *settings) {
    std::lock_guard<std::mutex> update_lock(update_mutex_);

    const std::string selection = migrate_model_selection(settings);
    const std::string requested_path = selected_model_path(settings, selection);
    const int input_channel =
        static_cast<int>(obs_data_get_int(settings, SETTING_INPUT_CHANNEL));
    DpdfnetControls controls;
    controls.input_channel = input_channel;
    controls.attenuation_limit_db =
        obs_data_get_double(settings, SETTING_ATTENUATION_LIMIT_DB);
    controls.wet_mix = std::clamp(
        obs_data_get_double(settings, SETTING_WET_MIX) / 100.0, 0.0, 1.0);
    controls.output_gain = dpdfnet_db_to_amp(
        obs_data_get_double(settings, SETTING_OUTPUT_GAIN_DB));
    controls.bypass = obs_data_get_bool(settings, SETTING_BYPASS);
    const bool show_details = obs_data_get_bool(settings, SETTING_SHOW_DETAILS);

    const uint32_t obs_rate = audio_output_get_sample_rate(obs_get_audio());
    const size_t obs_channels =
        std::clamp<size_t>(audio_output_get_channels(obs_get_audio()), 1,
                           DPDFNET_MAX_AUDIO_PLANES);

    DpdfnetProcessorState before;
    int old_input_channel = 0;
    bool need_load = false;
    bool skip_failed = false;
    bool clear_failed_request = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      before = processor_.state();
      old_input_channel = controls_.input_channel;
      need_load = requested_path != active_model_path_ ||
                  (selection == DPDFNET_MODEL_CUSTOM && requested_path.empty());
      skip_failed = need_load && have_failed_model_path_ &&
                    requested_path == failed_model_path_;
      clear_failed_request =
          have_failed_model_path_ && requested_path != failed_model_path_;
    }

    const bool channel_boundary = input_channel != old_input_channel;
    const bool format_boundary =
        before.sample_rate != obs_rate || before.channels != obs_channels;

    DpdfnetModelBundle new_model;
    bool load_attempted = false;
    std::string load_error;
    if (need_load && !skip_failed && !requested_path.empty()) {
      load_attempted = true;
      try {
        new_model = prepare_dpdfnet_model(requested_path);
      } catch (const std::exception &ex) {
        load_error = ex.what();
      }
    } else if (need_load && !skip_failed && requested_path.empty()) {
      load_attempted = true;
      load_error = "no custom ONNX model was selected";
    }

    const DpdfnetModel *target_model =
        new_model.model ? new_model.model.get() : nullptr;
    const int target_rate =
        target_model ? target_model->sample_rate() : before.model_rate;
    const int target_hop =
        target_model ? target_model->hop_size() : before.hop_size;
    const int target_nfft = target_model ? target_model->n_fft() : before.n_fft;
    const std::string target_name =
        target_model ? target_model->name() : std::string();
    const bool replacing_model = static_cast<bool>(new_model.model);
    const bool want_resamplers =
        target_rate > 0 && obs_rate != static_cast<uint32_t>(target_rate);
    const bool fresh_resamplers =
        want_resamplers &&
        (replacing_model || channel_boundary || format_boundary ||
         !processor_resampler_matches(obs_rate, target_rate));

    DpdfnetResamplers new_resamplers;
    bool resampler_attempted = false;
    std::string resampler_error;
    if (fresh_resamplers) {
      resampler_attempted = true;
      try {
        new_resamplers =
            prepare_dpdfnet_resamplers(obs_rate, target_rate, target_hop);
      } catch (const std::exception &ex) {
        resampler_error = ex.what();
        if (replacing_model) {
          load_error = resampler_error;
          new_model = {};
        }
      }
    }
    if (new_model.model) {
      try {
        prefill_dpdfnet_model_bundle(new_model, obs_channels,
                                     new_resamplers.prefill_frames());
      } catch (const std::exception &ex) {
        load_error = ex.what();
        new_model = {};
        new_resamplers = {};
      }
    }
    const bool new_resamplers_ready = static_cast<bool>(new_resamplers);
    const bool model_activation_failed =
        replacing_model && resampler_attempted && !new_resamplers_ready;

    const bool old_needs_resamplers =
        before.has_model && before.model_rate > 0 &&
        obs_rate != static_cast<uint32_t>(before.model_rate);
    const bool fallback_needed =
        model_activation_failed && old_needs_resamplers &&
        (format_boundary ||
         !processor_resampler_matches(obs_rate, before.model_rate));
    DpdfnetResamplers fallback_resamplers;
    bool fallback_attempted = false;
    std::string fallback_error;
    if (fallback_needed) {
      fallback_attempted = true;
      try {
        fallback_resamplers = prepare_dpdfnet_resamplers(
            obs_rate, before.model_rate, before.hop_size);
      } catch (const std::exception &ex) {
        fallback_error = ex.what();
      }
    }
    const bool fallback_ready = static_cast<bool>(fallback_resamplers);

    std::string prepared_active_path = requested_path;
    std::string prepared_failed_path = requested_path;
    std::string prepared_load_error = load_error;
    std::string prepared_resampler_error = resampler_error;
    std::string prepared_fallback_error = fallback_error;
    std::string discarded_failed_path;
    std::string discarded_load_error;
    std::string discarded_resampler_error;

    DpdfnetModelBundle old_model;
    DpdfnetResamplers old_resamplers;
    bool model_loaded = false;
    bool model_failed = false;
    const char *active_path_after_failure = nullptr;
    DpdfnetProcessorState after;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      processor_.set_format(obs_rate, obs_channels);

      if (replacing_model && new_model.model) {
        old_model = processor_.replace_model(std::move(new_model));
        model_loaded = true;
        overload_retries_.reset();
        realtime_guard_.set_probe(false);
        probe_audio_ns_ = 0;
        active_model_path_.swap(prepared_active_path);
        have_failed_model_path_ = false;
        failed_model_path_.swap(discarded_failed_path);
        last_load_error_.swap(discarded_load_error);
      } else if (load_attempted && !load_error.empty()) {
        have_failed_model_path_ = true;
        failed_model_path_.swap(prepared_failed_path);
        last_load_error_.swap(prepared_load_error);
        model_failed = true;
        active_path_after_failure = active_model_path_.c_str();
      } else if (clear_failed_request) {
        have_failed_model_path_ = false;
        failed_model_path_.swap(discarded_failed_path);
        last_load_error_.swap(discarded_load_error);
      }

      if (model_loaded) {
        if (want_resamplers) {
          old_resamplers =
              processor_.replace_resamplers(std::move(new_resamplers), false);
        } else {
          old_resamplers = processor_.replace_resamplers({}, false);
        }
        last_resampler_error_.swap(discarded_resampler_error);
      } else if (model_activation_failed) {
        if (!old_needs_resamplers) {
          if (format_boundary || before.resampling ||
              before.resampler_refresh_required)
            old_resamplers = processor_.replace_resamplers({});
          last_resampler_error_.swap(discarded_resampler_error);
        } else if (fallback_ready) {
          old_resamplers =
              processor_.replace_resamplers(std::move(fallback_resamplers));
          last_resampler_error_.swap(discarded_resampler_error);
        } else if (fallback_attempted) {
          old_resamplers = processor_.replace_resamplers({});
          last_resampler_error_.swap(prepared_fallback_error);
        }
      } else if (fresh_resamplers) {
        if (new_resamplers_ready) {
          old_resamplers =
              processor_.replace_resamplers(std::move(new_resamplers));
          last_resampler_error_.swap(discarded_resampler_error);
        } else {
          old_resamplers = processor_.replace_resamplers({});
          last_resampler_error_.swap(prepared_resampler_error);
        }
      } else if (!want_resamplers && (format_boundary || before.resampling ||
                                      before.resampler_refresh_required)) {
        old_resamplers = processor_.replace_resamplers({});
        last_resampler_error_.swap(discarded_resampler_error);
      }

      controls_ = controls;
      processor_.set_controls(controls_);
      custom_model_selected_ = selection == DPDFNET_MODEL_CUSTOM;
      show_details_ = show_details;
      if (model_loaded || format_boundary ||
          (fresh_resamplers && !model_activation_failed) || fallback_attempted)
        reset_timing_epoch();
      after = processor_.state();
    }

    if (model_loaded) {
      blog(LOG_INFO,
           "[obs-dpdfnet] loaded %s (model %s, %d Hz, frame %d, hop %d)",
           requested_path.c_str(), target_name.c_str(), target_rate,
           target_nfft, target_hop);
    } else if (model_failed) {
      blog(LOG_ERROR, "[obs-dpdfnet] failed to load model '%s': %s",
           requested_path.c_str(), load_error.c_str());
      blog(
          LOG_WARNING,
          "[obs-dpdfnet] keeping previously loaded model after failed load: %s",
          !active_path_after_failure || !*active_path_after_failure
              ? "(none)"
              : active_path_after_failure);
    }

    if (fresh_resamplers && new_resamplers_ready && !model_activation_failed &&
        after.resampling) {
      blog(LOG_INFO, "[obs-dpdfnet] resampling %u Hz <-> %d Hz", obs_rate,
           target_rate);
    } else if (resampler_attempted && !new_resamplers_ready) {
      blog(LOG_ERROR,
           "[obs-dpdfnet] failed to activate %u Hz <-> %d Hz resampling: %s",
           obs_rate, target_rate, resampler_error.c_str());
    }
    if (fallback_attempted) {
      if (fallback_ready) {
        blog(LOG_INFO,
             "[obs-dpdfnet] retained active model with fresh %u Hz <-> %d Hz "
             "resampling",
             obs_rate, before.model_rate);
      } else {
        blog(LOG_ERROR,
             "[obs-dpdfnet] failed to restore %u Hz <-> %d Hz resampling for "
             "the active model: %s",
             obs_rate, before.model_rate, fallback_error.c_str());
      }
    }

    blog(LOG_INFO,
         "[obs-dpdfnet] settings: model=%s input=%d max_suppression=%.1f dB "
         "wet=%.0f%% gain=%.2f bypass=%s",
         selection.c_str(), input_channel, controls.attenuation_limit_db,
         controls.wet_mix * 100.0, controls.output_gain,
         controls.bypass ? "true" : "false");
  }

  struct obs_audio_data *filter_audio(struct obs_audio_data *audio) {
    if (!audio || !audio->frames)
      return audio;

    const uint64_t callback_start = os_gettime_ns();
    const uint32_t obs_rate = audio_output_get_sample_rate(obs_get_audio());
    const size_t obs_channels =
        std::clamp<size_t>(audio_output_get_channels(obs_get_audio()), 1,
                           DPDFNET_MAX_AUDIO_PLANES);
    DpdfnetAudioPacket packet;
    packet.frames = audio->frames;
    packet.timestamp = audio->timestamp;
    for (size_t channel = 0; channel < obs_channels; ++channel)
      packet.data[channel] =
          reinterpret_cast<const float *>(audio->data[channel]);

    const uint64_t lock_start = os_gettime_ns();
    std::unique_lock<std::mutex> lock(mutex_);
    const uint64_t lock_acquired = os_gettime_ns();
    const DpdfnetProcessorState before = processor_.state();
    const bool format_changed =
        before.sample_rate != obs_rate || before.channels != obs_channels;
    if (format_changed)
      reset_timing_epoch();
    processor_.set_format(obs_rate, obs_channels);
    const DpdfnetProcessorState active = processor_.state();
    const bool format_resampler_refresh =
        format_changed && active.resampler_refresh_required;
    timestamp_floor_.observe_input(audio->timestamp);
    const bool timing_eligible =
        active.has_model && !active.processing_disabled &&
        (active.sample_rate == static_cast<uint32_t>(active.model_rate) ||
         active.resampling);
    const uint64_t processor_started = os_gettime_ns();
    DpdfnetProcessResult result = processor_.process(packet);
    const uint64_t processor_finished = os_gettime_ns();
    const uint64_t realtime_processing_ns =
        processor_finished - processor_started;
    uint64_t realtime_budget_ns = 0;
    const bool active_processing_result =
        result.disposition != DpdfnetDisposition::Passthrough;
    const bool guard_observed = timing_eligible && active_processing_result;
    uint64_t overload_retry_delay_ns = 0;

    if (guard_observed) {
      const DpdfnetRealtimeObservation observation =
          realtime_guard_.observe(realtime_processing_ns, result.processed_hops,
                                  active.hop_size, active.model_rate);
      realtime_budget_ns = observation.budget_ns;
      if (observation.tripped) {
        std::snprintf(result.message.data(), result.message.size(),
                      "processing took %.1f ms for %.1f ms of audio, debt "
                      "%.1f ms",
                      realtime_processing_ns / 1e6, observation.budget_ns / 1e6,
                      observation.debt_ns / 1e6);
        if (processor_.disable_for_realtime_overload(result.message.data())) {
          result.event = DpdfnetEvent::RealtimeOverloadCircuitOpened;
          overload_retry_delay_ns = overload_retries_.next_delay_ns();
        }
      } else if (realtime_guard_.probe() &&
                 result.event == DpdfnetEvent::None) {
        probe_audio_ns_ += observation.budget_ns;
        if (probe_audio_ns_ >= OVERLOAD_PROBE_AUDIO_NS) {
          realtime_guard_.set_probe(false);
          overload_retries_.reset();
          result.event = DpdfnetEvent::RealtimeOverloadRecovered;
        }
      }
    }

    // Inference the guard did not observe, during a pause's fade-out or in a
    // callback that failed, still counts toward timing and load.
    const bool unobserved_inference =
        !guard_observed && result.inference_hops > 0;
    if (unobserved_inference) {
      // A hop whose inference failed was attempted but not processed.
      realtime_budget_ns = DpdfnetRealtimeBudgetGuard::audio_budget_ns(
          std::max(result.processed_hops, result.inference_hops),
          active.hop_size, active.model_rate);
    }

    struct obs_audio_data *output = audio;
    if (result.disposition == DpdfnetDisposition::Pending) {
      output = nullptr;
    } else if (result.disposition == DpdfnetDisposition::Processed) {
      output_audio_ = {};
      output_audio_.frames = result.frames;
      output_audio_.timestamp = result.timestamp;
      for (size_t channel = 0; channel < obs_channels; ++channel) {
        output_audio_.data[channel] =
            reinterpret_cast<uint8_t *>(result.data[channel]);
      }
      output = &output_audio_;
    }

    if (output) {
      const uint64_t adjusted_timestamp =
          timestamp_floor_.apply(output->timestamp, output->frames, obs_rate);
      if (adjusted_timestamp != output->timestamp) {
        if (output == audio) {
          output_audio_ = *audio;
          output = &output_audio_;
        }
        output->timestamp = adjusted_timestamp;
      }
    }

    const uint64_t processing_finished = os_gettime_ns();
    const uint64_t deadline_ns =
        static_cast<uint64_t>(static_cast<double>(audio->frames) /
                              static_cast<double>(obs_rate) * 1e9);
    if (guard_observed || unobserved_inference) {
      timings_.record(
          lock_acquired - lock_start, processing_finished - lock_acquired,
          processing_finished - callback_start, deadline_ns,
          result.processed_hops, realtime_processing_ns, realtime_budget_ns);
    }
    lock.unlock();

    request_worker(format_resampler_refresh || result.resampler_refresh_needed,
                   result, overload_retry_delay_ns);
    return output;
  }

  void reset_state() {
    {
      std::lock_guard<std::mutex> update_lock(update_mutex_);
      DpdfnetProcessorState state;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        state = processor_.state();
      }

      FreshResamplers fresh = prepare_fresh_resamplers(state);
      if (!fresh.error.empty()) {
        blog(LOG_ERROR, "[obs-dpdfnet] reset resampler preparation failed: %s",
             fresh.error.c_str());
      }

      DpdfnetResamplers old;
      std::string discarded_failed_path;
      std::string discarded_load_error;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        old = install_fresh_resamplers(fresh, state.resampler_refresh_required);
        processor_.reset_state();
        reset_timing_epoch();
        realtime_guard_.set_probe(false);
        probe_audio_ns_ = 0;
        overload_retries_.reset();
        have_failed_model_path_ = false;
        failed_model_path_.swap(discarded_failed_path);
        last_load_error_.swap(discarded_load_error);
      }
    }
    {
      std::lock_guard<std::mutex> lock(resampler_request_mutex_);
      overload_retry_deadline_ns_ = 0;
    }

    obs_data_t *settings = obs_source_get_settings(source_);
    update(settings);
    obs_data_release(settings);
  }

  // Resumes the model where the overload paused it. The audio lanes kept
  // running, so the retry needs no fresh resamplers or stream reset.
  void retry_after_overload() {
    size_t attempt = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!processor_.resume_after_overload())
        return;
      attempt = overload_retries_.attempts();
      reset_timing_epoch();
      realtime_guard_.set_probe(true);
      probe_audio_ns_ = 0;
    }
    blog(LOG_INFO,
         "[obs-dpdfnet] retrying processing after realtime overload "
         "(attempt %zu)",
         attempt);
  }

  void reset_stream_boundary() {
    std::lock_guard<std::mutex> update_lock(update_mutex_);
    DpdfnetProcessorState state;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      state = processor_.state();
    }

    FreshResamplers fresh = prepare_fresh_resamplers(state);
    if (!fresh.error.empty()) {
      blog(LOG_ERROR,
           "[obs-dpdfnet] stream-boundary resampler preparation failed: %s",
           fresh.error.c_str());
    }

    DpdfnetResamplers old;
    std::lock_guard<std::mutex> lock(mutex_);
    old = install_fresh_resamplers(fresh, state.resampler_refresh_required);
    processor_.reset_stream();
    reset_timing_epoch();
    timestamp_floor_.reset();
  }

  FilterStatus status() const {
    std::lock_guard<std::mutex> update_lock(update_mutex_);
    DpdfnetProcessorState state;
    TimingSnapshot timing;
    const DpdfnetModel *model = nullptr;
    bool probe = false;
    size_t retry_attempts = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      state = processor_.state();
      timing = timings_.snapshot();
      model = processor_.model();
      probe = realtime_guard_.probe();
      retry_attempts = overload_retries_.attempts();
    }
    // Only update() replaces the model, and it holds update_mutex_.
    const DpdfnetProcessorSnapshot snapshot =
        make_dpdfnet_snapshot(state, model);
    const std::string load_error = last_load_error_;
    const std::string resampler_error = last_resampler_error_;
    uint64_t retry_in_ns = 0;
    {
      std::lock_guard<std::mutex> lock(resampler_request_mutex_);
      if (overload_retry_deadline_ns_) {
        const uint64_t now = os_gettime_ns();
        retry_in_ns = overload_retry_deadline_ns_ > now
                          ? overload_retry_deadline_ns_ - now
                          : 1;
      }
    }

    const auto khz = [](double hz) { return hz / 1000.0; };
    const auto ms = [](uint64_t ns) {
      char buffer[32];
      std::snprintf(buffer, sizeof(buffer), "%.1f", ns / 1e6);
      return std::string(buffer);
    };
    const bool rate_mismatch =
        snapshot.has_model && snapshot.sample_rate && snapshot.model_rate > 0 &&
        snapshot.sample_rate != static_cast<uint32_t>(snapshot.model_rate) &&
        !snapshot.resampling;
    const bool overload =
        snapshot.processing_disabled &&
        snapshot.disable_reason == DpdfnetDisableReason::RealtimeOverload;
    std::string model_label;
    if (paths_equivalent_for_status(snapshot.model_path, quality_model_path()))
      model_label = "DPDFNet8";
    else if (paths_equivalent_for_status(snapshot.model_path,
                                         low_cpu_model_path()))
      model_label = "DPDFNet2";
    else
      model_label = snapshot.model_name + " (custom)";

    // Two lines: a state word, then one plain sentence or a spec line.
    FilterStatus result;
    std::ostringstream summary;
    // A retry that is starting clears its deadline first; count it as 1 s.
    const uint64_t retry_in_s =
        std::max<uint64_t>(1, (retry_in_ns + 999'999'999) / 1'000'000'000);
    if (overload) {
      result.severity = StatusSeverity::Warning;
      summary << "Paused\nAudio is passing through without noise suppression. "
                 "Processing will retry in "
              << retry_in_s << (retry_in_s == 1 ? " second." : " seconds.");
    } else if (snapshot.processing_disabled) {
      result.severity = StatusSeverity::Error;
      summary << "Off\nProcessing failed repeatedly. Press Reset processing to "
                 "try again.";
    } else if (rate_mismatch) {
      result.severity = StatusSeverity::Error;
      summary << "Off\nResampling from " << khz(snapshot.sample_rate)
              << " kHz is unavailable, so audio is passing through "
                 "unprocessed.";
    } else if (!snapshot.has_model) {
      result.severity = StatusSeverity::Error;
      summary << "Off\nNo model is loaded, so audio is passing through "
                 "unprocessed.";
    } else if (!load_error.empty()) {
      result.severity = StatusSeverity::Error;
      summary << "Active\nThe selected model failed to load, so " << model_label
              << " is still in use.";
    } else if (snapshot.capacity_failures) {
      result.severity = StatusSeverity::Error;
      summary << "Active\nA buffer error passed some audio through "
                 "unprocessed.";
    } else if (snapshot.bypass) {
      result.severity = StatusSeverity::Warning;
      summary << "Bypass\nThe original audio is passing through.";
    } else if (snapshot.consecutive_failures) {
      result.severity = StatusSeverity::Warning;
      summary << "Active\nRetrying after a processing error.";
    } else if (snapshot.oversized_packets) {
      result.severity = StatusSeverity::Warning;
      summary << "Active\nSome oversized audio packets passed through "
                 "unprocessed.";
    } else if (snapshot.capacity_recovery_pending) {
      result.severity = StatusSeverity::Warning;
      summary << "Active\nRecovering from a buffer error.";
    } else {
      summary << "Active\n" << model_label;
      if (snapshot.resampling)
        summary << ", resampling from " << khz(snapshot.sample_rate) << " kHz";
      if (probe)
        summary << ", recovering after an overload";
      if (timing.window_budget_ns) {
        const uint64_t load_percent = static_cast<uint64_t>(
            std::llround(static_cast<double>(timing.window_processing_ns) *
                         100.0 / static_cast<double>(timing.window_budget_ns)));
        summary << ", " << load_percent << "% processing load";
      }
    }

    // Details are a readout: key and value per line, no periods.
    std::ostringstream text;
    if (snapshot.has_model) {
      const double frame_ms = snapshot.model_rate
                                  ? snapshot.n_fft * 1000.0 /
                                        static_cast<double>(snapshot.model_rate)
                                  : 0.0;
      const double hop_ms = snapshot.model_rate
                                ? snapshot.hop_size * 1000.0 /
                                      static_cast<double>(snapshot.model_rate)
                                : 0.0;
      text << "Model: " << model_label;
      if (snapshot.resampling)
        text << ", OBS " << khz(snapshot.sample_rate) << " kHz resampled to "
             << khz(snapshot.model_rate) << " kHz";
      else
        text << ", " << khz(snapshot.model_rate) << " kHz native";
      text << ", " << frame_ms << " ms frame, " << hop_ms << " ms hop";
    } else {
      text << "Model: none loaded";
    }
    if (!load_error.empty())
      text << "\nModel load failed: " << load_error;
    if (rate_mismatch) {
      if (!resampler_error.empty())
        text << "\nResampler error: " << resampler_error;
      else if (snapshot.resampler_refresh_required)
        text << "\nResampler: being prepared";
    }
    if (!snapshot.last_error.empty())
      text << (overload ? "\nLast overload: " : "\nLast error: ")
           << snapshot.last_error;
    if (overload) {
      text << "\nNext retry: in " << retry_in_s << " s, attempt "
           << retry_attempts;
    }
    if (snapshot.bypass)
      text << "\nBypass: on, delay-matched original audio with the model kept "
              "warm";
    if (snapshot.oversized_packets) {
      text << "\nOversized packets: " << snapshot.oversized_packets
           << " over the " << DPDFNET_MAX_REALTIME_PACKET_FRAMES
           << "-frame realtime limit since the last reset";
    }
    if (snapshot.capacity_failures) {
      text << "\nBuffer capacity failures: " << snapshot.capacity_failures
           << ", affected audio passed through";
    }
    if (snapshot.capacity_recovery_pending)
      text << "\nRecovery: pending";
    if (timing.callbacks) {
      text << "\nWorst callback: " << ms(timing.total_max_ns) << " ms, "
           << timing.missed_deadlines << " of " << timing.callbacks
           << " missed the deadline";
    }

    result.summary = summary.str();
    result.text = text.str();
    return result;
  }

  TimingSnapshot timing_snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return timings_.snapshot();
  }

  bool custom_model_selected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return custom_model_selected_;
  }

  bool show_details() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return show_details_;
  }

private:
  struct CallbackDiagnostic {
    bool pending = false;
    DpdfnetEvent event = DpdfnetEvent::None;
    uint64_t retry_delay_ns = 0;
    std::array<char, 256> message = {};
  };

  void reset_timing_epoch() {
    timings_.reset();
    realtime_guard_.reset();
  }

  struct FreshResamplers {
    bool needed = false;
    DpdfnetResamplers resamplers;
    std::string error;
  };

  // Prepares resamplers for the processor's current format outside the audio
  // lock. They are needed whenever the model rate differs from OBS's.
  static FreshResamplers
  prepare_fresh_resamplers(const DpdfnetProcessorState &state) {
    FreshResamplers fresh;
    fresh.needed = state.has_model && state.sample_rate &&
                   state.sample_rate != static_cast<uint32_t>(state.model_rate);
    if (fresh.needed) {
      try {
        fresh.resamplers = prepare_dpdfnet_resamplers(
            state.sample_rate, state.model_rate, state.hop_size);
      } catch (const std::exception &ex) {
        fresh.error = ex.what();
      }
    }
    return fresh;
  }

  // Requires mutex_. Installs the prepared resamplers, or clears stale ones,
  // and records the preparation error. The previous error text lands in
  // `fresh` and the previous resamplers are returned, so the caller frees
  // both after releasing the lock.
  DpdfnetResamplers install_fresh_resamplers(FreshResamplers &fresh,
                                             bool refresh_required) {
    DpdfnetResamplers old;
    if (fresh.resamplers)
      old = processor_.replace_resamplers(std::move(fresh.resamplers));
    else if (fresh.needed)
      old = processor_.replace_resamplers({});
    else if (refresh_required)
      old = processor_.release_invalid_resamplers();
    last_resampler_error_.swap(fresh.error);
    return old;
  }

  void request_worker(bool resampler_refresh,
                      const DpdfnetProcessResult &result,
                      uint64_t overload_retry_delay_ns) {
    if (!resampler_refresh && result.event == DpdfnetEvent::None)
      return;

    {
      std::lock_guard<std::mutex> lock(resampler_request_mutex_);
      resampler_refresh_requested_ |= resampler_refresh;
      if (result.event != DpdfnetEvent::None) {
        const size_t index = static_cast<size_t>(result.event) - 1;
        if (index < callback_diagnostics_.size()) {
          callback_diagnostics_[index].pending = true;
          callback_diagnostics_[index].event = result.event;
          callback_diagnostics_[index].retry_delay_ns = overload_retry_delay_ns;
          callback_diagnostics_[index].message = result.message;
        }
      }
      if (result.event == DpdfnetEvent::RealtimeOverloadCircuitOpened) {
        overload_retry_deadline_ns_ =
            overload_retry_delay_ns ? os_gettime_ns() + overload_retry_delay_ns
                                    : 0;
      }
    }
    resampler_request_cv_.notify_one();
  }

  bool callback_diagnostic_pending() const {
    return std::any_of(callback_diagnostics_.begin(),
                       callback_diagnostics_.end(),
                       [](const CallbackDiagnostic &diagnostic) {
                         return diagnostic.pending;
                       });
  }

  static void log_callback_diagnostic(const CallbackDiagnostic &diagnostic) {
    if (diagnostic.event == DpdfnetEvent::RateMismatch) {
      blog(LOG_WARNING, "[obs-dpdfnet] %s; audio is passing through",
           diagnostic.message.data());
    } else if (diagnostic.event == DpdfnetEvent::ResamplerRefreshNeeded) {
      blog(LOG_WARNING,
           "[obs-dpdfnet] %s; audio is passing through while a fresh pair is "
           "prepared",
           diagnostic.message.data());
    } else if (diagnostic.event == DpdfnetEvent::ProcessingFailure) {
      blog(LOG_ERROR, "[obs-dpdfnet] processing failed: %s",
           diagnostic.message.data());
    } else if (diagnostic.event == DpdfnetEvent::CircuitOpened) {
      blog(LOG_ERROR,
           "[obs-dpdfnet] processing disabled after repeated failures: %s; "
           "audio is passing through until processing is reset",
           diagnostic.message.data());
    } else if (diagnostic.event == DpdfnetEvent::OversizedPacket) {
      blog(LOG_WARNING,
           "[obs-dpdfnet] %s; packet passed through and pipeline state was "
           "reset",
           diagnostic.message.data());
    } else if (diagnostic.event == DpdfnetEvent::CapacityInvariantFailure) {
      blog(LOG_ERROR,
           "[obs-dpdfnet] %s; packet passed through and pipeline state was "
           "reset",
           diagnostic.message.data());
    } else if (diagnostic.event ==
               DpdfnetEvent::RealtimeOverloadCircuitOpened) {
      blog(LOG_ERROR,
           "[obs-dpdfnet] sustained realtime overload: %s; noise "
           "suppression is paused and audio is passing through without it; "
           "retrying in %llu s",
           diagnostic.message.data(),
           static_cast<unsigned long long>(diagnostic.retry_delay_ns /
                                           1'000'000'000ULL));
    } else if (diagnostic.event == DpdfnetEvent::RealtimeOverloadRecovered) {
      blog(LOG_INFO,
           "[obs-dpdfnet] processing recovered after realtime overload");
    }
  }

  void resampler_worker_loop() {
    std::unique_lock<std::mutex> request_lock(resampler_request_mutex_);
    const auto ready = [this] {
      return stop_resampler_worker_ || resampler_refresh_requested_ ||
             callback_diagnostic_pending();
    };
    for (;;) {
      if (overload_retry_deadline_ns_) {
        const uint64_t now = os_gettime_ns();
        if (now < overload_retry_deadline_ns_) {
          resampler_request_cv_.wait_for(
              request_lock,
              std::chrono::nanoseconds(overload_retry_deadline_ns_ - now),
              ready);
        }
      } else {
        resampler_request_cv_.wait(request_lock, ready);
      }
      if (stop_resampler_worker_)
        return;
      const bool retry_overload =
          overload_retry_deadline_ns_ &&
          os_gettime_ns() >= overload_retry_deadline_ns_;
      if (retry_overload)
        overload_retry_deadline_ns_ = 0;
      const bool refresh_resamplers = resampler_refresh_requested_;
      resampler_refresh_requested_ = false;
      auto diagnostics = callback_diagnostics_;
      for (auto &diagnostic : callback_diagnostics_)
        diagnostic.pending = false;
      request_lock.unlock();
      for (const auto &diagnostic : diagnostics) {
        if (diagnostic.pending)
          log_callback_diagnostic(diagnostic);
      }
      if (refresh_resamplers)
        rebuild_resamplers_after_discontinuity();
      if (retry_overload)
        retry_after_overload();
      request_lock.lock();
    }
  }

  void rebuild_resamplers_after_discontinuity() {
    std::lock_guard<std::mutex> update_lock(update_mutex_);
    DpdfnetProcessorState requested;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      requested = processor_.state();
    }
    if (!requested.resampler_refresh_required)
      return;

    FreshResamplers fresh = prepare_fresh_resamplers(requested);
    const bool needs_resampling = fresh.needed;
    const std::string error = fresh.error;

    DpdfnetResamplers old;
    bool applied = false;
    std::string prepared_error = error;
    std::string discarded_error;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const DpdfnetProcessorState current = processor_.state();
      const bool still_current = current.resampler_refresh_required &&
                                 current.sample_rate == requested.sample_rate &&
                                 current.model_rate == requested.model_rate &&
                                 current.hop_size == requested.hop_size;
      if (still_current && (fresh.resamplers || !needs_resampling)) {
        if (needs_resampling)
          old = processor_.replace_resamplers(std::move(fresh.resamplers));
        else
          old = processor_.release_invalid_resamplers();
        last_resampler_error_.swap(discarded_error);
        if (needs_resampling)
          reset_timing_epoch();
        applied = true;
      } else if (still_current && !error.empty()) {
        last_resampler_error_.swap(prepared_error);
      }
    }

    if (applied) {
      if (needs_resampling) {
        blog(LOG_INFO,
             "[obs-dpdfnet] activated fresh %u Hz <-> %d Hz resamplers after "
             "a stream transition",
             requested.sample_rate, requested.model_rate);
      } else {
        blog(LOG_INFO,
             "[obs-dpdfnet] cleared stale resampler state for native %u Hz "
             "processing",
             requested.sample_rate);
      }
    } else if (!error.empty()) {
      blog(LOG_ERROR,
           "[obs-dpdfnet] could not prepare resampling after a stream "
           "transition: %s",
           error.c_str());
    }
  }

  static void enabled_changed(void *data, calldata_t *params) {
    if (!calldata_bool(params, "enabled"))
      static_cast<DpdfnetFilter *>(data)->reset_stream_boundary();
  }

  static bool paths_equivalent_for_status(const std::string &left,
                                          const std::string &right) {
    return dpdfnet_paths_equivalent(left, right);
  }
  bool processor_resampler_matches(uint32_t native_rate, int model_rate) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return processor_.resampler_matches(native_rate, model_rate);
  }

  obs_source_t *source_ = nullptr;
  mutable std::mutex mutex_;
  mutable std::mutex update_mutex_;
  DpdfnetProcessor processor_;
  DpdfnetTimestampFloor timestamp_floor_;
  DpdfnetControls controls_;
  CallbackTimings timings_;
  DpdfnetRealtimeBudgetGuard realtime_guard_;
  DpdfnetOverloadRetrySchedule overload_retries_;
  uint64_t probe_audio_ns_ = 0;
  bool custom_model_selected_ = false;
  bool show_details_ = false;
  std::string active_model_path_;
  bool have_failed_model_path_ = false;
  std::string failed_model_path_;
  std::string last_load_error_;
  std::string last_resampler_error_;
  struct obs_audio_data output_audio_ = {};
  mutable std::mutex resampler_request_mutex_;
  std::condition_variable resampler_request_cv_;
  bool resampler_refresh_requested_ = false;
  uint64_t overload_retry_deadline_ns_ = 0;
  std::array<CallbackDiagnostic, static_cast<size_t>(DpdfnetEvent::Count) - 1>
      callback_diagnostics_ = {};
  bool stop_resampler_worker_ = false;
  std::thread resampler_worker_;
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
  obs_data_set_default_string(settings, SETTING_MODEL_SELECTION,
                              DPDFNET_MODEL_QUALITY);
  obs_data_set_default_string(settings, SETTING_MODEL_PATH, "");
  obs_data_set_default_int(settings, SETTING_INPUT_CHANNEL, 0);
  obs_data_set_default_double(settings, SETTING_ATTENUATION_LIMIT_DB, 24.0);
  obs_data_set_default_double(settings, SETTING_WET_MIX, 100.0);
  obs_data_set_default_double(settings, SETTING_OUTPUT_GAIN_DB, 0.0);
  obs_data_set_default_bool(settings, SETTING_BYPASS, false);
  obs_data_set_default_bool(settings, SETTING_SHOW_DETAILS, false);
}

obs_text_info_type status_info_type(StatusSeverity severity) {
  if (severity == StatusSeverity::Error)
    return OBS_TEXT_INFO_ERROR;
  if (severity == StatusSeverity::Warning)
    return OBS_TEXT_INFO_WARNING;
  return OBS_TEXT_INFO_NORMAL;
}

bool set_info_text(obs_property_t *property, const std::string &text,
                   obs_text_info_type type) {
  if (!property)
    return false;
  const char *current = obs_property_description(property);
  if (current && text == current &&
      obs_property_text_info_type(property) == type)
    return false;
  obs_property_set_description(property, text.c_str());
  obs_property_text_set_info_type(property, type);
  return true;
}

// Returns true when a text changed, so callers can ask OBS to redraw only then.
bool update_status_properties(obs_properties_t *props, void *data) {
  if (!data)
    return false;
  const FilterStatus status = static_cast<DpdfnetFilter *>(data)->status();
  bool changed =
      set_info_text(obs_properties_get(props, "status_summary"), status.summary,
                    status_info_type(status.severity));
  changed |= set_info_text(obs_properties_get(props, "status_info"),
                           status.text, OBS_TEXT_INFO_NORMAL);
  return changed;
}

// OBS applies the setting to the source before calling this, so the status
// already reflects the change.
bool status_settings_modified(void *data, obs_properties_t *props,
                              obs_property_t *, obs_data_t *) {
  return update_status_properties(props, data);
}

bool reset_clicked(obs_properties_t *props, obs_property_t *, void *data) {
  if (data) {
    static_cast<DpdfnetFilter *>(data)->reset_state();
    update_status_properties(props, data);
  }
  return true;
}

bool refresh_clicked(obs_properties_t *props, obs_property_t *, void *data) {
  update_status_properties(props, data);
  return true;
}

bool model_selection_modified(void *data, obs_properties_t *props,
                              obs_property_t *, obs_data_t *settings) {
  const char *value = obs_data_get_string(settings, SETTING_MODEL_SELECTION);
  obs_property_t *path = obs_properties_get(props, SETTING_MODEL_PATH);
  obs_property_set_visible(path,
                           value && std::string(value) == DPDFNET_MODEL_CUSTOM);
  update_status_properties(props, data);
  return true;
}

bool show_details_modified(void *data, obs_properties_t *props,
                           obs_property_t *, obs_data_t *settings) {
  const bool show = obs_data_get_bool(settings, SETTING_SHOW_DETAILS);
  if (obs_property_t *details = obs_properties_get(props, "status_info"))
    obs_property_set_visible(details, show);
  if (obs_property_t *refresh = obs_properties_get(props, "refresh_status"))
    obs_property_set_visible(refresh, show);
  if (show)
    update_status_properties(props, data);
  return true;
}

void set_tooltip(obs_property_t *property, const char *key) {
  // Qt only word-wraps tooltips it recognizes as rich text.
  const std::string html =
      std::string("<html>") + obs_module_text(key) + "</html>";
  obs_property_set_long_description(property, html.c_str());
}

obs_properties_t *filter_properties(void *data) {
  obs_properties_t *props = obs_properties_create();
  FilterStatus status;
  bool show_details = false;
  bool custom_selected = false;
  if (data) {
    auto *filter = static_cast<DpdfnetFilter *>(data);
    status = filter->status();
    show_details = filter->show_details();
    custom_selected = filter->custom_model_selected();
  }

  obs_properties_t *status_group = obs_properties_create();
  obs_property_t *summary = obs_properties_add_text(
      status_group, "status_summary", status.summary.c_str(), OBS_TEXT_INFO);
  obs_property_text_set_info_type(summary, status_info_type(status.severity));
  obs_property_text_set_info_word_wrap(summary, true);
  obs_properties_add_group(props, "status_group",
                           obs_module_text("DPDFNet.Status"), OBS_GROUP_NORMAL,
                           status_group);

  obs_properties_t *processing = obs_properties_create();

  obs_property_t *selection =
      obs_properties_add_list(processing, SETTING_MODEL_SELECTION,
                              obs_module_text("DPDFNet.ModelSelection"),
                              OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
  obs_property_list_add_string(selection,
                               obs_module_text("DPDFNet.Model.Quality"),
                               DPDFNET_MODEL_QUALITY);
  obs_property_list_add_string(selection,
                               obs_module_text("DPDFNet.Model.LowCpu"),
                               DPDFNET_MODEL_LOW_CPU);
  obs_property_list_add_string(
      selection, obs_module_text("DPDFNet.Model.Custom"), DPDFNET_MODEL_CUSTOM);
  set_tooltip(selection, "DPDFNet.ModelSelection.Tooltip");
  obs_property_set_modified_callback2(selection, model_selection_modified,
                                      data);

  obs_property_t *model_path = obs_properties_add_path(
      processing, SETTING_MODEL_PATH, obs_module_text("DPDFNet.ModelPath"),
      OBS_PATH_FILE, "ONNX model (*.onnx);;All files (*.*)", nullptr);
  set_tooltip(model_path, "DPDFNet.ModelPath.Tooltip");
  obs_property_set_modified_callback2(model_path, status_settings_modified,
                                      data);
  obs_property_set_visible(model_path, custom_selected);

  obs_property_t *input_channel =
      obs_properties_add_list(processing, SETTING_INPUT_CHANNEL,
                              obs_module_text("DPDFNet.InputChannel"),
                              OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
  obs_property_list_add_int(input_channel,
                            obs_module_text("DPDFNet.InputChannel.Input1"), 0);
  obs_property_list_add_int(input_channel,
                            obs_module_text("DPDFNet.InputChannel.Input2"), 1);
  obs_property_list_add_int(input_channel,
                            obs_module_text("DPDFNet.InputChannel.Mix"), -1);
  set_tooltip(input_channel, "DPDFNet.InputChannel.Tooltip");
  obs_property_set_modified_callback2(input_channel, status_settings_modified,
                                      data);

  obs_property_t *attenuation = obs_properties_add_float_slider(
      processing, SETTING_ATTENUATION_LIMIT_DB,
      obs_module_text("DPDFNet.AttenuationLimit"), 0.0, 60.0, 0.5);
  obs_property_float_set_suffix(attenuation, " dB");
  set_tooltip(attenuation, "DPDFNet.AttenuationLimit.Tooltip");

  obs_property_t *wet = obs_properties_add_float_slider(
      processing, SETTING_WET_MIX, obs_module_text("DPDFNet.WetMix"), 0.0,
      100.0, 1.0);
  obs_property_float_set_suffix(wet, "%");
  set_tooltip(wet, "DPDFNet.WetMix.Tooltip");

  obs_property_t *gain = obs_properties_add_float_slider(
      processing, SETTING_OUTPUT_GAIN_DB, obs_module_text("DPDFNet.OutputGain"),
      -12.0, 12.0, 0.1);
  obs_property_float_set_suffix(gain, " dB");
  set_tooltip(gain, "DPDFNet.OutputGain.Tooltip");

  obs_property_t *bypass = obs_properties_add_bool(
      processing, SETTING_BYPASS, obs_module_text("DPDFNet.Bypass"));
  set_tooltip(bypass, "DPDFNet.Bypass.Tooltip");
  obs_property_set_modified_callback2(bypass, status_settings_modified, data);

  obs_properties_add_group(props, "processing_group",
                           obs_module_text("DPDFNet.Processing"),
                           OBS_GROUP_NORMAL, processing);

  obs_properties_t *diagnostics = obs_properties_create();
  obs_property_t *details_toggle =
      obs_properties_add_bool(diagnostics, SETTING_SHOW_DETAILS,
                              obs_module_text("DPDFNet.ShowDetails"));
  set_tooltip(details_toggle, "DPDFNet.ShowDetails.Tooltip");
  obs_property_set_modified_callback2(details_toggle, show_details_modified,
                                      data);

  obs_property_t *details = obs_properties_add_text(
      diagnostics, "status_info", status.text.c_str(), OBS_TEXT_INFO);
  obs_property_text_set_info_word_wrap(details, true);
  obs_property_set_visible(details, show_details);

  obs_property_t *refresh = obs_properties_add_button2(
      diagnostics, "refresh_status", obs_module_text("DPDFNet.RefreshStatus"),
      refresh_clicked, data);
  set_tooltip(refresh, "DPDFNet.RefreshStatus.Tooltip");
  obs_property_set_visible(refresh, show_details);

  obs_property_t *reset = obs_properties_add_button2(
      diagnostics, "reset_state", obs_module_text("DPDFNet.ResetState"),
      reset_clicked, data);
  set_tooltip(reset, "DPDFNet.ResetState.Tooltip");

  obs_properties_add_group(props, "diagnostics_group",
                           obs_module_text("DPDFNet.Diagnostics"),
                           OBS_GROUP_NORMAL, diagnostics);

  std::string version_info = obs_module_text("DPDFNet.Version");
  version_info += ": ";
  version_info += PLUGIN_VERSION;
  obs_properties_add_text(props, "version_info", version_info.c_str(),
                          OBS_TEXT_INFO);
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
