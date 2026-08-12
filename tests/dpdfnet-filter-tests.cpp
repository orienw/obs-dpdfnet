// SPDX-License-Identifier: GPL-2.0-or-later

#include "../src/dpdfnet-processor.hpp"
#include "../src/dpdfnet-settings.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _MSC_VER
#include <malloc.h>
#endif

namespace filter_test_instrumentation {
thread_local bool callback_scope = false;
std::atomic<uint64_t> callback_allocations{0};

void record_allocation() {
  if (callback_scope)
    callback_allocations.fetch_add(1, std::memory_order_relaxed);
}

void *allocate(size_t size) {
  record_allocation();
  if (void *memory = std::malloc(size ? size : 1))
    return memory;
  throw std::bad_alloc();
}

void *allocate_aligned(size_t size, size_t alignment) {
  record_allocation();
#ifdef _MSC_VER
  if (void *memory = _aligned_malloc(size ? size : 1, alignment))
    return memory;
#else
  void *memory = nullptr;
  if (posix_memalign(&memory, alignment, size ? size : 1) == 0)
    return memory;
#endif
  throw std::bad_alloc();
}

void free_aligned(void *memory) noexcept {
#ifdef _MSC_VER
  _aligned_free(memory);
#else
  std::free(memory);
#endif
}
} // namespace filter_test_instrumentation

void *operator new(size_t size) {
  return filter_test_instrumentation::allocate(size);
}

void *operator new[](size_t size) {
  return filter_test_instrumentation::allocate(size);
}

void *operator new(size_t size, const std::nothrow_t &) noexcept {
  try {
    return ::operator new(size);
  } catch (...) {
    return nullptr;
  }
}

void *operator new[](size_t size, const std::nothrow_t &) noexcept {
  try {
    return ::operator new[](size);
  } catch (...) {
    return nullptr;
  }
}

void *operator new(size_t size, std::align_val_t alignment) {
  return filter_test_instrumentation::allocate_aligned(
      size, static_cast<size_t>(alignment));
}

void *operator new[](size_t size, std::align_val_t alignment) {
  return filter_test_instrumentation::allocate_aligned(
      size, static_cast<size_t>(alignment));
}

void *operator new(size_t size, std::align_val_t alignment,
                   const std::nothrow_t &) noexcept {
  try {
    return ::operator new(size, alignment);
  } catch (...) {
    return nullptr;
  }
}

void *operator new[](size_t size, std::align_val_t alignment,
                     const std::nothrow_t &) noexcept {
  try {
    return ::operator new[](size, alignment);
  } catch (...) {
    return nullptr;
  }
}

void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, size_t) noexcept { std::free(memory); }
void operator delete[](void *memory, size_t) noexcept { std::free(memory); }
void operator delete(void *memory, const std::nothrow_t &) noexcept {
  std::free(memory);
}
void operator delete[](void *memory, const std::nothrow_t &) noexcept {
  std::free(memory);
}
void operator delete(void *memory, std::align_val_t) noexcept {
  filter_test_instrumentation::free_aligned(memory);
}
void operator delete[](void *memory, std::align_val_t) noexcept {
  filter_test_instrumentation::free_aligned(memory);
}
void operator delete(void *memory, size_t, std::align_val_t) noexcept {
  filter_test_instrumentation::free_aligned(memory);
}
void operator delete[](void *memory, size_t, std::align_val_t) noexcept {
  filter_test_instrumentation::free_aligned(memory);
}
void operator delete(void *memory, std::align_val_t,
                     const std::nothrow_t &) noexcept {
  filter_test_instrumentation::free_aligned(memory);
}
void operator delete[](void *memory, std::align_val_t,
                       const std::nothrow_t &) noexcept {
  filter_test_instrumentation::free_aligned(memory);
}

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-dpdfnet", "en-US")

extern struct obs_source_info dpdfnet_filter_info;

namespace {
constexpr const char *TEST_SOURCE_ID = "dpdfnet_filter_test_source";
constexpr const char *BARRIER_FILTER_ID = "dpdfnet_filter_test_barrier";
constexpr uint32_t SAMPLE_RATE = 48000;
constexpr uint32_t PACKET_FRAMES = 480;
constexpr uint64_t PACKET_DURATION_NS = 10000000;
constexpr size_t SNAPSHOT_SAMPLES = 32;

class TestFailure : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

void require(bool condition, const std::string &message) {
  if (!condition)
    throw TestFailure(message);
}

class CallbackScope {
public:
  CallbackScope() { filter_test_instrumentation::callback_scope = true; }
  ~CallbackScope() { filter_test_instrumentation::callback_scope = false; }

  CallbackScope(const CallbackScope &) = delete;
  CallbackScope &operator=(const CallbackScope &) = delete;
};

class CallbackLogProbe {
public:
  CallbackLogProbe() {
    base_get_log_handler(&previous_handler_, &previous_parameter_);
    base_set_log_handler(handle_log, this);
  }

  ~CallbackLogProbe() {
    base_set_log_handler(previous_handler_, previous_parameter_);
  }

  CallbackLogProbe(const CallbackLogProbe &) = delete;
  CallbackLogProbe &operator=(const CallbackLogProbe &) = delete;

  uint64_t synchronous_logs() const {
    return synchronous_logs_.load(std::memory_order_relaxed);
  }

private:
  static void handle_log(int level, const char *message, va_list arguments,
                         void *parameter) {
    auto *probe = static_cast<CallbackLogProbe *>(parameter);
    if (filter_test_instrumentation::callback_scope) {
      probe->synchronous_logs_.fetch_add(1, std::memory_order_relaxed);
    }
    if (probe->previous_handler_) {
      probe->previous_handler_(level, message, arguments,
                               probe->previous_parameter_);
    }
  }

  log_handler_t previous_handler_ = nullptr;
  void *previous_parameter_ = nullptr;
  std::atomic<uint64_t> synchronous_logs_{0};
};

class ObsData {
public:
  ObsData() : data_(obs_data_create()) {
    if (!data_)
      throw TestFailure("obs_data_create failed");
  }
  ~ObsData() { obs_data_release(data_); }

  ObsData(const ObsData &) = delete;
  ObsData &operator=(const ObsData &) = delete;

  operator obs_data_t *() const { return data_; }

private:
  obs_data_t *data_ = nullptr;
};

class ObsSource {
public:
  explicit ObsSource(obs_source_t *source) : source_(source) {
    if (!source_)
      throw TestFailure("obs_source_create_private failed");
  }
  ~ObsSource() { obs_source_release(source_); }

  ObsSource(const ObsSource &) = delete;
  ObsSource &operator=(const ObsSource &) = delete;

  operator obs_source_t *() const { return source_; }

private:
  obs_source_t *source_ = nullptr;
};

class ObsProperties {
public:
  explicit ObsProperties(obs_properties_t *properties)
      : properties_(properties) {
    if (!properties_)
      throw TestFailure("get_properties returned null");
  }
  ~ObsProperties() { obs_properties_destroy(properties_); }

  ObsProperties(const ObsProperties &) = delete;
  ObsProperties &operator=(const ObsProperties &) = delete;

  operator obs_properties_t *() const { return properties_; }

private:
  obs_properties_t *properties_ = nullptr;
};

class DirectFilter {
public:
  DirectFilter(obs_data_t *settings, obs_source_t *source)
      : data_(dpdfnet_filter_info.create(settings, source)) {
    if (!data_)
      throw TestFailure("DPDFNet filter create callback returned null");
  }
  ~DirectFilter() { dpdfnet_filter_info.destroy(data_); }

  DirectFilter(const DirectFilter &) = delete;
  DirectFilter &operator=(const DirectFilter &) = delete;

  void *get() const { return data_; }

private:
  void *data_ = nullptr;
};

void configure_custom_model(obs_data_t *settings,
                            const std::string &model_path) {
  dpdfnet_filter_info.get_defaults(settings);
  obs_data_set_string(settings, "model_selection", DPDFNET_MODEL_CUSTOM);
  obs_data_set_string(settings, "model_path", model_path.c_str());
  obs_data_set_int(settings, "input_channel", 0);
  obs_data_set_double(settings, "attenuation_limit_db", 24.0);
  obs_data_set_double(settings, "wet_mix", 100.0);
  obs_data_set_double(settings, "output_gain_db", 0.0);
  obs_data_set_bool(settings, "bypass", false);
}

void reset_audio(enum speaker_layout speakers,
                 uint32_t sample_rate = SAMPLE_RATE) {
  struct obs_audio_info info = {};
  info.samples_per_sec = sample_rate;
  info.speakers = speakers;
  require(obs_reset_audio(&info), "obs_reset_audio failed");
}

const char *test_source_name(void *) { return "DPDFNet filter test source"; }

char test_source_token;

void *test_source_create(obs_data_t *, obs_source_t *) {
  return &test_source_token;
}

void test_source_destroy(void *) {}

struct BarrierState {
  std::mutex mutex;
  std::condition_variable condition;
  bool armed = false;
  bool entered = false;
  bool release = false;
  bool audio_valid = true;
  uint32_t frames = 0;
  std::array<const float *, 2> planes = {};
  std::array<std::array<float, SNAPSHOT_SAMPLES>, 2> samples = {};
};

BarrierState *active_barrier = nullptr;

const char *barrier_filter_name(void *) {
  return "DPDFNet filter test barrier";
}

void *barrier_filter_create(obs_data_t *, obs_source_t *) {
  return active_barrier;
}

void barrier_filter_destroy(void *) {}

struct obs_audio_data *barrier_filter_audio(void *data,
                                            struct obs_audio_data *audio) {
  auto *barrier = static_cast<BarrierState *>(data);
  std::unique_lock<std::mutex> lock(barrier->mutex);
  if (!barrier->armed)
    return audio;

  barrier->armed = false;
  barrier->entered = true;
  barrier->frames = audio ? audio->frames : 0;
  const size_t count =
      std::min<size_t>(barrier->frames, barrier->samples[0].size());
  for (size_t channel = 0; channel < barrier->planes.size(); ++channel) {
    barrier->planes[channel] =
        audio ? reinterpret_cast<const float *>(audio->data[channel]) : nullptr;
    if (!barrier->planes[channel]) {
      barrier->audio_valid = false;
      continue;
    }
    std::copy_n(barrier->planes[channel], count,
                barrier->samples[channel].begin());
  }
  barrier->condition.notify_all();
  barrier->condition.wait(lock, [barrier] { return barrier->release; });

  for (size_t channel = 0; channel < barrier->planes.size(); ++channel) {
    if (!barrier->planes[channel] ||
        !std::equal(barrier->samples[channel].begin(),
                    barrier->samples[channel].begin() + count,
                    barrier->planes[channel])) {
      barrier->audio_valid = false;
    }
  }
  return audio;
}

struct obs_source_info make_test_source_info() {
  struct obs_source_info info = {};
  info.id = TEST_SOURCE_ID;
  info.type = OBS_SOURCE_TYPE_INPUT;
  info.output_flags = OBS_SOURCE_AUDIO;
  info.get_name = test_source_name;
  info.create = test_source_create;
  info.destroy = test_source_destroy;
  return info;
}

struct obs_source_info make_barrier_filter_info() {
  struct obs_source_info info = {};
  info.id = BARRIER_FILTER_ID;
  info.type = OBS_SOURCE_TYPE_FILTER;
  info.output_flags = OBS_SOURCE_AUDIO;
  info.get_name = barrier_filter_name;
  info.create = barrier_filter_create;
  info.destroy = barrier_filter_destroy;
  info.filter_audio = barrier_filter_audio;
  return info;
}

struct PacketStorage {
  std::array<float, PACKET_FRAMES> left = {};
  std::array<float, PACKET_FRAMES> right = {};

  PacketStorage() {
    for (size_t i = 0; i < left.size(); ++i) {
      const float phase = static_cast<float>(i) / SAMPLE_RATE;
      left[i] =
          0.3f * std::sin(2.0f * 3.14159265358979323846f * 440.0f * phase);
      right[i] =
          0.2f * std::sin(2.0f * 3.14159265358979323846f * 660.0f * phase);
    }
  }

  struct obs_audio_data direct_packet(uint64_t timestamp) {
    struct obs_audio_data audio = {};
    audio.data[0] = reinterpret_cast<uint8_t *>(left.data());
    audio.data[1] = reinterpret_cast<uint8_t *>(right.data());
    audio.frames = PACKET_FRAMES;
    audio.timestamp = timestamp;
    return audio;
  }

  struct obs_source_audio source_packet(uint64_t timestamp) {
    struct obs_source_audio audio = {};
    audio.data[0] = reinterpret_cast<const uint8_t *>(left.data());
    audio.data[1] = reinterpret_cast<const uint8_t *>(right.data());
    audio.frames = PACKET_FRAMES;
    audio.speakers = SPEAKERS_STEREO;
    audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
    audio.samples_per_sec = SAMPLE_RATE;
    audio.timestamp = timestamp;
    return audio;
  }
};

struct obs_audio_data *wait_for_processed_audio(void *filter,
                                                PacketStorage &storage,
                                                uint64_t start_timestamp) {
  for (uint64_t packet = 0; packet < 64; ++packet) {
    struct obs_audio_data input =
        storage.direct_packet(start_timestamp + packet * PACKET_DURATION_NS);
    struct obs_audio_data *output =
        dpdfnet_filter_info.filter_audio(filter, &input);
    if (output && output != &input)
      return output;
  }
  throw TestFailure("filter produced no processed packet after 64 callbacks");
}

void verify_status_property(obs_properties_t *properties) {
  obs_property_t *summary = obs_properties_get(properties, "status_summary");
  require(summary != nullptr, "status summary is missing");
  const char *summary_description = obs_property_description(summary);
  require(summary_description &&
              std::string(summary_description) == "Processing normally.",
          "status summary is not concise or does not report normal operation");

  obs_property_t *status = obs_properties_get(properties, "status_info");
  require(status != nullptr, "status property is missing");
  const char *description = obs_property_description(status);
  require(description &&
              std::string(description).find("Active:") != std::string::npos,
          "status property does not report an active model");
}

void verify_properties_layout(obs_properties_t *properties,
                              obs_data_t *settings) {
  constexpr std::array<const char *, 4> root_order = {
      "status_summary", "processing_group", "diagnostics_group",
      "version_info"};
  obs_property_t *root_property = obs_properties_first(properties);
  for (const char *expected : root_order) {
    require(root_property != nullptr, "settings layout is missing a root row");
    require(std::string(obs_property_name(root_property)) == expected,
            "settings layout root rows are in the wrong order");
    obs_property_next(&root_property);
  }
  require(root_property == nullptr,
          "settings layout contains an unexpected root row");

  obs_property_t *processing_group =
      obs_properties_get(properties, "processing_group");
  obs_property_t *diagnostics_group =
      obs_properties_get(properties, "diagnostics_group");
  require(processing_group &&
              obs_property_get_type(processing_group) == OBS_PROPERTY_GROUP &&
              obs_property_group_type(processing_group) == OBS_GROUP_NORMAL,
          "processing controls are not in a normal group");
  require(diagnostics_group &&
              obs_property_get_type(diagnostics_group) == OBS_PROPERTY_GROUP &&
              obs_property_group_type(diagnostics_group) == OBS_GROUP_NORMAL,
          "diagnostics controls are not in a normal group");

  obs_properties_t *processing = obs_property_group_content(processing_group);
  obs_properties_t *diagnostics = obs_property_group_content(diagnostics_group);
  for (const char *name :
       {"model_selection", "model_path", "input_channel",
        "attenuation_limit_db", "wet_mix", "output_gain_db", "bypass"}) {
    require(obs_properties_get(processing, name) != nullptr,
            "processing group is missing a control");
  }
  for (const char *name : {"status_info", "refresh_status", "reset_state"}) {
    require(obs_properties_get(diagnostics, name) != nullptr,
            "diagnostics group is missing a control");
  }

  for (const char *name : {"model_selection", "model_path", "input_channel",
                           "attenuation_limit_db", "wet_mix", "output_gain_db",
                           "bypass", "refresh_status", "reset_state"}) {
    obs_property_t *property = obs_properties_get(properties, name);
    const char *tooltip =
        property ? obs_property_long_description(property) : nullptr;
    require(tooltip && *tooltip, "settings control is missing its help text");
  }

  require(!obs_data_has_user_value(settings, "processing_group") &&
              !obs_data_has_user_value(settings, "diagnostics_group") &&
              !obs_data_has_user_value(settings, "status_summary"),
          "UI-only layout properties leaked into saved settings");
}

void test_empty_custom_model_error() {
  ObsData settings;
  configure_custom_model(settings, "");
  ObsSource source(obs_source_create_private(
      TEST_SOURCE_ID, "empty custom model owner", settings));
  DirectFilter filter(settings, source);

  const auto require_cached_error = [&] {
    ObsProperties properties(dpdfnet_filter_info.get_properties(filter.get()));
    obs_property_t *status = obs_properties_get(properties, "status_info");
    require(status != nullptr, "empty custom model status is missing");
    const char *description = obs_property_description(status);
    require(description && std::string(description)
                                   .find("no custom ONNX model was selected") !=
                               std::string::npos,
            "empty custom model error was not retained in status");
  };

  require_cached_error();
  dpdfnet_filter_info.update(filter.get(), settings);
  require_cached_error();
}

void test_direct_callbacks(const std::string &model_path) {
  ObsData settings;
  configure_custom_model(settings, model_path);
  ObsSource source(obs_source_create_private(TEST_SOURCE_ID,
                                             "direct filter owner", settings));
  CallbackLogProbe log_probe;
  DirectFilter filter(settings, source);

  {
    ObsProperties properties(dpdfnet_filter_info.get_properties(filter.get()));
    obs_property_t *model_path_property =
        obs_properties_get(properties, "model_path");
    require(model_path_property && obs_property_visible(model_path_property),
            "custom model path property is not visible");

    obs_property_t *selection =
        obs_properties_get(properties, "model_selection");
    require(selection && obs_property_modified(selection, settings),
            "model selection modified callback did not request a refresh");
    require(obs_property_visible(model_path_property),
            "custom model path was hidden after the modified callback");

    verify_properties_layout(properties, settings);
    verify_status_property(properties);
    obs_property_t *refresh = obs_properties_get(properties, "refresh_status");
    require(refresh && obs_property_button_clicked(refresh, nullptr),
            "Refresh button callback failed");
    verify_status_property(properties);

    obs_property_t *reset = obs_properties_get(properties, "reset_state");
    require(reset && obs_property_button_clicked(reset, nullptr),
            "Reset button callback failed");
    verify_status_property(properties);
  }

  obs_source_set_enabled(source, false);
  obs_source_set_enabled(source, true);
  require(obs_source_enabled(source), "source did not return to enabled state");

  PacketStorage storage;
  struct obs_audio_data *output =
      wait_for_processed_audio(filter.get(), storage, 1000000000ULL);
  require(output->frames > 0 && output->data[0] && output->data[1],
          "processed callback returned incomplete audio");

  const struct obs_audio_data published = *output;
  const size_t frames = published.frames;
  std::array<std::vector<float>, 2> snapshot;
  for (size_t channel = 0; channel < snapshot.size(); ++channel) {
    const float *plane =
        reinterpret_cast<const float *>(published.data[channel]);
    snapshot[channel].assign(plane, plane + frames);
  }

  reset_audio(SPEAKERS_7POINT1);
  dpdfnet_filter_info.update(filter.get(), settings);

  // Reuse likely freed, ring-sized blocks before reading the published packet.
  // A conforming filter keeps that packet alive until the next audio callback,
  // regardless of settings or OBS-format updates in between.
  std::vector<std::vector<float>> allocation_churn;
  allocation_churn.reserve(64);
  for (size_t i = 0; i < 64; ++i)
    allocation_churn.emplace_back(16384, 1000.0f + static_cast<float>(i));

  require(output->frames == published.frames &&
              output->timestamp == published.timestamp,
          "intervening update mutated the published audio descriptor");
  for (size_t channel = 0; channel < snapshot.size(); ++channel) {
    require(output->data[channel] == published.data[channel],
            "intervening update replaced a published audio pointer");
    const float *plane =
        reinterpret_cast<const float *>(published.data[channel]);
    require(
        std::equal(snapshot[channel].begin(), snapshot[channel].end(), plane),
        "intervening update invalidated published audio samples");
  }

  reset_audio(SPEAKERS_STEREO);
  dpdfnet_filter_info.update(filter.get(), settings);
  (void)wait_for_processed_audio(filter.get(), storage, 3000000000ULL);

  constexpr uint64_t steady_start = 4000000000ULL;
  for (uint64_t packet = 0; packet < 8; ++packet) {
    struct obs_audio_data input =
        storage.direct_packet(steady_start + packet * PACKET_DURATION_NS);
    (void)dpdfnet_filter_info.filter_audio(filter.get(), &input);
  }
  struct obs_audio_data steady =
      storage.direct_packet(steady_start + 8 * PACKET_DURATION_NS);
  const uint64_t steady_allocations_before =
      filter_test_instrumentation::callback_allocations.load(
          std::memory_order_relaxed);
  struct obs_audio_data *steady_output = nullptr;
  {
    CallbackScope callback_scope;
    steady_output = dpdfnet_filter_info.filter_audio(filter.get(), &steady);
  }
  require(steady_output && steady_output != &steady,
          "steady-state callback did not produce processed audio");
  require(filter_test_instrumentation::callback_allocations.load(
              std::memory_order_relaxed) == steady_allocations_before,
          "steady-state processing allocated on the audio callback");

  std::array<std::vector<float>, 2> oversized_storage = {
      std::vector<float>(DPDFNET_MAX_REALTIME_PACKET_FRAMES + 1, 0.025f),
      std::vector<float>(DPDFNET_MAX_REALTIME_PACKET_FRAMES + 1, -0.025f)};
  struct obs_audio_data oversized = {};
  oversized.data[0] = reinterpret_cast<uint8_t *>(oversized_storage[0].data());
  oversized.data[1] = reinterpret_cast<uint8_t *>(oversized_storage[1].data());
  oversized.frames = DPDFNET_MAX_REALTIME_PACKET_FRAMES + 1;
  oversized.timestamp = steady_start + 9 * PACKET_DURATION_NS;
  const uint64_t oversized_allocations_before =
      filter_test_instrumentation::callback_allocations.load(
          std::memory_order_relaxed);
  const uint64_t oversized_logs_before = log_probe.synchronous_logs();
  struct obs_audio_data *oversized_output = nullptr;
  {
    CallbackScope callback_scope;
    oversized_output =
        dpdfnet_filter_info.filter_audio(filter.get(), &oversized);
  }
  require(oversized_output && oversized_output->frames == oversized.frames &&
              oversized_output->data[0] == oversized.data[0] &&
              oversized_output->data[1] == oversized.data[1],
          "oversized callback did not fail open with the original audio");
  require(filter_test_instrumentation::callback_allocations.load(
              std::memory_order_relaxed) == oversized_allocations_before,
          "oversized callback allocated on the audio callback");
  require(log_probe.synchronous_logs() == oversized_logs_before,
          "oversized callback logged synchronously on the audio callback");
  {
    ObsProperties properties(dpdfnet_filter_info.get_properties(filter.get()));
    obs_property_t *status = obs_properties_get(properties, "status_info");
    const char *description =
        status ? obs_property_description(status) : nullptr;
    require(description &&
                std::string(description).find("8192-frame realtime limit") !=
                    std::string::npos,
            "oversized callback was not retained in filter diagnostics");
  }

  reset_audio(SPEAKERS_STEREO, 44100);
  struct obs_audio_data mismatch = storage.direct_packet(5000000000ULL);
  const uint64_t allocations_before =
      filter_test_instrumentation::callback_allocations.load(
          std::memory_order_relaxed);
  const uint64_t logs_before = log_probe.synchronous_logs();
  {
    CallbackScope callback_scope;
    (void)dpdfnet_filter_info.filter_audio(filter.get(), &mismatch);
  }
  const uint64_t allocations_after =
      filter_test_instrumentation::callback_allocations.load(
          std::memory_order_relaxed);
  require(allocations_after == allocations_before,
          "sample-rate transition allocated on the audio callback");
  require(log_probe.synchronous_logs() == logs_before,
          "sample-rate mismatch logged synchronously on the audio callback");

  reset_audio(SPEAKERS_STEREO);
  dpdfnet_filter_info.update(filter.get(), settings);
}

struct CaptureState {
  std::atomic<uint64_t> callbacks{0};
};

void capture_audio(void *data, obs_source_t *, const struct audio_data *audio,
                   bool) {
  if (audio && audio->frames)
    static_cast<CaptureState *>(data)->callbacks.fetch_add(
        1, std::memory_order_relaxed);
}

void test_obs_lifecycle(const std::string &model_path) {
  ObsData filter_settings;
  configure_custom_model(filter_settings, model_path);
  ObsSource source(obs_source_create_private(
      TEST_SOURCE_ID, "filter lifecycle source", nullptr));
  ObsSource filter(obs_source_create_private(
      "obs_dpdfnet_filter", "DPDFNet lifecycle filter", filter_settings));

  BarrierState barrier;
  active_barrier = &barrier;
  ObsSource barrier_filter(obs_source_create_private(
      BARRIER_FILTER_ID, "post-DPDFNet barrier", nullptr));
  active_barrier = nullptr;

  // libobs traverses audio filters from the back of its array. Adding the
  // DPDFNet filter first makes the barrier run after it and hold libobs's
  // filter mutex while another thread updates, toggles, and removes DPDFNet.
  obs_source_filter_add(source, filter);
  obs_source_filter_add(source, barrier_filter);
  require(obs_source_filter_count(source) == 2,
          "failed to attach lifecycle filters");

  CaptureState capture;
  obs_source_add_audio_capture_callback(source, capture_audio, &capture);

  {
    std::lock_guard<std::mutex> lock(barrier.mutex);
    barrier.armed = true;
  }

  PacketStorage storage;
  std::thread producer([&] {
    const uint64_t start = os_gettime_ns();
    for (uint64_t packet = 0; packet < 64; ++packet) {
      const struct obs_source_audio audio =
          storage.source_packet(start + packet * PACKET_DURATION_NS);
      obs_source_output_audio(source, &audio);
      std::lock_guard<std::mutex> lock(barrier.mutex);
      if (barrier.entered)
        return;
    }
  });

  bool entered = false;
  {
    std::unique_lock<std::mutex> lock(barrier.mutex);
    entered = barrier.condition.wait_for(lock, std::chrono::seconds(20),
                                         [&] { return barrier.entered; });
  }
  if (!entered) {
    producer.join();
    throw TestFailure("post-DPDFNet barrier received no processed audio");
  }

  ObsData update;
  obs_data_set_int(update, "input_channel", 1);
  obs_data_set_double(update, "wet_mix", 50.0);
  obs_data_set_double(update, "output_gain_db", -1.0);
  obs_source_update(filter, update);
  obs_source_set_enabled(filter, false);
  obs_source_set_enabled(filter, true);

  std::mutex remove_mutex;
  std::condition_variable remove_condition;
  bool remove_started = false;
  std::thread remover([&] {
    {
      std::lock_guard<std::mutex> lock(remove_mutex);
      remove_started = true;
    }
    remove_condition.notify_one();
    obs_source_filter_remove(source, filter);
  });

  {
    std::unique_lock<std::mutex> lock(remove_mutex);
    remove_condition.wait(lock, [&] { return remove_started; });
  }
  {
    std::lock_guard<std::mutex> lock(barrier.mutex);
    barrier.release = true;
  }
  barrier.condition.notify_all();

  producer.join();
  remover.join();

  require(barrier.audio_valid,
          "update or enable overlap invalidated audio still held by libobs");
  require(capture.callbacks.load(std::memory_order_relaxed) > 0,
          "libobs did not deliver filtered audio after the lifecycle overlap");
  require(obs_source_filter_count(source) == 1,
          "DPDFNet filter removal did not complete");

  obs_source_remove_audio_capture_callback(source, capture_audio, &capture);
  obs_source_filter_remove(source, barrier_filter);
  require(obs_source_filter_count(source) == 0,
          "barrier filter removal did not complete");
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: dpdfnet-filter-tests <model.onnx>\n";
    return 2;
  }

  bool started = false;
  try {
    const std::filesystem::path model =
        std::filesystem::absolute(argv[1]).lexically_normal();
    require(std::filesystem::is_regular_file(model),
            "model path does not name a regular file");

    require(obs_startup("en-US", nullptr, nullptr), "obs_startup failed");
    started = true;
    reset_audio(SPEAKERS_STEREO);

    const struct obs_source_info test_source_info = make_test_source_info();
    const struct obs_source_info barrier_filter_info =
        make_barrier_filter_info();
    obs_register_source(&test_source_info);
    obs_register_source(&barrier_filter_info);
    obs_register_source(&dpdfnet_filter_info);

    test_empty_custom_model_error();
    test_direct_callbacks(model.string());
    test_obs_lifecycle(model.string());

    obs_shutdown();
    std::cout << "[PASS] filter callback and OBS lifecycle integration\n";
    return 0;
  } catch (const std::exception &ex) {
    if (started)
      obs_shutdown();
    std::cerr << "[FAIL] " << ex.what() << '\n';
    return 1;
  }
}
