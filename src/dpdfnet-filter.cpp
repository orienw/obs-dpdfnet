// SPDX-License-Identifier: GPL-2.0-or-later

#include "dpdfnet-model.hpp"
#include "stft.hpp"

#if defined(_M_X64) || defined(__x86_64__)
#include <pmmintrin.h>
#include <xmmintrin.h>
#endif

#include <media-io/audio-resampler.h>
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

// Cap on one packet through the resampled path; anything bigger is the same
// pathological-packet case the ring capacity check already drops. Keeping it
// at half the probe block guarantees audio_resampler_resample never sees a
// larger request at runtime than the one that sized its buffer at creation.
constexpr uint32_t MAX_RESAMPLE_INPUT_FRAMES = 8192;
// Headroom added to rate-scaled ring bounds. swresample can drain its standing
// delay on top of the rate-scaled estimate (libobs sizes its own buffer as
// ceil((swr_get_delay + in) * ratio)), so this must exceed delay * ratio for
// any plausible pair, not just the sub-sample fractional carry.
constexpr size_t RESAMPLE_BOUND_SLACK = 256;

struct PacketInfo {
  uint32_t frames = 0;
  uint64_t timestamp = 0;
};

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

audio_resampler_t *create_mono_resampler(uint32_t src_rate,
                                         uint32_t dst_rate) {
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

// Trace an impulse through in -> out. Two constants fall out: the impulse
// peak index (the pair's content delay from a fresh stream head) and the
// standing holdback (frames fed minus frames emitted, the filter tail swr
// keeps buffered). Live audio starts AFTER this probe, so the wet lane lags
// the dry lane by holdback + peak; that sum is the alignment constant used to
// delay the dry lane and compensate output timestamps. The oversized blocks
// double as priming: audio_resampler_resample only grows its internal output
// buffer when a call's estimate exceeds every previous call, so feeding more
// here than the runtime cap ever allows keeps the audio callback
// reallocation-free (the block scales with the model hop so the out-resampler
// priming call also always exceeds the per-hop runtime calls).
ResampleProbe probe_resampler_pair(audio_resampler_t *in, audio_resampler_t *out,
                                   uint32_t native_rate, uint32_t model_rate,
                                   int model_hop) {
  constexpr size_t MIN_PRIME_FRAMES = 16384;
  constexpr size_t FLUSH_FRAMES = 4096;
  constexpr size_t MAX_PLAUSIBLE_DELAY = 4096;

  const size_t hop = model_hop > 0 ? static_cast<size_t>(model_hop) : 0;
  const size_t hop_native =
      (2 * hop * native_rate + model_rate - 1) / model_rate;
  const size_t prime_frames = std::max(MIN_PRIME_FRAMES, hop_native);

  std::vector<float> block(prime_frames, 0.0f);
  std::vector<float> collected;
  collected.reserve(prime_frames + 2 * FLUSH_FRAMES);

  const auto feed = [&](const float *data, uint32_t frames) {
    uint8_t *mid[MAX_AV_PLANES] = {};
    uint32_t mid_frames = 0;
    uint64_t ts_offset = 0;
    const uint8_t *input[MAX_AV_PLANES] = {
        reinterpret_cast<const uint8_t *>(data)};
    if (!audio_resampler_resample(in, mid, &mid_frames, &ts_offset, input,
                                  frames))
      return false;
    if (!mid_frames)
      return true;

    uint8_t *fin[MAX_AV_PLANES] = {};
    uint32_t fin_frames = 0;
    const uint8_t *mid_input[MAX_AV_PLANES] = {mid[0]};
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
    const float a = std::fabs(collected[i]);
    if (a > magnitude) {
      magnitude = a;
      peak = i;
    }
  }

  // A polyphase round trip keeps most of the impulse's energy at the peak.
  // The trip is a lowpass at min(native, model)/2, so the expected peak scales
  // with the passband fraction; a response far below that, or wildly late,
  // means the pair is not behaving like a plain resampler and cannot be
  // trusted for alignment.
  const float passband =
      std::min(1.0f, static_cast<float>(model_rate) /
                         static_cast<float>(native_rate));
  if (magnitude < 0.5f * passband || peak > MAX_PLAUSIBLE_DELAY ||
      holdback > MAX_PLAUSIBLE_DELAY)
    return {};

  // Everything past the kernel must be silence: residual energy in the tail
  // means the pair still has probe content queued and its steady state cannot
  // be trusted for the live stream.
  float tail = 0.0f;
  for (size_t i = peak + 512; i < collected.size(); ++i)
    tail = std::max(tail, std::fabs(collected[i]));
  if (tail > 1e-3f)
    return {};

  ResampleProbe probe;
  probe.delay_frames = holdback + peak;
  probe.delay_ns = static_cast<uint64_t>(
      static_cast<double>(holdback + peak) /
          static_cast<double>(native_rate) *
          static_cast<double>(NS_PER_SECOND) +
      0.5);
  probe.ok = true;
  return probe;
}

class DpdfnetFilter {
public:
  explicit DpdfnetFilter(obs_source_t *) {}

  ~DpdfnetFilter() {
    audio_resampler_destroy(resampler_in_);
    audio_resampler_destroy(resampler_out_);
  }

  DpdfnetFilter(const DpdfnetFilter &) = delete;
  DpdfnetFilter &operator=(const DpdfnetFilter &) = delete;

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
    bool skip_failed_model_load;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!failed_model_path_.empty() &&
          new_model_path != failed_model_path_)
        failed_model_path_.clear();
      need_load = new_model_path != loaded_model_path_;
      skip_failed_model_load =
          need_load && new_model_path == failed_model_path_;
    }

    // Build the new session OFF the audio lock: constructing an Ort::Session
    // for a multi-MB model is slow, and holding mutex_ through it would stall
    // the OBS audio callback for the entire load on every settings change.
    std::unique_ptr<DpdfnetModel> new_model;
    std::unique_ptr<StreamingStft> new_stft;
    bool load_attempted = false;
    if (need_load && !skip_failed_model_load && !new_model_path.empty()) {
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

    std::string loaded_model_name;
    std::string loaded_model_profile;
    int loaded_model_rate = 0;
    int loaded_model_hop = 0;
    if (new_model) {
      loaded_model_name = new_model->name();
      loaded_model_profile = new_model->profile();
      loaded_model_rate = new_model->sample_rate();
      loaded_model_hop = new_model->hop_size();
    }

    // Resolve the model rate this update leaves in place. Reading model_ and
    // the resampler members here without mutex_ is safe: updates are
    // serialized by update_mutex_ and the audio thread never writes them.
    int target_model_rate = 0;
    int target_model_hop = 0;
    if (new_model) {
      target_model_rate = new_model->sample_rate();
      target_model_hop = new_model->hop_size();
    } else if (model_) {
      target_model_rate = model_->sample_rate();
      target_model_hop = model_->hop_size();
    }

    const bool want_resample =
        target_model_rate > 0 && obs_rate > 0 &&
        obs_rate != static_cast<uint32_t>(target_model_rate);
    const bool pair_current = resampler_in_ && resampler_out_ &&
                              resampler_native_rate_ == obs_rate &&
                              resampler_model_rate_ == target_model_rate;
    const bool drop_resamplers =
        !want_resample && (resampler_in_ || resampler_out_);

    // Like the model session, the resampler pair is built and probed OFF the
    // audio lock: creation runs swr_init plus the impulse probe, and the probe
    // also forces both resamplers to allocate their steady-state output
    // buffers here rather than inside the audio callback.
    audio_resampler_t *new_in = nullptr;
    audio_resampler_t *new_out = nullptr;
    uint64_t new_delay_ns = 0;
    size_t new_prefill = 0;
    bool swap_resamplers = false;
    if (want_resample && !pair_current) {
      new_in = create_mono_resampler(
          obs_rate, static_cast<uint32_t>(target_model_rate));
      new_out = create_mono_resampler(
          static_cast<uint32_t>(target_model_rate), obs_rate);
      ResampleProbe probe;
      if (new_in && new_out)
        probe = probe_resampler_pair(new_in, new_out, obs_rate,
                                     static_cast<uint32_t>(target_model_rate),
                                     target_model_hop);
      if (probe.ok) {
        new_delay_ns = probe.delay_ns;
        new_prefill = probe.delay_frames;
        swap_resamplers = true;
      } else {
        audio_resampler_destroy(new_in);
        audio_resampler_destroy(new_out);
        new_in = nullptr;
        new_out = nullptr;
        blog(LOG_ERROR,
             "[obs-dpdfnet] failed to create %u <-> %d Hz resamplers; the "
             "filter will pass audio through until the rates match",
             obs_rate, target_model_rate);
      }
    }

    // Old session objects are moved out under the lock and destroyed AFTER it
    // releases: ~Ort::Session / IoBinding teardown is not instant, and running
    // it under mutex_ would stall the audio callback the same way the load did.
    std::unique_ptr<DpdfnetModel> old_model;
    std::unique_ptr<StreamingStft> old_stft;
    audio_resampler_t *old_in = nullptr;
    audio_resampler_t *old_out = nullptr;
    bool log_resamplers_active = false;
    bool log_model_loaded = false;
    bool log_keep_model = false;
    std::string log_loaded_model_path;
    std::string log_keep_model_path;

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

      if (swap_resamplers || drop_resamplers) {
        old_in = resampler_in_;
        old_out = resampler_out_;
        resampler_in_ = new_in;
        resampler_out_ = new_out;
        resampler_native_rate_ = swap_resamplers ? obs_rate : 0;
        resampler_model_rate_ = swap_resamplers ? target_model_rate : 0;
        resample_delay_ns_ = new_delay_ns;
        dry_prefill_frames_ = new_prefill;
        rate_warning_logged_ = false;
        reset_stream_locked();
        log_resamplers_active = swap_resamplers;
      }

      if (load_attempted) {
        if (new_model) {
          old_model = std::move(model_);
          old_stft = std::move(stft_);
          model_ = std::move(new_model);
          stft_ = std::move(new_stft);
          loaded_model_path_ = new_model_path;
          failed_model_path_.clear();
          resize_model_buffers_locked();
          recompute_latency_locked();
          reset_stream_locked();
          process_error_logged_ = false;
          log_model_loaded = true;
          log_loaded_model_path = loaded_model_path_;
        } else {
          failed_model_path_ = new_model_path;
          log_keep_model = true;
          log_keep_model_path =
              loaded_model_path_.empty() ? "(none)" : loaded_model_path_;
        }
      }
    }

    audio_resampler_destroy(old_in);
    audio_resampler_destroy(old_out);

    if (log_resamplers_active) {
      blog(LOG_INFO,
           "[obs-dpdfnet] resampling %u Hz <-> %d Hz (round-trip delay "
           "%zu samples, %.2f ms)",
           obs_rate, target_model_rate, new_prefill,
           static_cast<double>(new_prefill) * 1000.0 /
               static_cast<double>(obs_rate));
    }

    if (log_model_loaded) {
      blog(LOG_INFO,
           "[obs-dpdfnet] loaded %s (model %s, metadata profile %s, %d Hz, "
           "hop %d)",
           log_loaded_model_path.c_str(), loaded_model_name.c_str(),
           loaded_model_profile.c_str(), loaded_model_rate, loaded_model_hop);
    } else if (log_keep_model) {
      blog(LOG_WARNING,
           "[obs-dpdfnet] keeping previously loaded model after failed load: %s",
           log_keep_model_path.c_str());
    }

    blog(LOG_INFO,
         "[obs-dpdfnet] settings: input=%d max_suppression=%.1f dB "
         "wet=%.0f%% gain=%.2f bypass=%s",
         channel, atten, wet * 100.0, gain, bypass ? "true" : "false");
  }

  struct obs_audio_data *filter_audio(struct obs_audio_data *audio) {
    [[maybe_unused]] DenormalModeGuard denormal_guard;

    if (!audio || !audio->frames)
      return audio;

    std::lock_guard<std::mutex> lock(mutex_);

    const uint32_t obs_rate = audio_output_get_sample_rate(obs_get_audio());
    const size_t obs_channels = std::clamp<size_t>(
        audio_output_get_channels(obs_get_audio()), 1, MAX_AV_PLANES);
    if (obs_rate != sample_rate_ || obs_channels != channels_) {
      // OBS rebuilds the audio graph on settings changes; this resize only runs
      // on that rare reconfiguration, not during steady-state packets.
      sample_rate_ = obs_rate;
      channels_ = obs_channels;
      resize_channel_buffers();
      reset_stream_locked();
      rate_warning_logged_ = false;
    }

    if (bypass_ || !model_ || !stft_)
      return audio;

    if (sample_rate_ != static_cast<uint32_t>(model_->sample_rate()) &&
        !resample_path_) {
      if (!rate_warning_logged_) {
        blog(LOG_WARNING,
             "[obs-dpdfnet] OBS sample rate is %u Hz but the loaded model is "
             "%d Hz; passing audio through (change any filter setting or "
             "restart OBS to activate resampling)",
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
    if (!push_input(audio)) {
      if (!process_error_logged_) {
        blog(LOG_ERROR, "[obs-dpdfnet] input resampling failed");
        process_error_logged_ = true;
      }
      reset_stream_locked();
      return audio;
    }

    try {
      const size_t processed_hops = process_available_hops();
      if (processed_hops)
        process_error_logged_ = false;
    } catch (const std::exception &ex) {
      if (!process_error_logged_) {
        blog(LOG_ERROR, "[obs-dpdfnet] processing failed: %s", ex.what());
        process_error_logged_ = true;
      }
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
    // The reset button doubles as the retry affordance: forget a failed model
    // path and a logged processing failure so the next update() or packet gets
    // a fresh attempt (and a fresh log line) without toggling paths.
    failed_model_path_.clear();
    process_error_logged_ = false;
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

  void recompute_resample_path_locked() {
    resample_path_ = model_ && resampler_in_ && resampler_out_ &&
                     resampler_native_rate_ == sample_rate_ &&
                     resampler_model_rate_ == model_->sample_rate();
  }

  void recompute_latency_locked() {
    hop_latency_ns_ = 0;
    if (model_)
      hop_latency_ns_ = static_cast<uint64_t>(
          static_cast<double>(model_->hop_size()) /
          static_cast<double>(model_->sample_rate()) * NS_PER_SECOND);
    output_latency_ns_ =
        hop_latency_ns_ + (resample_path_ ? resample_delay_ns_ : 0);
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
    recompute_resample_path_locked();
    recompute_latency_locked();

    input_mono_.clear();
    output_mono_.clear();
    for (auto &buffer : dry_buffers_)
      buffer.clear();
    info_queue_.clear();

    // The wet lane lags the dry lane by the pair's standing holdback plus its
    // content delay (measured by the probe); preloading that much silence
    // keeps the two index-aligned at the mixer. The swr contexts themselves
    // are intentionally NOT reset here: recreating them would allocate on the
    // audio thread, the holdback is a stable property of the filter so the
    // prefill stays correct across resets, and the cost is only the last
    // ~holdback samples of pre-reset audio leaking into the first packet's
    // wet lane against prefill silence. Same reasoning covers a stale pair
    // reactivated when OBS flips back to a previously probed rate.
    // zero_scratch_ never shrinks below RING_RESERVE_SAMPLES, which bounds
    // dry_prefill_frames_, so this push stays within preallocated capacity.
    if (resample_path_ && dry_prefill_frames_) {
      for (auto &buffer : dry_buffers_)
        buffer.push(zero_scratch_.data(), dry_prefill_frames_);
    }

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

  // Rate-scaled ring-growth bounds for the resampled path; ceiling division
  // plus slack covers swresample draining its standing delay on top of the
  // rate-scaled output.
  size_t to_model_frames(size_t native_frames) const {
    const auto num = native_frames * static_cast<size_t>(resampler_model_rate_);
    const auto den = static_cast<size_t>(resampler_native_rate_);
    return (num + den - 1) / den + RESAMPLE_BOUND_SLACK;
  }

  size_t to_native_frames(size_t model_frames) const {
    const auto num = model_frames * static_cast<size_t>(resampler_native_rate_);
    const auto den = static_cast<size_t>(resampler_model_rate_);
    return (num + den - 1) / den + RESAMPLE_BOUND_SLACK;
  }

  // True if buffering this packet stays within every ring's preallocated
  // capacity, so no push() reallocates on the audio thread.
  bool buffers_can_accept(uint32_t frames) const {
    if (info_queue_.size() + 1 > info_queue_.capacity())
      return false;
    if (resample_path_ && frames > MAX_RESAMPLE_INPUT_FRAMES)
      return false;
    const size_t input_gain = resample_path_ ? to_model_frames(frames) : frames;
    if (input_mono_.size() + input_gain > input_mono_.capacity())
      return false;
    // Upper bound on what synthesis can append before this packet drains.
    const size_t synthesis_gain =
        resample_path_ ? to_native_frames(input_mono_.size() + input_gain)
                       : input_mono_.size() + frames;
    if (output_mono_.size() + synthesis_gain > output_mono_.capacity())
      return false;
    for (size_t channel = 0; channel < channels_; ++channel)
      if (dry_buffers_[channel].size() + frames >
          dry_buffers_[channel].capacity())
        return false;
    return true;
  }

  bool push_input(struct obs_audio_data *audio) {
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

    const float *mono = nullptr;
    if (input_channel_ >= 0 &&
        static_cast<size_t>(input_channel_) < channels_) {
      const auto selected_channel = static_cast<size_t>(input_channel_);
      const float *selected =
          reinterpret_cast<const float *>(audio->data[selected_channel]);
      mono = selected ? selected : (ch0 ? ch0 : zero_scratch_.data());
    } else {
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

        mono_scratch_[frame] = mixed_channels
                                   ? mixed / static_cast<float>(mixed_channels)
                                   : fallback;
      }
      mono = mono_scratch_.data();
    }

    if (!resample_path_) {
      input_mono_.push(mono, frames);
      return true;
    }

    uint8_t *out[MAX_AV_PLANES] = {};
    uint32_t out_frames = 0;
    uint64_t ts_offset = 0;
    const uint8_t *input[MAX_AV_PLANES] = {
        reinterpret_cast<const uint8_t *>(mono)};
    if (!audio_resampler_resample(resampler_in_, out, &out_frames, &ts_offset,
                                  input, frames))
      return false;
    if (out_frames)
      input_mono_.push(reinterpret_cast<const float *>(out[0]), out_frames);
    return true;
  }

  size_t process_available_hops() {
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
        uint8_t *out[MAX_AV_PLANES] = {};
        uint32_t out_frames = 0;
        uint64_t ts_offset = 0;
        const uint8_t *input[MAX_AV_PLANES] = {
            reinterpret_cast<const uint8_t *>(enhanced_hop_.data())};
        if (!audio_resampler_resample(resampler_out_, out, &out_frames,
                                      &ts_offset, input,
                                      static_cast<uint32_t>(hop_size)))
          throw std::runtime_error("output resampling failed");
        if (out_frames)
          output_mono_.push(reinterpret_cast<const float *>(out[0]),
                            out_frames);
      } else {
        output_mono_.push(enhanced_hop_.data(), hop_size);
      }
      input_mono_.pop(hop_size);
      ++processed_hops;
    }

    return processed_hops;
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
    output_audio_.timestamp = info.timestamp > output_latency_ns_
                                  ? info.timestamp - output_latency_ns_
                                  : 0;

    info_queue_.pop(1);
    return &output_audio_;
  }

  std::mutex mutex_;
  std::mutex update_mutex_;

  std::string loaded_model_path_;
  std::string failed_model_path_;
  std::unique_ptr<DpdfnetModel> model_;
  std::unique_ptr<StreamingStft> stft_;

  // Owned mono resampler pair bridging the OBS session rate and the model
  // rate; null when the rates match. Only update() writes these (under
  // mutex_); the audio thread reads them through resample_path_.
  audio_resampler_t *resampler_in_ = nullptr;
  audio_resampler_t *resampler_out_ = nullptr;
  uint32_t resampler_native_rate_ = 0;
  int resampler_model_rate_ = 0;
  uint64_t resample_delay_ns_ = 0;
  size_t dry_prefill_frames_ = 0;
  bool resample_path_ = false;

  uint32_t sample_rate_ = 0;
  size_t channels_ = 0;
  uint64_t last_timestamp_ = 0;
  uint64_t hop_latency_ns_ = 0;
  uint64_t output_latency_ns_ = 0;

  double attenuation_limit_db_ = 24.0;
  int input_channel_ = 0;
  double wet_mix_ = 1.0;
  float output_gain_ = 1.0f;
  bool bypass_ = false;
  bool rate_warning_logged_ = false;
  bool process_error_logged_ = false;

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
