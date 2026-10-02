// SPDX-License-Identifier: GPL-2.0-or-later

// Loads an installed copy of the plugin the way OBS does and runs it with
// default settings. The module, its runtime libraries, the bundled models,
// and the locale are found through the install layout, not test paths.

#include <obs.h>
#include <util/platform.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr const char *FILTER_ID = "obs_dpdfnet_filter";
constexpr const char *SOURCE_ID = "dpdfnet_module_test_source";
constexpr uint32_t SAMPLE_RATE = 48000;
constexpr uint32_t PACKET_FRAMES = 480;
constexpr uint64_t PACKET_DURATION_NS = 10000000;

class TestFailure : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

void require(bool condition, const std::string &message) {
  if (!condition)
    throw TestFailure(message);
}

const char *source_name(void *) { return "DPDFNet module test source"; }
char source_token;
void *source_create(obs_data_t *, obs_source_t *) { return &source_token; }
void source_destroy(void *) {}

std::string summary_of(obs_source_t *filter) {
  obs_properties_t *properties = obs_source_properties(filter);
  require(properties != nullptr, "filter returned no properties");
  const char *text = obs_property_description(
      obs_properties_get(properties, "status_summary"));
  std::string summary = text ? text : "";
  obs_properties_destroy(properties);
  return summary;
}

// A missing locale key comes back as the key itself, which starts with
// "DPDFNet." for every key this plugin uses.
void require_translated(obs_properties_t *properties) {
  for (obs_property_t *property = obs_properties_first(properties); property;
       obs_property_next(&property)) {
    const std::string name = obs_property_name(property);
    for (const char *text : {obs_property_description(property),
                             obs_property_long_description(property)}) {
      require(!text || std::string(text).find("DPDFNet.") == std::string::npos,
              "property " + name + " shows an untranslated locale key");
    }
    if (obs_property_get_type(property) == OBS_PROPERTY_LIST) {
      for (size_t i = 0; i < obs_property_list_item_count(property); ++i) {
        const char *item = obs_property_list_item_name(property, i);
        require(item && std::string(item).find("DPDFNet.") == std::string::npos,
                "list " + name + " shows an untranslated locale key");
      }
    }
    if (obs_property_get_type(property) == OBS_PROPERTY_GROUP)
      require_translated(obs_property_group_content(property));
  }
}

void run(const char *module_path, const char *data_path) {
  struct obs_audio_info audio = {};
  audio.samples_per_sec = SAMPLE_RATE;
  audio.speakers = SPEAKERS_STEREO;
  require(obs_reset_audio(&audio), "obs_reset_audio failed");

  obs_module_t *module = nullptr;
  require(obs_open_module(&module, module_path, data_path) == MODULE_SUCCESS,
          "OBS could not open the installed plugin");
  require(obs_init_module(module), "the installed plugin failed to load");

  const char *display_name = obs_source_get_display_name(FILTER_ID);
  require(display_name && std::string(display_name) ==
                              "DPDFNet Noise Suppression",
          "the filter name is not translated");

  struct obs_source_info source_info = {};
  source_info.id = SOURCE_ID;
  source_info.type = OBS_SOURCE_TYPE_INPUT;
  source_info.output_flags = OBS_SOURCE_AUDIO;
  source_info.get_name = source_name;
  source_info.create = source_create;
  source_info.destroy = source_destroy;
  obs_register_source(&source_info);

  obs_source_t *source =
      obs_source_create_private(SOURCE_ID, "module test source", nullptr);
  obs_source_t *filter =
      obs_source_create_private(FILTER_ID, "module test filter", nullptr);
  require(source && filter, "could not create the test source and filter");

  try {
    obs_properties_t *properties = obs_source_properties(filter);
    require(properties != nullptr, "filter returned no properties");
    require_translated(properties);
    obs_properties_destroy(properties);

    const std::string loaded = summary_of(filter);
    require(loaded.rfind("Active\nDPDFNet8", 0) == 0,
            "default settings did not load the bundled DPDFNet8 model: " +
                loaded);

    obs_source_filter_add(source, filter);
    std::vector<float> left(PACKET_FRAMES), right(PACKET_FRAMES);
    for (uint32_t i = 0; i < PACKET_FRAMES; ++i) {
      left[i] = 0.1f * std::sin(0.0576f * static_cast<float>(i));
      right[i] = 0.1f * std::sin(0.0864f * static_cast<float>(i));
    }
    const uint64_t start = os_gettime_ns();
    for (uint64_t packet = 0; packet < 50; ++packet) {
      struct obs_source_audio input = {};
      input.data[0] = reinterpret_cast<const uint8_t *>(left.data());
      input.data[1] = reinterpret_cast<const uint8_t *>(right.data());
      input.frames = PACKET_FRAMES;
      input.speakers = SPEAKERS_STEREO;
      input.format = AUDIO_FORMAT_FLOAT_PLANAR;
      input.samples_per_sec = SAMPLE_RATE;
      input.timestamp = start + packet * PACKET_DURATION_NS;
      obs_source_output_audio(source, &input);
    }
    const std::string processed = summary_of(filter);
    require(processed.find("% processing load") != std::string::npos,
            "the installed plugin did not process audio: " + processed);
  } catch (...) {
    obs_source_filter_remove(source, filter);
    obs_source_release(filter);
    obs_source_release(source);
    throw;
  }
  obs_source_filter_remove(source, filter);
  obs_source_release(filter);
  obs_source_release(source);
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 3) {
    std::cerr << "usage: dpdfnet-module-test <installed-module> "
                 "<installed-data-directory>\n";
    return 2;
  }
  if (!obs_startup("en-US", nullptr, nullptr)) {
    std::cerr << "[FAIL] obs_startup failed\n";
    return 1;
  }
  int result = 0;
  try {
    run(argv[1], argv[2]);
    std::cout << "[PASS] installed plugin loads and processes audio\n";
  } catch (const std::exception &ex) {
    std::cerr << "[FAIL] " << ex.what() << '\n';
    result = 1;
  }
  obs_shutdown();
  return result;
}
