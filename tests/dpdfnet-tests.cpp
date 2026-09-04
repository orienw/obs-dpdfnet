// SPDX-License-Identifier: GPL-2.0-or-later

#include "../src/dpdfnet-processor.hpp"
#include "../src/dpdfnet-realtime-guard.hpp"
#include "../src/dpdfnet-settings.hpp"
#include "../src/ring.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr uint64_t NS_PER_SECOND = 1000000000ULL;

class TestFailure : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

void require(bool condition, const std::string &message) {
  if (!condition)
    throw TestFailure(message);
}

bool nearly_equal(float left, float right, float tolerance = 1e-6f) {
  return std::fabs(left - right) <= tolerance;
}

DpdfnetProcessor make_processor(const std::string &model_path,
                                uint32_t sample_rate = 48000,
                                size_t channels = 1) {
  DpdfnetProcessor processor;
  processor.set_format(sample_rate, channels);
  auto model = prepare_dpdfnet_model(model_path);
  const int model_rate = model.model->sample_rate();
  const int model_hop = model.model->hop_size();
  processor.replace_model(std::move(model));
  if (sample_rate != static_cast<uint32_t>(model_rate)) {
    processor.replace_resamplers(
        prepare_dpdfnet_resamplers(sample_rate, model_rate, model_hop));
  }
  return processor;
}

void test_ring() {
  Ring<int> ring;
  ring.reserve(7);
  std::deque<int> expected;
  for (int round = 0; round < 1000; ++round) {
    const int value = round * 3 + 1;
    ring.push(value);
    expected.push_back(value);
    if (round % 3 == 0) {
      require(ring.front() == expected.front(), "ring front diverged");
      ring.pop(1);
      expected.pop_front();
    }
    require(ring.size() == expected.size(), "ring size diverged");
  }

  std::vector<int> actual(expected.size());
  ring.peek(actual.data(), actual.size());
  require(std::equal(actual.begin(), actual.end(), expected.begin()),
          "ring wraparound order diverged");
  ring.clear();
  require(ring.empty(), "ring clear failed");

  Ring<int> realtime_ring;
  realtime_ring.reserve(2);
  const int values[] = {4, 5};
  require(realtime_ring.try_push(values, 2),
          "ring rejected an in-capacity realtime push");
  require(!realtime_ring.try_push(6),
          "ring grew during an over-capacity realtime push");
  require(realtime_ring.capacity() == 2 && realtime_ring.size() == 2,
          "failed realtime push changed the ring");
}

void test_realtime_budget_guard() {
  constexpr uint64_t hop_budget = 10'000'000;
  DpdfnetRealtimeBudgetGuard guard;

  const auto exact = guard.observe(hop_budget, 1, 480, 48000);
  require(exact.budget_ns == hop_budget && !exact.tripped,
          "exact realtime budget tripped the overload guard");
  require(guard.debt_ns() == 0,
          "exact realtime budget accumulated overload debt");

  for (size_t hop = 1; hop <= 10; ++hop) {
    const auto observation = guard.observe(2 * hop_budget, 1, 480, 48000);
    require(observation.tripped == (hop == 10),
            "2x realtime load tripped at the wrong point");
    require(observation.debt_ns == hop * hop_budget,
            "overload observation reported the wrong accumulated debt");
  }

  guard.reset();
  for (size_t hop = 1; hop <= 100; ++hop) {
    const auto observation =
        guard.observe(hop_budget + 1'000'000, 1, 480, 48000);
    require(observation.tripped == (hop == 100),
            "sustained 10 percent overload tripped at the wrong point");
  }

  guard.reset();
  require(!guard.observe(200'000'000, 1, 480, 48000).tripped,
          "one scheduling spike opened the overload circuit");
  for (size_t hop = 0; hop < 22; ++hop)
    require(!guard.observe(1'000'000, 1, 480, 48000).tripped,
            "healthy processing tripped while repaying overload debt");
  require(guard.debt_ns() == 0 && guard.observed_audio_ns() == 0,
          "healthy processing did not repay overload debt");

  guard.reset();
  require(!guard.observe(19'000'000, 2, 480, 48000).tripped,
          "multi-hop processing did not receive a multi-hop budget");
  require(guard.debt_ns() == 0,
          "in-budget multi-hop processing accumulated overload debt");

  require(!guard.observe(20'000'000, 1, 480, 48000).tripped,
          "initial overload unexpectedly tripped the guard");
  const uint64_t debt_before_zero_hop = guard.debt_ns();
  require(!guard.observe(UINT64_MAX, 0, 480, 48000).tripped &&
              guard.debt_ns() == debt_before_zero_hop,
          "zero-hop callback changed overload debt");

  guard.reset();
  const auto saturated =
      guard.observe(UINT64_MAX, std::numeric_limits<size_t>::max(),
                    std::numeric_limits<int>::max(), 1);
  require(saturated.budget_ns == UINT64_MAX && !saturated.tripped,
          "realtime budget arithmetic did not saturate safely");
}

void test_process_result_fail_open() {
  DpdfnetProcessResult result;
  result.disposition = DpdfnetDisposition::Pending;
  result.data[0] = reinterpret_cast<float *>(uintptr_t{1});
  result.frames = 8192;
  result.timestamp = NS_PER_SECOND;
  result.processed_hops = 16;
  result.resampler_refresh_needed = true;
  result.event = DpdfnetEvent::RealtimeOverloadCircuitOpened;
  result.message[0] = 'x';

  result.fail_open();
  require(result.disposition == DpdfnetDisposition::Passthrough &&
              result.data[0] == nullptr && result.frames == 0 &&
              result.timestamp == 0,
          "fail-open transition retained pending output state");
  require(result.processed_hops == 16 && result.resampler_refresh_needed &&
              result.event == DpdfnetEvent::RealtimeOverloadCircuitOpened &&
              result.message[0] == 'x',
          "fail-open transition discarded diagnostic state");
}

void test_timestamp_floor() {
  DpdfnetTimestampFloor floor;
  floor.observe_input(NS_PER_SECOND);
  require(floor.apply(NS_PER_SECOND, 480, 48000) == NS_PER_SECOND,
          "timestamp floor changed the first packet");
  floor.observe_input(NS_PER_SECOND + 10'000'000);
  require(floor.apply(NS_PER_SECOND + 10'000'000, 480, 48000) ==
              NS_PER_SECOND + 10'000'000,
          "timestamp floor changed a continuous packet");
  require(floor.apply(NS_PER_SECOND + 15'000'000, 480, 48000) ==
              NS_PER_SECOND + 20'000'000,
          "timestamp floor allowed recovery output to move backwards");
  floor.observe_input(500'000'000);
  require(floor.apply(500'000'000, 480, 48000) == 500'000'000,
          "backward input epoch retained the previous output timeline");
  floor.reset();
  floor.observe_input(100'000'000);
  require(floor.apply(100'000'000, 480, 48000) == 100'000'000,
          "timestamp floor reset retained input history");
}

void test_model_selection_migration(const std::string &quality_path,
                                    const std::string &low_cpu_path,
                                    const std::filesystem::path &fixtures) {
  const std::string missing_custom =
      (fixtures / "missing-custom-model.onnx").string();
  require(dpdfnet_classify_model_selection(false, "", false, "", quality_path,
                                           low_cpu_path) ==
              DPDFNET_MODEL_QUALITY,
          "new instance did not select the quality model");
  require(dpdfnet_classify_model_selection(false, "", true, quality_path,
                                           quality_path, low_cpu_path) ==
              DPDFNET_MODEL_QUALITY,
          "legacy quality path migration failed");
  require(dpdfnet_classify_model_selection(false, "", true, low_cpu_path,
                                           quality_path, low_cpu_path) ==
              DPDFNET_MODEL_LOW_CPU,
          "legacy low-CPU path migration failed");
  require(dpdfnet_classify_model_selection(false, "", true, missing_custom,
                                           quality_path, low_cpu_path) ==
              DPDFNET_MODEL_CUSTOM,
          "missing custom model path was not preserved");
  require(dpdfnet_classify_model_selection(
              true, DPDFNET_MODEL_CUSTOM, true, quality_path, quality_path,
              low_cpu_path) == DPDFNET_MODEL_CUSTOM,
          "explicit custom selection was overwritten");
  require(dpdfnet_classify_model_selection(
              true, "future-value", true, missing_custom, quality_path,
              low_cpu_path) == DPDFNET_MODEL_CUSTOM,
          "unknown selection discarded a custom path");
  require(dpdfnet_paths_equivalent(quality_path, quality_path),
          "filesystem-equivalent model path was not recognized");
}

struct ExpectedPacket {
  std::vector<std::vector<float>> channels;
  uint64_t timestamp = 0;
};

void test_variable_packets_and_bypass(const std::string &model_path) {
  DpdfnetProcessor processor = make_processor(model_path, 48000, 2);
  DpdfnetControls controls;
  controls.input_channel = 0;
  controls.bypass = true;
  processor.set_controls(controls);

  const uint32_t sizes[] = {137, 441, 480, 512, 960, 1024};
  std::deque<ExpectedPacket> expected;
  uint64_t timestamp = NS_PER_SECOND;
  uint64_t last_output_timestamp = 0;
  size_t outputs = 0;

  for (size_t packet_index = 0; packet_index < 80; ++packet_index) {
    const uint32_t frames = sizes[packet_index % std::size(sizes)];
    ExpectedPacket item;
    item.timestamp = timestamp;
    item.channels.resize(2);
    for (size_t channel = 0; channel < 2; ++channel) {
      item.channels[channel].resize(frames);
      const float value = 0.001f * static_cast<float>(packet_index + 1) +
                          0.01f * static_cast<float>(channel);
      std::fill(item.channels[channel].begin(), item.channels[channel].end(),
                value);
    }

    DpdfnetAudioPacket packet;
    packet.frames = frames;
    packet.timestamp = timestamp;
    for (size_t channel = 0; channel < 2; ++channel)
      packet.data[channel] = item.channels[channel].data();
    expected.push_back(item);

    const auto result = processor.process(packet);
    require(result.disposition != DpdfnetDisposition::Passthrough,
            "healthy bypass returned immediate raw audio");
    if (result.disposition == DpdfnetDisposition::Processed) {
      require(!expected.empty(), "processor emitted an unqueued packet");
      const auto &front = expected.front();
      require(result.frames == front.channels[0].size(),
              "processor changed packet frame count");
      require(result.timestamp == front.timestamp,
              "bypass changed the input samples' timestamp");
      require(result.timestamp >= last_output_timestamp,
              "processor output timestamps went backwards");
      for (size_t channel = 0; channel < 2; ++channel) {
        for (size_t frame = 0; frame < result.frames; ++frame) {
          require(nearly_equal(result.data[channel][frame],
                               front.channels[channel][frame]),
                  "latency-aligned bypass changed a sample");
        }
      }
      last_output_timestamp = result.timestamp;
      expected.pop_front();
      ++outputs;
    }
    timestamp += static_cast<uint64_t>(static_cast<double>(frames) / 48000.0 *
                                       NS_PER_SECOND);
  }

  require(outputs > 60, "processor failed to drain variable packet stream");
  require(expected.size() <=
              static_cast<size_t>(processor.model()->output_delay_hops()) + 3,
          "processor accumulated an unbounded backlog");
}

std::vector<float> process_signal(DpdfnetProcessor &processor,
                                  const std::vector<float> &signal,
                                  uint64_t start_timestamp = NS_PER_SECOND) {
  const uint32_t sizes[] = {137, 441, 480, 512, 960, 1024};
  std::vector<float> output;
  output.reserve(signal.size());
  size_t offset = 0;
  size_t packet_index = 0;
  uint64_t timestamp = start_timestamp;
  std::vector<float> padding(1024, 0.0f);

  for (size_t guard = 0; output.size() < signal.size() && guard < 10000;
       ++guard) {
    const uint32_t requested = sizes[packet_index % std::size(sizes)];
    const uint32_t frames = offset < signal.size()
                                ? static_cast<uint32_t>(std::min<size_t>(
                                      requested, signal.size() - offset))
                                : requested;
    const float *data =
        offset < signal.size() ? signal.data() + offset : padding.data();
    DpdfnetAudioPacket packet;
    packet.data[0] = data;
    packet.frames = frames;
    packet.timestamp = timestamp;
    const auto result = processor.process(packet);
    require(result.disposition != DpdfnetDisposition::Passthrough,
            "healthy processor unexpectedly passed through");
    if (result.disposition == DpdfnetDisposition::Processed)
      output.insert(output.end(), result.data[0],
                    result.data[0] + result.frames);

    if (offset < signal.size())
      offset += frames;
    timestamp += static_cast<uint64_t>(static_cast<double>(frames) / 48000.0 *
                                       NS_PER_SECOND);
    ++packet_index;
  }
  require(output.size() >= signal.size(), "processor failed to drain signal");
  output.resize(signal.size());
  return output;
}

void test_bypass_transition(const std::string &model_path) {
  DpdfnetProcessor processor = make_processor(model_path);
  DpdfnetControls controls;
  processor.set_controls(controls);
  std::vector<float> signal(48000);
  for (size_t i = 0; i < signal.size(); ++i)
    signal[i] = static_cast<float>(0.05 * std::sin(i * 0.031));

  const uint32_t frames = 480;
  uint64_t timestamp = NS_PER_SECOND;
  uint64_t last_timestamp = 0;
  for (size_t packet_index = 0; packet_index < 40; ++packet_index) {
    if (packet_index == 12) {
      controls.bypass = true;
      processor.set_controls(controls);
    } else if (packet_index == 28) {
      controls.bypass = false;
      processor.set_controls(controls);
    }
    DpdfnetAudioPacket packet;
    packet.data[0] = signal.data() + packet_index * frames;
    packet.frames = frames;
    packet.timestamp = timestamp;
    const auto result = processor.process(packet);
    require(result.disposition != DpdfnetDisposition::Passthrough,
            "bypass transition escaped the aligned pipeline");
    if (result.disposition == DpdfnetDisposition::Processed) {
      require(result.timestamp >= last_timestamp,
              "bypass transition reordered timestamps");
      last_timestamp = result.timestamp;
    }
    timestamp += 10'000'000;
  }
}

void compare_results(const DpdfnetProcessResult &left,
                     const DpdfnetProcessResult &right) {
  require(left.disposition == right.disposition,
          "reset stream disposition differs from fresh stream");
  if (left.disposition != DpdfnetDisposition::Processed)
    return;
  require(left.frames == right.frames && left.timestamp == right.timestamp,
          "reset stream packet metadata differs from fresh stream");
  for (uint32_t frame = 0; frame < left.frames; ++frame) {
    require(nearly_equal(left.data[0][frame], right.data[0][frame], 2e-6f),
            "reset stream audio differs from fresh stream");
  }
}

void test_model_delay_alignment(const std::filesystem::path &fixtures) {
  for (uint32_t rate : {48000, 44100, 96000}) {
    for (int mode = 0; mode < 4; ++mode) {
      auto reference =
          make_processor((fixtures / "valid_identity.onnx").string(), rate);
      auto delayed = make_processor(
          (fixtures / "valid_delayed_identity.onnx").string(), rate);
      DpdfnetControls controls;
      controls.attenuation_limit_db = mode == 0 ? 0.0 : 24.0;
      controls.wet_mix = mode == 2 ? 0.5 : 1.0;
      controls.bypass = mode == 3;
      reference.set_controls(controls);
      delayed.set_controls(controls);

      const uint32_t frames = rate / 100;
      std::vector<float> input(frames);
      for (uint64_t epoch = 0; epoch < 2; ++epoch) {
        if (epoch && rate != 48000) {
          reference.replace_resamplers(
              prepare_dpdfnet_resamplers(rate, 48000, 480));
          delayed.replace_resamplers(
              prepare_dpdfnet_resamplers(rate, 48000, 480));
        }
        reference.reset_state();
        delayed.reset_state();
        std::vector<float> expected, actual;
        std::vector<uint64_t> expected_timestamps, actual_timestamps;
        for (uint64_t index = 0; index < 40; ++index) {
          for (size_t i = 0; i < frames; ++i)
            input[i] = static_cast<float>(
                0.1 * std::sin((index * frames + i) * 0.13 + epoch));
          DpdfnetAudioPacket packet;
          packet.data[0] = input.data();
          packet.frames = frames;
          packet.timestamp = (1 + epoch) * NS_PER_SECOND + index * 10'000'000;
          const auto collect = [&](DpdfnetProcessor &processor,
                                   std::vector<float> &samples,
                                   std::vector<uint64_t> &timestamps) {
            const auto result = processor.process(packet);
            require(result.disposition != DpdfnetDisposition::Passthrough,
                    "delay alignment failed open");
            if (result.disposition == DpdfnetDisposition::Processed) {
              samples.insert(samples.end(), result.data[0],
                             result.data[0] + result.frames);
              timestamps.push_back(result.timestamp);
            }
          };
          collect(reference, expected, expected_timestamps);
          collect(delayed, actual, actual_timestamps);
        }
        require(actual.size() >= 30 * frames,
                "delayed model did not drain startup output");
        require(actual.size() <= expected.size(),
                "delayed model returned excess audio");
        require(std::equal(actual_timestamps.begin(), actual_timestamps.end(),
                           expected_timestamps.begin()),
                "model delay changed the output timeline");
        for (size_t i = 0; i < actual.size(); ++i)
          require(nearly_equal(actual[i], expected[i], 2e-6f),
                  "model delay misaligned audio: rate=" + std::to_string(rate) +
                      " mode=" + std::to_string(mode) + " epoch=" +
                      std::to_string(epoch) + " sample=" + std::to_string(i));
      }
    }
  }
}

void test_bundled_model_delay(const std::string &path) {
  DpdfnetModel model(path);
  require(model.output_delay_hops() == 4,
          "bundled model delay was not recognized");
  std::mt19937 rng(71921);
  std::uniform_real_distribution<float> sample(-0.3f, 0.3f);
  std::vector<std::pair<float, float>> history;
  for (int hop = 0; hop < 24; ++hop) {
    for (size_t i = 0; i < model.spectrum_size(); ++i)
      model.input_spectrum()[i] = sample(rng);
    history.emplace_back(model.input_spectrum()[800],
                         model.input_spectrum()[801]);
    model.enhance();
    if (hop < 8)
      continue;
    const double re = model.output_spectrum()[800];
    const double im = model.output_spectrum()[801];
    const double magnitude = std::hypot(re, im);
    require(magnitude > 1e-30, "bundled delay probe produced no signal");
    const auto [expected_re, expected_im] = history[hop - 4];
    const double expected_magnitude = std::hypot(expected_re, expected_im);
    require(std::hypot(re / magnitude - expected_re / expected_magnitude,
                       im / magnitude - expected_im / expected_magnitude) < 1e-5,
            "bundled model signal delay differs from its declared contract");
  }
}

void test_channel_and_timestamp_resets(const std::string &model_path) {
  DpdfnetProcessor transitioned = make_processor(model_path, 48000, 2);
  DpdfnetControls left;
  left.input_channel = 0;
  transitioned.set_controls(left);

  std::vector<float> old_left(960, 0.08f);
  std::vector<float> old_right(960, -0.08f);
  DpdfnetAudioPacket old_packet;
  old_packet.data[0] = old_left.data();
  old_packet.data[1] = old_right.data();
  old_packet.frames = 960;
  old_packet.timestamp = 2 * NS_PER_SECOND;
  transitioned.process(old_packet);

  DpdfnetControls right = left;
  right.input_channel = 1;
  transitioned.set_controls(right);
  DpdfnetProcessor fresh = make_processor(model_path, 48000, 2);
  fresh.set_controls(right);

  std::vector<float> new_left(960, 0.02f);
  std::vector<float> new_right(960, -0.03f);
  for (size_t packet_index = 0; packet_index < 4; ++packet_index) {
    DpdfnetAudioPacket packet;
    packet.data[0] = new_left.data();
    packet.data[1] = new_right.data();
    packet.frames = 960;
    packet.timestamp = 3 * NS_PER_SECOND + packet_index * 20'000'000;
    compare_results(transitioned.process(packet), fresh.process(packet));
  }

  std::vector<float> history(960, 0.04f);
  DpdfnetAudioPacket packet;
  packet.data[0] = history.data();
  packet.data[1] = history.data();
  packet.frames = 960;
  packet.timestamp = 4 * NS_PER_SECOND;
  transitioned.process(packet);

  DpdfnetProcessor backward_fresh = make_processor(model_path, 48000, 2);
  backward_fresh.set_controls(right);
  for (size_t index = 0; index < 4; ++index) {
    packet.timestamp = NS_PER_SECOND + index * 20'000'000;
    compare_results(transitioned.process(packet),
                    backward_fresh.process(packet));
  }

  DpdfnetProcessor forward_jump = make_processor(model_path, 48000, 2);
  forward_jump.set_controls(right);
  packet.timestamp = NS_PER_SECOND;
  forward_jump.process(packet);
  DpdfnetProcessor forward_fresh = make_processor(model_path, 48000, 2);
  forward_fresh.set_controls(right);
  constexpr uint64_t first_after_gap = NS_PER_SECOND + 20'000'000 + 51'000'000;
  for (size_t index = 0; index < 4; ++index) {
    packet.timestamp = first_after_gap + index * 20'000'000;
    compare_results(forward_jump.process(packet),
                    forward_fresh.process(packet));
  }

  DpdfnetProcessor tolerated = make_processor(model_path, 48000, 2);
  tolerated.set_controls(right);
  for (uint64_t index = 0; index < 8; ++index) {
    packet.timestamp = NS_PER_SECOND + index * 20'000'000;
    tolerated.process(packet);
  }
  packet.timestamp = NS_PER_SECOND + 8 * 20'000'000 + 49'000'000;
  require(tolerated.process(packet).disposition ==
              DpdfnetDisposition::Processed,
          "timestamp deviation below the tolerance reset the stream");
}

void test_empty_resampler_replacement_is_noop(const std::string &model_path) {
  DpdfnetProcessor control = make_processor(model_path);
  DpdfnetProcessor unchanged = make_processor(model_path);
  std::vector<float> data(960, 0.04f);
  DpdfnetAudioPacket packet;
  packet.data[0] = data.data();
  packet.frames = 960;
  packet.timestamp = NS_PER_SECOND;
  compare_results(control.process(packet), unchanged.process(packet));
  unchanged.replace_resamplers({});
  packet.timestamp += 20'000'000;
  compare_results(control.process(packet), unchanged.process(packet));
}

void test_extreme_contract_capacity_plan() {
  const auto low_rate = plan_dpdfnet_realtime_capacity(8000, 8192);
  const size_t native_window = 8192 * (384000 / 8000);
  require(low_rate.output_samples >=
              native_window + DPDFNET_MAX_REALTIME_PACKET_FRAMES,
          "8 kHz / 8192 FFT plan cannot hold the native-rate output window");
  require(low_rate.dry_samples > low_rate.output_samples,
          "extreme contract plan cannot retain latency-aligned dry audio");
  require(low_rate.packet_infos >= native_window,
          "extreme contract plan cannot queue small native packets");

  const auto high_rate = plan_dpdfnet_realtime_capacity(384000, 8192);
  const size_t model_input =
      DPDFNET_MAX_REALTIME_PACKET_FRAMES * (384000 / 8000);
  require(high_rate.input_samples >= model_input + 8192,
          "384 kHz model plan cannot hold an upsampled 8 kHz packet");
}

void test_extreme_contract_stream(const std::filesystem::path &fixtures) {
  DpdfnetProcessor processor = make_processor(
      (fixtures / "valid_extreme_capacity.onnx").string(), 48000);
  DpdfnetControls controls;
  controls.bypass = true;
  processor.set_controls(controls);

  std::vector<float> data(960, 0.025f);
  uint64_t timestamp = NS_PER_SECOND;
  size_t processed = 0;
  for (size_t packet_index = 0; packet_index < 160; ++packet_index) {
    DpdfnetAudioPacket packet;
    packet.data[0] = data.data();
    packet.frames = static_cast<uint32_t>(data.size());
    packet.timestamp = timestamp;
    const auto result = processor.process(packet);
    require(result.disposition != DpdfnetDisposition::Passthrough,
            "extreme contract stream reset at its former fixed capacity");
    if (result.disposition == DpdfnetDisposition::Processed) {
      ++processed;
      for (uint32_t frame = 0; frame < result.frames; ++frame) {
        require(std::isfinite(result.data[0][frame]),
                "extreme contract stream produced non-finite audio");
      }
    }
    timestamp += 20'000'000;
  }
  require(processed > 60,
          "extreme contract stream never reached processed output");
}

void test_capacity_failure_diagnostics(const std::string &model_path) {
  std::vector<float> oversized(DPDFNET_MAX_REALTIME_PACKET_FRAMES + 1, 0.02f);
  DpdfnetAudioPacket packet;
  packet.data[0] = oversized.data();
  packet.frames = static_cast<uint32_t>(oversized.size());
  packet.timestamp = NS_PER_SECOND;

  DpdfnetProcessor native = make_processor(model_path);
  auto result = native.process(packet);
  require(result.disposition == DpdfnetDisposition::Passthrough &&
              result.event == DpdfnetEvent::OversizedPacket,
          "first oversized packet was not reported while failing open");
  auto state = native.state();
  require(state.oversized_packets == 1 && state.capacity_failures == 0 &&
              state.capacity_recovery_pending && !state.processing_disabled &&
              !state.consecutive_failures,
          "oversized packet changed the wrong processor state");

  packet.timestamp += 200'000'000;
  result = native.process(packet);
  require(result.event == DpdfnetEvent::None &&
              native.state().oversized_packets == 2,
          "repeated oversized packet was not counted or was reported twice");

  std::vector<float> supported(960, 0.02f);
  packet.data[0] = supported.data();
  packet.frames = static_cast<uint32_t>(supported.size());
  packet.timestamp += 200'000'000;
  result = native.process(packet);
  require(result.disposition != DpdfnetDisposition::Passthrough &&
              !native.state().capacity_recovery_pending,
          "supported packet did not recover the native pipeline");

  native.reset_state();
  state = native.state();
  require(state.oversized_packets == 0 && state.capacity_failures == 0,
          "Reset did not clear capacity diagnostics");
  packet.data[0] = oversized.data();
  packet.frames = static_cast<uint32_t>(oversized.size());
  packet.timestamp += 200'000'000;
  require(native.process(packet).event == DpdfnetEvent::OversizedPacket,
          "Reset did not rearm oversized-packet reporting");

  constexpr uint32_t resampled_rate = 44100;
  DpdfnetProcessor resampled = make_processor(model_path, resampled_rate);
  std::vector<float> resampled_audio(882, 0.02f);
  packet.data[0] = resampled_audio.data();
  packet.frames = static_cast<uint32_t>(resampled_audio.size());
  packet.timestamp = 2 * NS_PER_SECOND;
  (void)resampled.process(packet);

  packet.data[0] = oversized.data();
  packet.frames = static_cast<uint32_t>(oversized.size());
  packet.timestamp += 200'000'000;
  result = resampled.process(packet);
  require(result.event == DpdfnetEvent::OversizedPacket &&
              result.resampler_refresh_needed &&
              resampled.state().resampler_refresh_required,
          "resampled oversized packet did not invalidate stale resamplers");

  packet.timestamp += 200'000'000;
  result = resampled.process(packet);
  require(!result.resampler_refresh_needed &&
              resampled.state().oversized_packets == 2,
          "continuous oversized packets repeatedly requested resamplers");

  state = resampled.state();
  resampled.replace_resamplers(prepare_dpdfnet_resamplers(
      resampled_rate, state.model_rate, state.hop_size));
  packet.timestamp += 200'000'000;
  result = resampled.process(packet);
  require(!result.resampler_refresh_needed && resampled.state().resampling,
          "unused fresh resamplers were invalidated by another oversized "
          "packet");

  packet.data[0] = resampled_audio.data();
  packet.frames = static_cast<uint32_t>(resampled_audio.size());
  packet.timestamp += 200'000'000;
  result = resampled.process(packet);
  require(result.disposition != DpdfnetDisposition::Passthrough &&
              !resampled.state().capacity_recovery_pending,
          "supported packet did not recover the resampled pipeline");

  packet.data[0] = oversized.data();
  packet.frames = static_cast<uint32_t>(oversized.size());
  packet.timestamp += 200'000'000;
  result = resampled.process(packet);
  require(result.resampler_refresh_needed &&
              resampled.state().resampler_refresh_required,
          "later oversized packet did not invalidate used resamplers");
}

void test_capacity_invariant_diagnostic(const std::string &model_path) {
  DpdfnetProcessor processor;
  processor.set_format(48000, 1);
  auto model = prepare_dpdfnet_model(model_path);
  model.realtime = DpdfnetRealtimeStorage{};
  processor.replace_model(std::move(model));

  std::vector<float> data(480, 0.02f);
  DpdfnetAudioPacket packet;
  packet.data[0] = data.data();
  packet.frames = static_cast<uint32_t>(data.size());
  packet.timestamp = NS_PER_SECOND;
  auto result = processor.process(packet);
  require(result.disposition == DpdfnetDisposition::Passthrough &&
              result.event == DpdfnetEvent::CapacityInvariantFailure,
          "unexpected buffer exhaustion was not reported while failing open");
  auto state = processor.state();
  require(state.capacity_failures == 1 && state.oversized_packets == 0 &&
              state.capacity_recovery_pending && !state.processing_disabled &&
              !state.consecutive_failures,
          "capacity invariant failure changed the wrong processor state");

  packet.timestamp += 10'000'000;
  result = processor.process(packet);
  require(result.event == DpdfnetEvent::None &&
              processor.state().capacity_failures == 2,
          "repeated capacity invariant failure was not counted or was reported "
          "twice");
}

void test_realtime_overload_disable(const std::string &model_path) {
  DpdfnetProcessor processor = make_processor(model_path);
  require(processor.disable_for_realtime_overload(
              "processing used 20000 us for 10000 us of model audio"),
          "realtime overload did not open the circuit");
  auto snapshot = processor.snapshot();
  require(snapshot.processing_disabled &&
              snapshot.disable_reason == DpdfnetDisableReason::RealtimeOverload,
          "realtime overload circuit did not retain its distinct reason");
  require(snapshot.consecutive_failures == 0 &&
              snapshot.last_error.find("20000 us") != std::string::npos,
          "realtime overload was misreported as a processing failure");

  std::vector<float> data(960, 0.02f);
  DpdfnetAudioPacket packet;
  packet.data[0] = data.data();
  packet.frames = static_cast<uint32_t>(data.size());
  packet.timestamp = NS_PER_SECOND;
  require(processor.process(packet).disposition ==
              DpdfnetDisposition::Passthrough,
          "open realtime circuit retried processing");

  processor.reset_state();
  snapshot = processor.snapshot();
  require(!snapshot.processing_disabled &&
              snapshot.disable_reason == DpdfnetDisableReason::None,
          "Reset did not close the realtime overload circuit");
  require(processor.process(packet).disposition !=
              DpdfnetDisposition::Passthrough,
          "Reset did not resume processing after realtime overload");
}

void test_output_storage_survives_format_update(const std::string &model_path) {
  DpdfnetProcessor processor = make_processor(model_path);
  DpdfnetControls controls;
  controls.bypass = true;
  processor.set_controls(controls);

  std::vector<float> data(960, 0.0375f);
  float *returned = nullptr;
  uint32_t returned_frames = 0;
  uint64_t timestamp = NS_PER_SECOND;
  for (size_t attempt = 0; attempt < 4 && !returned; ++attempt) {
    DpdfnetAudioPacket packet;
    packet.data[0] = data.data();
    packet.frames = static_cast<uint32_t>(data.size());
    packet.timestamp = timestamp;
    const auto result = processor.process(packet);
    if (result.disposition == DpdfnetDisposition::Processed) {
      returned = result.data[0];
      returned_frames = result.frames;
    }
    timestamp += 20'000'000;
  }
  require(returned && returned_frames,
          "pointer-lifetime test did not receive processed audio");
  std::vector<float> expected(returned, returned + returned_frames);

  processor.set_format(96000, DPDFNET_MAX_AUDIO_PLANES);
  require(std::equal(expected.begin(), expected.end(), returned),
          "format update invalidated audio returned to OBS");
}

void test_model_activation_probe(const std::filesystem::path &fixtures) {
  for (const char *name :
       {"nonfinite_spectrum_output.onnx", "nonfinite_state_output.onnx"}) {
    bool rejected = false;
    try {
      prepare_dpdfnet_model((fixtures / name).string());
    } catch (const std::exception &) {
      rejected = true;
    }
    require(rejected, std::string("activation probe accepted ") + name);
  }
  const auto runtime_failure = prepare_dpdfnet_model(
      (fixtures / "runtime_nonfinite_spectrum_output.onnx").string());
  require(runtime_failure.model != nullptr,
          "activation probe rejected the runtime-only failure fixture");
}

void test_resampled_stream(const std::string &model_path,
                           uint32_t sample_rate) {
  DpdfnetProcessor processor = make_processor(model_path, sample_rate);
  DpdfnetControls controls;
  controls.bypass = true;
  processor.set_controls(controls);
  const uint32_t packet_frames = sample_rate / 100;
  std::vector<float> packet_data(packet_frames, 0.025f);
  uint64_t timestamp = NS_PER_SECOND;
  size_t processed = 0;
  for (size_t packet_index = 0; packet_index < 80; ++packet_index) {
    DpdfnetAudioPacket packet;
    packet.data[0] = packet_data.data();
    packet.frames = packet_frames;
    packet.timestamp = timestamp;
    const auto result = processor.process(packet);
    require(result.disposition != DpdfnetDisposition::Passthrough,
            "resampled path passed through despite active resamplers");
    if (result.disposition == DpdfnetDisposition::Processed) {
      ++processed;
      for (uint32_t frame = 0; frame < result.frames; ++frame)
        require(std::isfinite(result.data[0][frame]),
                "resampled path produced non-finite audio");
    }
    timestamp += 10'000'000;
  }
  require(processed > 60, "resampled path failed to reach steady state");

  const auto snapshot = processor.snapshot();
  processor.replace_resamplers(prepare_dpdfnet_resamplers(
      snapshot.sample_rate, snapshot.model_rate, snapshot.hop_size));
  processor.reset_state();
  std::fill(packet_data.begin(), packet_data.end(), 0.0f);
  for (size_t i = 0; i < 8; ++i) {
    DpdfnetAudioPacket packet;
    packet.data[0] = packet_data.data();
    packet.frames = packet_frames;
    packet.timestamp = 3 * NS_PER_SECOND + i * 10'000'000;
    const auto result = processor.process(packet);
    if (result.disposition == DpdfnetDisposition::Processed) {
      for (uint32_t frame = 0; frame < result.frames; ++frame)
        require(std::fabs(result.data[0][frame]) < 1e-6f,
                "fresh resampler leaked pre-reset content");
    }
  }
}

void test_resampled_timestamp_refresh(const std::string &model_path,
                                      uint32_t sample_rate) {
  DpdfnetProcessor transitioned = make_processor(model_path, sample_rate);
  DpdfnetControls controls;
  controls.bypass = true;
  transitioned.set_controls(controls);

  const uint32_t frames = sample_rate / 50;
  const uint64_t packet_ns = static_cast<uint64_t>(static_cast<double>(frames) /
                                                   sample_rate * NS_PER_SECOND);
  std::vector<float> old_data(frames, 0.08f);
  std::vector<float> new_data(frames, -0.03f);
  DpdfnetAudioPacket packet;
  packet.data[0] = old_data.data();
  packet.frames = frames;
  packet.timestamp = NS_PER_SECOND;
  transitioned.process(packet);

  packet.data[0] = new_data.data();
  packet.timestamp = NS_PER_SECOND + packet_ns + 51'000'000;
  const auto discontinuity = transitioned.process(packet);
  require(discontinuity.event == DpdfnetEvent::ResamplerRefreshNeeded,
          "resampled timestamp jump did not request fresh resamplers");
  require(transitioned.state().resampler_refresh_required,
          "resampled timestamp jump left stale resamplers active");

  const auto state = transitioned.state();
  transitioned.replace_resamplers(prepare_dpdfnet_resamplers(
      sample_rate, state.model_rate, state.hop_size));
  require(!transitioned.state().resampler_refresh_required,
          "fresh resamplers did not clear the refresh request");

  DpdfnetProcessor fresh = make_processor(model_path, sample_rate);
  fresh.set_controls(controls);
  uint64_t timestamp = packet.timestamp + packet_ns;
  for (size_t index = 0; index < 8; ++index) {
    packet.timestamp = timestamp;
    compare_results(transitioned.process(packet), fresh.process(packet));
    timestamp += packet_ns;
  }
}

void test_format_transition_invalidates_resamplers(
    const std::string &model_path) {
  DpdfnetProcessor processor = make_processor(model_path, 44100);
  require(processor.state().resampling,
          "format transition test did not start resampled");
  processor.set_format(48000, 1);
  require(processor.state().resampler_refresh_required,
          "native format transition kept old resampler state valid");
  processor.release_invalid_resamplers();
  require(!processor.state().resampler_refresh_required &&
              !processor.state().resampling,
          "native format transition did not clear old resamplers");
  processor.set_format(96000, 1);
  require(processor.state().resampler_refresh_required,
          "non-native format transition did not request fresh resamplers");
  const auto state = processor.state();
  processor.replace_resamplers(prepare_dpdfnet_resamplers(
      state.sample_rate, state.model_rate, state.hop_size));
  require(processor.state().resampling,
          "non-native format transition did not activate fresh resamplers");
}

void test_resampled_model_replacement(const std::string &quality_model,
                                      const std::string &low_cpu_model) {
  constexpr uint32_t sample_rate = 44100;
  DpdfnetProcessor transitioned = make_processor(quality_model, sample_rate);
  std::vector<float> history(882, 0.04f);
  DpdfnetAudioPacket packet;
  packet.data[0] = history.data();
  packet.frames = static_cast<uint32_t>(history.size());
  packet.timestamp = NS_PER_SECOND;
  transitioned.process(packet);

  auto replacement = prepare_dpdfnet_model(low_cpu_model);
  const int model_rate = replacement.model->sample_rate();
  const int model_hop = replacement.model->hop_size();
  transitioned.replace_model(std::move(replacement));
  transitioned.replace_resamplers(
      prepare_dpdfnet_resamplers(sample_rate, model_rate, model_hop));
  DpdfnetProcessor fresh = make_processor(low_cpu_model, sample_rate);

  std::vector<float> post(441, -0.025f);
  for (size_t index = 0; index < 8; ++index) {
    packet.data[0] = post.data();
    packet.frames = static_cast<uint32_t>(post.size());
    packet.timestamp = 2 * NS_PER_SECOND + index * 10'000'000;
    compare_results(transitioned.process(packet), fresh.process(packet));
  }
}

std::vector<float> make_signal(size_t frames, int kind) {
  std::vector<float> signal(frames, 0.0f);
  std::mt19937 rng(1234 + kind);
  std::normal_distribution<float> noise(0.0f, 0.01f);
  for (size_t i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i) / 48000.0;
    switch (kind) {
    case 0:
      break;
    case 1:
      if (i == 1200)
        signal[i] = 0.5f;
      break;
    case 2:
      signal[i] = noise(rng);
      break;
    case 3:
      signal[i] = static_cast<float>(0.03 * std::sin(2.0 * kPi * 60.0 * t));
      break;
    case 4:
      signal[i] = static_cast<float>(0.04 * std::sin(2.0 * kPi * 140.0 * t) +
                                     0.02 * std::sin(2.0 * kPi * 280.0 * t));
      break;
    default:
      if (i % 4000 == 0)
        signal[i] = 0.35f;
      signal[i] += noise(rng);
      break;
    }
  }
  return signal;
}

void test_signal_integrity(const std::string &model_path) {
  DpdfnetProcessor processor = make_processor(model_path);
  DpdfnetControls controls;
  controls.attenuation_limit_db = 24.0;
  controls.wet_mix = 1.0;
  controls.output_gain = 1.0f;
  processor.set_controls(controls);

  for (int kind = 0; kind < 6; ++kind) {
    const auto signal = make_signal(48000, kind);
    processor.reset_state();
    const auto first = process_signal(processor, signal);
    processor.reset_state();
    const auto second = process_signal(processor, signal);
    require(first.size() == signal.size(), "signal frame count changed");
    require(first == second, "reset did not produce deterministic output");

    double sum = 0.0;
    float peak = 0.0f;
    for (float sample : first) {
      require(std::isfinite(sample), "signal test produced non-finite output");
      peak = std::max(peak, std::fabs(sample));
      sum += sample;
    }
    require(peak <= 4.0f, "signal test produced a catastrophic peak");
    require(std::fabs(sum / static_cast<double>(first.size())) < 0.1,
            "signal test produced excessive DC");
    if (kind == 0)
      require(peak < 1e-6f, "silence test produced audible output");
  }
}

void test_circuit_breaker(const std::filesystem::path &fixture_directory) {
  const auto bad_model =
      (fixture_directory / "runtime_nonfinite_spectrum_output.onnx").string();
  DpdfnetProcessor processor = make_processor(bad_model);
  std::vector<float> data(960, 0.1f);
  for (unsigned attempt = 1; attempt <= 3; ++attempt) {
    DpdfnetAudioPacket packet;
    packet.data[0] = data.data();
    packet.frames = 960;
    packet.timestamp = NS_PER_SECOND + attempt * 20'000'000;
    const auto result = processor.process(packet);
    require(result.disposition == DpdfnetDisposition::Passthrough,
            "failed model did not pass audio through");
    if (attempt == 1)
      require(result.event == DpdfnetEvent::ProcessingFailure,
              "first processing failure was not reported");
    if (attempt == 3)
      require(result.event == DpdfnetEvent::CircuitOpened,
              "third processing failure did not open the circuit");
  }
  require(processor.snapshot().processing_disabled,
          "open circuit was not retained");

  DpdfnetAudioPacket packet;
  packet.data[0] = data.data();
  packet.frames = 960;
  packet.timestamp = 2 * NS_PER_SECOND;
  require(processor.process(packet).event == DpdfnetEvent::None,
          "open circuit retried inference");
  processor.reset_state();
  require(!processor.snapshot().processing_disabled,
          "Reset did not close the circuit");
  require(processor.process(packet).event == DpdfnetEvent::ProcessingFailure,
          "Reset did not retry inference");
}

void test_resampled_failure_refresh(
    const std::filesystem::path &fixture_directory) {
  constexpr uint32_t sample_rate = 44100;
  const auto bad_model =
      (fixture_directory / "runtime_nonfinite_spectrum_output.onnx").string();
  DpdfnetProcessor processor = make_processor(bad_model, sample_rate);
  std::vector<float> data(sample_rate / 50, 0.1f);
  for (unsigned attempt = 1; attempt <= 3; ++attempt) {
    DpdfnetAudioPacket packet;
    packet.data[0] = data.data();
    packet.frames = static_cast<uint32_t>(data.size());
    packet.timestamp = NS_PER_SECOND + attempt * 20'000'000;
    const auto result = processor.process(packet);
    require(result.disposition == DpdfnetDisposition::Passthrough,
            "resampled model failure did not fail open");
    require(processor.state().resampler_refresh_required,
            "resampled model failure left stale resamplers active");
    if (attempt < 3) {
      require(result.resampler_refresh_needed,
              "retryable resampled failure did not request fresh resamplers");
      const auto state = processor.state();
      processor.replace_resamplers(prepare_dpdfnet_resamplers(
          sample_rate, state.model_rate, state.hop_size));
    } else {
      require(result.event == DpdfnetEvent::CircuitOpened,
              "third resampled failure did not open the circuit");
      require(!result.resampler_refresh_needed,
              "open circuit requested an unnecessary resampler rebuild");
    }
  }
}

template <typename Function>
bool run_test(const char *name, Function function) {
  try {
    function();
    std::cout << "PASS " << name << "\n";
    return true;
  } catch (const std::exception &ex) {
    std::cerr << "FAIL " << name << ": " << ex.what() << "\n";
    return false;
  }
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 4) {
    std::cerr << "usage: obs-dpdfnet-tests <dpdfnet8.onnx> <dpdfnet2.onnx> "
                 "<fixture-directory>\n";
    return 2;
  }

  const std::string quality_model = argv[1];
  const std::string low_cpu_model = argv[2];
  const std::filesystem::path fixtures = argv[3];
  bool passed = true;
  passed = run_test("ring", test_ring) && passed;
  passed =
      run_test("realtime budget guard", test_realtime_budget_guard) && passed;
  passed =
      run_test("process result fail open", test_process_result_fail_open) &&
      passed;
  passed = run_test("timestamp floor", test_timestamp_floor) && passed;
  passed = run_test("model selection migration",
                    [&] {
                      test_model_selection_migration(quality_model,
                                                     low_cpu_model, fixtures);
                    }) &&
           passed;
  passed = run_test("model activation probe",
                    [&] { test_model_activation_probe(fixtures); }) &&
           passed;
  passed = run_test("empty resampler replacement is a no-op",
                    [&] {
                      test_empty_resampler_replacement_is_noop(low_cpu_model);
                    }) &&
           passed;
  passed = run_test("extreme model contract realtime capacity",
                    test_extreme_contract_capacity_plan) &&
           passed;
  passed = run_test("extreme model contract stream",
                    [&] { test_extreme_contract_stream(fixtures); }) &&
           passed;
  passed =
      run_test("capacity failure diagnostics",
               [&] { test_capacity_failure_diagnostics(low_cpu_model); }) &&
      passed;
  passed =
      run_test("capacity invariant diagnostic",
               [&] { test_capacity_invariant_diagnostic(low_cpu_model); }) &&
      passed;
  passed = run_test("realtime overload disable",
                    [&] { test_realtime_overload_disable(low_cpu_model); }) &&
           passed;
  passed = run_test("returned audio survives format update",
                    [&] {
                      test_output_storage_survives_format_update(low_cpu_model);
                    }) &&
           passed;
  passed = run_test("variable packets and aligned bypass",
                    [&] { test_variable_packets_and_bypass(quality_model); }) &&
           passed;
  passed = run_test("bypass transitions",
                    [&] { test_bypass_transition(quality_model); }) &&
           passed;
  passed = run_test("delayed model lane and timestamp alignment",
                    [&] { test_model_delay_alignment(fixtures); }) && passed;
  passed = run_test("DPDFNet8 output delay",
                    [&] { test_bundled_model_delay(quality_model); }) && passed;
  passed = run_test("DPDFNet2 output delay",
                    [&] { test_bundled_model_delay(low_cpu_model); }) && passed;
  passed =
      run_test("channel and timestamp resets",
               [&] { test_channel_and_timestamp_resets(low_cpu_model); }) &&
      passed;
  passed = run_test("44.1 kHz resampling",
                    [&] { test_resampled_stream(low_cpu_model, 44100); }) &&
           passed;
  passed = run_test("96 kHz resampling",
                    [&] { test_resampled_stream(low_cpu_model, 96000); }) &&
           passed;
  passed = run_test("44.1 kHz timestamp refresh",
                    [&] {
                      test_resampled_timestamp_refresh(low_cpu_model, 44100);
                    }) &&
           passed;
  passed = run_test("96 kHz timestamp refresh",
                    [&] {
                      test_resampled_timestamp_refresh(low_cpu_model, 96000);
                    }) &&
           passed;
  passed =
      run_test("format transition resampler invalidation",
               [&] {
                 test_format_transition_invalidates_resamplers(low_cpu_model);
               }) &&
      passed;
  passed =
      run_test("resampled model replacement",
               [&] {
                 test_resampled_model_replacement(quality_model, low_cpu_model);
               }) &&
      passed;
  passed = run_test("DPDFNet8 signal integrity",
                    [&] { test_signal_integrity(quality_model); }) &&
           passed;
  passed = run_test("DPDFNet2 signal integrity",
                    [&] { test_signal_integrity(low_cpu_model); }) &&
           passed;
  passed = run_test("processing circuit breaker",
                    [&] { test_circuit_breaker(fixtures); }) &&
           passed;
  passed = run_test("resampled failure refresh",
                    [&] { test_resampled_failure_refresh(fixtures); }) &&
           passed;
  return passed ? 0 : 1;
}
