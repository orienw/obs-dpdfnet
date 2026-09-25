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

  for (size_t hop = 1; hop <= 50; ++hop) {
    const auto observation = guard.observe(2 * hop_budget, 1, 480, 48000);
    require(observation.tripped == (hop == 50),
            "2x realtime load tripped at the wrong point");
    require(observation.debt_ns == hop * hop_budget,
            "overload observation reported the wrong accumulated debt");
  }

  guard.reset();
  for (size_t hop = 1; hop <= 500; ++hop) {
    const auto observation =
        guard.observe(hop_budget + 1'000'000, 1, 480, 48000);
    require(observation.tripped == (hop == 500),
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
  require(!guard.observe(1'000'000'000, 1, 480, 48000).tripped,
          "a one-second stall opened the overload circuit");
  for (size_t hop = 0; hop < 5; ++hop)
    require(!guard.observe(hop_budget + 1'000'000, 1, 480, 48000).tripped,
            "brief overload right after a stall opened the circuit");
  for (size_t hop = 0; hop < 111; ++hop)
    require(!guard.observe(1'000'000, 1, 480, 48000).tripped,
            "healthy processing tripped while repaying a stall");
  require(guard.debt_ns() == 0 && guard.observed_audio_ns() == 0,
          "healthy processing did not repay a one-second stall");

  guard.reset();
  require(!guard.observe(1'000'000'000, 1, 480, 48000).tripped,
          "a one-second stall opened the overload circuit");
  for (size_t hop = 1; hop <= 49; ++hop)
    require(guard.observe(hop_budget + 1'000'000, 1, 480, 48000).tripped ==
                (hop == 49),
            "overload that continued after a stall tripped at the wrong point");

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

  guard.reset();
  guard.set_probe(true);
  require(guard.probe(), "probe mode was not retained");
  for (size_t hop = 1; hop <= 10; ++hop)
    require(guard.observe(2 * hop_budget, 1, 480, 48000).tripped == (hop == 10),
            "probe mode did not trip 2x realtime load at the tight point");
  guard.reset();
  require(guard.probe(), "guard reset cleared probe mode");
  for (size_t hop = 1; hop <= 100; ++hop)
    require(guard.observe(hop_budget + 1'000'000, 1, 480, 48000).tripped ==
                (hop == 100),
            "probe mode did not trip 10 percent overload at the tight point");
  guard.set_probe(false);
  guard.reset();
  for (size_t hop = 1; hop <= 10; ++hop)
    require(!guard.observe(2 * hop_budget, 1, 480, 48000).tripped,
            "leaving probe mode did not restore the relaxed thresholds");
}

void test_overload_retry_schedule() {
  DpdfnetOverloadRetrySchedule schedule;
  require(schedule.attempts() == 0, "fresh retry schedule reported attempts");
  require(schedule.next_delay_ns() == 10'000'000'000ULL,
          "first retry was not scheduled after 10 s");
  require(schedule.next_delay_ns() == 30'000'000'000ULL,
          "second retry was not scheduled after 30 s");
  require(schedule.next_delay_ns() == 60'000'000'000ULL,
          "third retry was not scheduled after 60 s");
  for (int retry = 0; retry < 100; ++retry)
    require(schedule.next_delay_ns() == 60'000'000'000ULL,
            "later retries did not repeat every 60 s");
  require(schedule.attempts() == 103, "retry schedule miscounted attempts");
  schedule.reset();
  require(schedule.attempts() == 0 &&
              schedule.next_delay_ns() == 10'000'000'000ULL,
          "retry schedule did not restart after reset");
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
  const std::filesystem::path quality(quality_path);
  require(dpdfnet_paths_equivalent(
              (quality.parent_path() / "." / quality.filename()).string(),
              quality_path),
          "filesystem-equivalent model path was not recognized");
}

void check_packet_drain(const std::string &model_path, uint32_t rate,
                        const std::vector<uint32_t> &sizes, bool gaps = false) {
  auto processor = make_processor(model_path, rate, 2);
  DpdfnetControls controls;
  controls.bypass = true;
  processor.set_controls(controls);
  size_t prefill = 0;
  uint64_t latency = 0;
  if (rate != static_cast<uint32_t>(processor.model()->sample_rate())) {
    auto resamplers = prepare_dpdfnet_resamplers(
        rate, processor.model()->sample_rate(), processor.model()->hop_size());
    prefill = resamplers.prefill_frames();
    latency = resamplers.delay_ns();
    processor.replace_resamplers(std::move(resamplers));
  }
  std::array<std::deque<float>, 2> expected;
  for (auto &channel : expected)
    channel.resize(prefill, 0.0f);
  std::deque<uint64_t> timestamps;
  uint64_t sent = 0, received = 0, gap = 0;
  size_t outputs = 0;
  for (size_t index = 0; index < sizes.size(); ++index) {
    const uint32_t frames = sizes[index];
    if (gaps && index && index % 9 == 0)
      gap += 10'000'000;
    DpdfnetAudioPacket packet;
    packet.frames = frames;
    packet.timestamp = NS_PER_SECOND + sent * NS_PER_SECOND / rate + gap;
    std::array<std::vector<float>, 2> input;
    for (size_t channel = 0; channel < 2; ++channel) {
      input[channel].resize(frames);
      for (size_t i = 0; i < frames; ++i)
        input[channel][i] = static_cast<float>(
            0.05 * std::sin((sent + i) * 0.071 + channel));
      expected[channel].insert(expected[channel].end(), input[channel].begin(),
                               input[channel].end());
      packet.data[channel] = input[channel].data();
    }
    for (uint64_t i = 0; i < frames; ++i)
      timestamps.push_back(packet.timestamp + i * NS_PER_SECOND / rate - latency);
    sent += frames;
    const auto result = processor.process(packet);
    require(result.disposition != DpdfnetDisposition::Passthrough &&
                result.event == DpdfnetEvent::None,
            "valid packet stream failed open at callback " + std::to_string(index));
    if (result.disposition == DpdfnetDisposition::Processed) {
      require(result.frames && result.frames <= DPDFNET_MAX_REALTIME_PACKET_FRAMES,
              "drained output exceeded its realtime buffer");
      require(result.frames <= timestamps.size(), "processor emitted excess audio");
      for (uint64_t i = 0; i < result.frames; ++i) {
        const uint64_t timestamp = result.timestamp + i * NS_PER_SECOND / rate;
        const uint64_t expected_timestamp = timestamps.front();
        const uint64_t deviation = timestamp > expected_timestamp
                                       ? timestamp - expected_timestamp
                                       : expected_timestamp - timestamp;
        require(deviation <= 2, "drained output changed sample timestamps or crossed a gap");
        timestamps.pop_front();
        for (size_t channel = 0; channel < 2; ++channel) {
          require(result.data[channel][i] == expected[channel].front(),
                  "packet draining lost, duplicated, or reordered a bypass sample");
          expected[channel].pop_front();
        }
      }
      received += result.frames;
      ++outputs;
    }
  }
  const auto *model = processor.model();
  const uint64_t startup = static_cast<uint64_t>(
      model->n_fft() + model->hop_size() * model->output_delay_hops());
  const uint64_t bound = (startup * rate + model->sample_rate() - 1) /
                         model->sample_rate() + prefill + 512;
  require(outputs > 30, "processor failed to drain the packet stream");
  require(sent - received <= bound, "packet size changes stranded buffered audio");
  require(processor.state().capacity_failures == 0,
          "valid packet stream exceeded the capacity plan");
}

void test_variable_packets_and_bypass(const std::string &model_path) {
  const uint32_t sizes[] = {137, 441, 480, 512, 960, 1024};
  std::vector<uint32_t> stream;
  for (size_t i = 0; i < 80; ++i)
    stream.push_back(sizes[i % std::size(sizes)]);
  check_packet_drain(model_path, 48000, stream);
  check_packet_drain(model_path, 48000, stream, true);
}

void test_packet_size_transitions(const std::string &model_path) {
  for (uint32_t rate : {48000, 44100, 96000}) {
    for (uint32_t large : {1024u, 2048u, DPDFNET_MAX_REALTIME_PACKET_FRAMES}) {
      std::vector<uint32_t> stream(16, 64);
      stream.insert(stream.end(), 48, large);
      stream.insert(stream.end(), 1024, 1);
      stream.insert(stream.end(), 48, large);
      stream.insert(stream.end(), 80, 137);
      check_packet_drain(model_path, rate, stream);
    }
  }
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
      processed += result.frames;
      for (uint32_t frame = 0; frame < result.frames; ++frame) {
        require(std::isfinite(result.data[0][frame]),
                "extreme contract stream produced non-finite audio");
      }
    }
    timestamp += 20'000'000;
  }
  require(processed > 60 * data.size(),
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
  require(processor.process(packet).disposition !=
              DpdfnetDisposition::Passthrough,
          "overload pause dropped out of the delay-matched pipeline");

  processor.reset_state();
  snapshot = processor.snapshot();
  require(!snapshot.processing_disabled &&
              snapshot.disable_reason == DpdfnetDisableReason::None,
          "Reset did not close the realtime overload circuit");
  packet.timestamp += 20'000'000;
  require(processor.process(packet).disposition !=
              DpdfnetDisposition::Passthrough,
          "Reset did not resume processing after realtime overload");

  require(!processor.resume_after_overload(),
          "resume acted without an overload pause");
  require(processor.disable_for_realtime_overload("second overload") &&
              processor.resume_after_overload(),
          "resume did not close the realtime overload circuit");
  snapshot = processor.snapshot();
  require(!snapshot.processing_disabled && snapshot.last_error.empty(),
          "resume left the overload state behind");
}

// With a delayed-identity model, enhanced and paused output are the same
// signal, so a stream that pauses and resumes must match one that never
// paused: no dropped, repeated, or shifted samples at either transition.
void test_overload_pause_continuity(const std::filesystem::path &fixtures) {
  const std::string model = (fixtures / "valid_delayed_identity.onnx").string();
  for (uint32_t rate : {48000, 44100, 96000}) {
    auto reference = make_processor(model, rate);
    auto paused = make_processor(model, rate);
    const uint32_t frames = rate / 100;
    std::vector<float> input(frames);
    std::vector<float> expected, actual;
    std::vector<uint64_t> expected_timestamps, actual_timestamps;
    for (uint64_t index = 0; index < 60; ++index) {
      if (index == 15)
        require(paused.disable_for_realtime_overload("test overload"),
                "overload pause did not open");
      if (index == 35)
        require(paused.resume_after_overload(), "overload pause did not close");
      for (size_t i = 0; i < frames; ++i)
        input[i] =
            static_cast<float>(0.1 * std::sin((index * frames + i) * 0.13));
      DpdfnetAudioPacket packet;
      packet.data[0] = input.data();
      packet.frames = frames;
      packet.timestamp = NS_PER_SECOND + index * 10'000'000;
      const auto collect = [&](DpdfnetProcessor &processor,
                               std::vector<float> &samples,
                               std::vector<uint64_t> &timestamps) {
        const auto result = processor.process(packet);
        require(result.disposition != DpdfnetDisposition::Passthrough &&
                    result.event == DpdfnetEvent::None,
                "overload pause left the delay-matched pipeline");
        if (result.disposition == DpdfnetDisposition::Processed) {
          samples.insert(samples.end(), result.data[0],
                         result.data[0] + result.frames);
          timestamps.push_back(result.timestamp);
        }
      };
      collect(reference, expected, expected_timestamps);
      collect(paused, actual, actual_timestamps);
    }
    require(actual.size() == expected.size() &&
                actual_timestamps == expected_timestamps,
            "overload pause changed the output timeline at " +
                std::to_string(rate) + " Hz");
    for (size_t i = 0; i < actual.size(); ++i)
      require(nearly_equal(actual[i], expected[i], 2e-6f),
              "overload pause changed the audio at " + std::to_string(rate) +
                  " Hz, sample " + std::to_string(i));
  }
}

struct FadeRun {
  std::vector<float> output;
  // Model inferences per input packet.
  std::vector<size_t> inference;
};

// Feeds 480-frame packets, pausing and resuming before the given packets. The
// stream must stay delay-matched: Pending only during startup, then one
// contiguous, finite 480-frame packet out per packet in, so a test cannot
// pass on audio from before the transition it checks.
FadeRun run_fade(DpdfnetProcessor &processor, const std::vector<float> &input,
                 uint64_t pause_at, uint64_t resume_at) {
  FadeRun run;
  bool started = false;
  size_t startup = 0;
  uint64_t next_timestamp = 0;
  for (uint64_t index = 0; index * 480 < input.size(); ++index) {
    if (index == pause_at)
      require(processor.disable_for_realtime_overload("test overload"),
              "overload pause did not open");
    if (index == resume_at)
      require(processor.resume_after_overload(),
              "overload pause did not close");
    DpdfnetAudioPacket packet;
    packet.data[0] = input.data() + index * 480;
    packet.frames = 480;
    packet.timestamp = NS_PER_SECOND + index * 10'000'000;
    const auto result = processor.process(packet);
    const std::string at = " at packet " + std::to_string(index);
    require(result.disposition != DpdfnetDisposition::Passthrough &&
                result.event == DpdfnetEvent::None,
            "fade test left the delay-matched pipeline" + at);
    run.inference.push_back(result.inference_hops);
    if (result.disposition == DpdfnetDisposition::Pending) {
      require(!started, "fade test stopped receiving audio" + at);
      ++startup;
      continue;
    }
    require(result.frames == 480 &&
                (!started || result.timestamp == next_timestamp),
            "fade test broke the output timeline" + at);
    started = true;
    next_timestamp = result.timestamp + 10'000'000;
    for (uint32_t frame = 0; frame < result.frames; ++frame)
      require(std::isfinite(result.data[0][frame]),
              "fade test produced non-finite audio" + at);
    run.output.insert(run.output.end(), result.data[0],
                      result.data[0] + result.frames);
  }
  require(started && startup <= 6, "fade test startup took too long");
  return run;
}

// Heard-to-input level ratio per 10 ms block for noise at a 50 dB limit,
// with an overload pause and resume at the given packets.
std::vector<double> fade_trace(const std::filesystem::path &model, double wet,
                               uint64_t pause_at, uint64_t resume_at,
                               uint64_t packets) {
  auto processor = make_processor(model.string());
  DpdfnetControls controls;
  controls.attenuation_limit_db = 50.0;
  controls.wet_mix = wet;
  processor.set_controls(controls);
  std::mt19937 rng(11);
  std::normal_distribution<float> noise(0.0f, 0.05f);
  std::vector<float> input(packets * 480);
  for (float &sample : input)
    sample = noise(rng);
  const auto output = run_fade(processor, input, pause_at, resume_at).output;
  std::vector<double> ratio;
  for (size_t block = 0; (block + 1) * 480 <= output.size(); ++block) {
    double in = 0.0;
    double out = 0.0;
    for (size_t i = block * 480; i < (block + 1) * 480; ++i) {
      in += static_cast<double>(input[i]) * input[i];
      out += static_cast<double>(output[i]) * output[i];
    }
    ratio.push_back(10.0 * std::log10(out / in + 1e-30));
  }
  return ratio;
}

// A pause and a resume must fade the noise floor instead of switching it. At a
// 50 dB limit, switching in one hop steps the level by about 45 dB, which
// sounds like a click. The pause fades about 5 dB per 10 ms.
void test_overload_pause_fades(const std::string &model_path) {
  auto processor = make_processor(model_path);
  DpdfnetControls controls;
  controls.attenuation_limit_db = 50.0;
  processor.set_controls(controls);
  std::mt19937 rng(7);
  std::normal_distribution<float> noise(0.0f, 0.01f);
  std::vector<float> input(900 * 480);
  for (float &sample : input)
    sample = noise(rng);
  const auto output = run_fade(processor, input, 300, 600).output;
  std::vector<double> levels;
  for (size_t block = 0; (block + 1) * 480 <= output.size(); ++block) {
    double energy = 0.0;
    for (size_t i = block * 480; i < (block + 1) * 480; ++i)
      energy += static_cast<double>(output[i]) * output[i];
    levels.push_back(10.0 * std::log10(energy / 480.0 + 1e-20));
  }
  const double input_db = 20.0 * std::log10(0.01);
  const auto level_at = [&](double seconds) {
    return levels[static_cast<size_t>(seconds * 100.0)];
  };
  require(level_at(2.5) < input_db - 30.0 && level_at(3.5) > input_db - 3.0 &&
              level_at(6.5) < input_db - 30.0,
          "fade test did not pause and resume suppression");
  for (size_t block = 250; block + 1 < 700; ++block) {
    require(std::fabs(levels[block + 1] - levels[block]) < 8.0,
            "overload pause or resume stepped the noise floor by " +
                std::to_string(levels[block + 1] - levels[block]) + " dB at " +
                std::to_string(block / 100.0) + " s");
  }
}

size_t first_block(const std::vector<double> &ratio, size_t from, bool above,
                   double level) {
  for (size_t block = from; block < ratio.size(); ++block) {
    if (above ? ratio[block] > level : ratio[block] < level)
      return block;
  }
  throw TestFailure("fade never crossed " + std::to_string(level) + " dB");
}

// A pause must raise the level from -50 dB to 0 dB over 100 ms in steps of
// about 5 dB, and a resume lower it back over 200 ms in steps of about
// 2.5 dB, starting output_delay_hops later because the model restarts.
void check_fade_envelope(const std::vector<double> &ratio, const char *name) {
  const std::string label = std::string(name) + ": ";
  for (size_t block = 50; block < 90; ++block)
    require(std::fabs(ratio[block] + 50.0) < 0.5,
            label + "steady suppression is not the 50 dB limit");
  const size_t pause_start = first_block(ratio, 90, true, -49.5);
  const size_t pause_end = first_block(ratio, pause_start, true, -0.5);
  const size_t resume_start = first_block(ratio, pause_end + 2, false, -0.5);
  const size_t resume_end = first_block(ratio, resume_start, false, -49.5);
  require(pause_end - pause_start >= 9 && pause_end - pause_start <= 11,
          label + "pause fade did not last 100 ms");
  require(resume_end - resume_start >= 19 && resume_end - resume_start <= 21,
          label + "resume fade did not last 200 ms");
  const int pause_lag = static_cast<int>(pause_start) - 100;
  const int resume_lag = static_cast<int>(resume_start) - 200;
  require(resume_lag - pause_lag == 4,
          label + "resume did not bridge output_delay_hops before fading: " +
              "pause lag " + std::to_string(pause_lag) + ", resume lag " +
              std::to_string(resume_lag));
  for (size_t block = pause_end; block < resume_start; ++block)
    require(std::fabs(ratio[block]) < 0.5,
            label + "paused audio is not the unsuppressed input");
  for (size_t block = 90; block + 1 < ratio.size(); ++block) {
    const double step = ratio[block + 1] - ratio[block];
    require(std::fabs(step) < 6.0, label + "level stepped by " +
                                       std::to_string(step) + " dB at block " +
                                       std::to_string(block));
    // Levels only rise until the resume fade begins, then only fall.
    require(block + 1 < resume_start ? step > -0.5 : step < 0.5,
            label + "fade reversed direction at block " +
                std::to_string(block));
  }
}

void test_overload_fade_envelope(const std::filesystem::path &fixtures) {
  check_fade_envelope(
      fade_trace(fixtures / "valid_delayed_silence.onnx", 1.0, 100, 200, 300),
      "silence model");
  // Enhanced output is the inverted input, so a 50% mix cancels to the limit.
  // Fading anything but the whole mix would unmask the full level at once.
  check_fade_envelope(
      fade_trace(fixtures / "valid_delayed_negation.onnx", 0.5, 100, 200, 300),
      "phase-inverting model at 50% mix");
}

// A retry can land before a pause has faded out, for example when callbacks
// stall. The model is still running, so the fade must turn around without a
// jump or a bridge.
void test_overload_fade_interrupted(const std::filesystem::path &fixtures) {
  for (const char *name :
       {"valid_delayed_silence.onnx", "valid_delayed_negation.onnx"}) {
    const double wet =
        std::string(name).find("negation") != std::string::npos ? 0.5 : 1.0;
    const auto ratio = fade_trace(fixtures / name, wet, 100, 105, 200);
    double peak = -100.0;
    for (size_t block = 90; block + 1 < ratio.size(); ++block) {
      peak = std::max(peak, ratio[block]);
      require(std::fabs(ratio[block + 1] - ratio[block]) < 6.0,
              std::string(name) + ": interrupted fade stepped by " +
                  std::to_string(ratio[block + 1] - ratio[block]) + " dB");
    }
    require(peak > -35.0 && peak < -15.0,
            std::string(name) + ": interrupted fade did not turn around");
    require(std::fabs(ratio.back() + 50.0) < 0.5,
            std::string(name) +
                ": interrupted fade did not return to the limit");
  }
}

// Inference during a pause runs from the first packet after it.
size_t inference_after(const FadeRun &run, size_t from) {
  size_t total = 0;
  for (size_t index = from; index < run.inference.size(); ++index)
    total += run.inference[index];
  return total;
}

// A pause fades out only enhanced audio that has been heard. Before the first
// processed hop goes out there is none, whether startup output is still
// discarded or the delay line has just filled, so the model stops at once.
void test_overload_pause_during_startup(const std::filesystem::path &fixtures) {
  std::mt19937 rng(11);
  std::normal_distribution<float> noise(0.0f, 0.05f);
  std::vector<float> input(100 * 480);
  for (float &sample : input)
    sample = noise(rng);
  // The four-hop delay model's first processed hop goes out at packet 5.
  for (uint64_t pause_at = 0; pause_at <= 6; ++pause_at) {
    auto processor =
        make_processor((fixtures / "valid_delayed_silence.onnx").string());
    const auto run = run_fade(processor, input, pause_at, 1000);
    const size_t expected = pause_at <= 5 ? 0 : 9;
    require(inference_after(run, pause_at) == expected,
            "pause before packet " + std::to_string(pause_at) +
                " ran the model " +
                std::to_string(inference_after(run, pause_at)) + " times");
  }
  // With nothing to fade, output is the delayed input once the first hop has
  // faded in through overlap-add.
  auto processor =
      make_processor((fixtures / "valid_delayed_silence.onnx").string());
  const auto run = run_fade(processor, input, 0, 1000);
  for (size_t i = 2 * 480; i < run.output.size(); ++i)
    require(nearly_equal(run.output[i], input[i], 1e-5f),
            "pause during startup did not pass the input through");
}

// A pause runs the model only while its output can still be heard, then stops
// it, and a resume from a full pause restarts it on every hop.
void test_overload_fade_out_inference(const std::filesystem::path &fixtures) {
  const std::vector<float> input(200 * 480, 0.01f);
  struct Setting {
    const char *name;
    bool bypass;
    double wet_mix;
    double limit_db;
    size_t fade_out_inferences;
  };
  for (const Setting &setting : {Setting{"heard", false, 1.0, 50.0, 9},
                                 Setting{"Bypass", true, 1.0, 50.0, 0},
                                 Setting{"0% mix", false, 0.0, 50.0, 0},
                                 Setting{"0 dB limit", false, 1.0, 0.0, 0}}) {
    auto processor =
        make_processor((fixtures / "valid_delayed_silence.onnx").string());
    DpdfnetControls controls;
    controls.bypass = setting.bypass;
    controls.wet_mix = setting.wet_mix;
    controls.attenuation_limit_db = setting.limit_db;
    processor.set_controls(controls);
    const auto run = run_fade(processor, input, 100, 150);
    size_t paused = 0;
    for (size_t index = 100; index < 150; ++index)
      paused += run.inference[index];
    require(paused == setting.fade_out_inferences,
            std::string(setting.name) + ": fade-out ran the model " +
                std::to_string(paused) + " times");
    for (size_t index = 150; index < 200; ++index)
      require(run.inference[index] == 1,
              std::string(setting.name) +
                  ": resume did not run the model on every hop");
  }
}

// A callback that fails still reports the inference it spent, whether it was
// processing normally or fading out a pause.
void test_failure_reports_inference(const std::filesystem::path &fixtures) {
  const auto model =
      (fixtures / "runtime_nonfinite_spectrum_output.onnx").string();
  std::vector<float> silence(960, 0.0f);
  std::vector<float> data(960, 0.1f);
  for (bool paused : {false, true}) {
    auto processor = make_processor(model);
    DpdfnetAudioPacket packet;
    packet.frames = static_cast<uint32_t>(data.size());
    // The model stays finite on silence, so the stream reaches steady output.
    for (uint64_t index = 0; index < 4; ++index) {
      packet.data[0] = silence.data();
      packet.timestamp = NS_PER_SECOND + index * 20'000'000;
      require(processor.process(packet).event == DpdfnetEvent::None,
              "silent warm-up failed");
    }
    if (paused)
      require(processor.disable_for_realtime_overload("test overload"),
              "overload pause did not open");
    packet.data[0] = data.data();
    packet.timestamp = NS_PER_SECOND + 4 * 20'000'000;
    const auto result = processor.process(packet);
    require(result.event == DpdfnetEvent::ProcessingFailure &&
                result.inference_hops == 1,
            std::string(paused ? "paused" : "normal") +
                " failure did not report its inference");
  }
}

std::vector<float> tone(size_t packets, float peak) {
  std::vector<float> signal(packets * 480);
  for (size_t i = 0; i < signal.size(); ++i)
    signal[i] =
        peak * static_cast<float>(std::sin(2.0 * kPi * 1000.0 * i / 48000.0));
  return signal;
}

// A fade must not amplify: a quiet stretch right before a pause once made a
// held output-to-input ratio push a tone far past full scale.
void test_overload_fade_gain_bound(const std::string &model_path) {
  auto processor = make_processor(model_path);
  DpdfnetControls controls;
  controls.attenuation_limit_db = 50.0;
  processor.set_controls(controls);
  auto input = tone(160, 0.1f);
  for (size_t i = 94 * 480; i < 100 * 480; ++i)
    input[i] *= 0.1f;
  const auto output = run_fade(processor, input, 100, 1000).output;
  float peak = 0.0f;
  for (float sample : output)
    peak = std::max(peak, std::fabs(sample));
  require(peak < 0.15f,
          "overload fade amplified audio to a peak of " + std::to_string(peak));
}

// Model output on silent input must fade like any other: it once stayed
// silent until the last resume hop and then appeared at full level.
void test_overload_fade_model_residual(const std::filesystem::path &fixtures) {
  auto processor =
      make_processor((fixtures / "valid_constant_tone.onnx").string());
  DpdfnetControls controls;
  controls.attenuation_limit_db = 50.0;
  processor.set_controls(controls);
  const std::vector<float> silence(300 * 480, 0.0f);
  const auto output = run_fade(processor, silence, 100, 200).output;
  std::vector<double> levels;
  for (size_t block = 0; (block + 1) * 480 <= output.size(); ++block) {
    double energy = 0.0;
    for (size_t i = block * 480; i < (block + 1) * 480; ++i)
      energy += static_cast<double>(output[i]) * output[i];
    levels.push_back(10.0 * std::log10(energy / 480.0 + 1e-30));
  }
  const double steady = levels[80];
  require(steady > -60.0, "constant-tone model produced no steady tone");
  const size_t silent = first_block(levels, 90, false, -250.0);
  const size_t audible = first_block(levels, silent, true, -250.0);
  require(audible > 150 && audible <= 201,
          "model output did not start fading in with the resume");
  require(levels[audible] > steady - 20.0 && levels[audible + 3] > steady - 6.0,
          "model output did not start fading in with the resume");
  for (size_t block = audible; block + 1 < levels.size(); ++block) {
    require(levels[block + 1] >= levels[block] - 0.1 &&
                levels[block] < steady + 0.5,
            "model output did not rise steadily to its level");
  }
  require(std::fabs(levels[audible + 20] - steady) < 0.5,
          "model output did not reach its level in 200 ms");
}

// A fade must keep the waveform continuous even when the model inverts the
// signal, where the heard transfer passes from -1 to +1.
void test_overload_fade_waveform(const std::filesystem::path &fixtures) {
  auto processor =
      make_processor((fixtures / "valid_delayed_negation.onnx").string());
  DpdfnetControls controls;
  controls.attenuation_limit_db = 50.0;
  processor.set_controls(controls);
  const auto output = run_fade(processor, tone(300, 0.1f), 100, 200).output;
  float steady_step = 0.0f;
  for (size_t i = 60 * 480; i < 90 * 480; ++i)
    steady_step = std::max(steady_step, std::fabs(output[i] - output[i - 1]));
  for (size_t i = 90 * 480; i < output.size(); ++i)
    require(std::fabs(output[i] - output[i - 1]) < 1.2f * steady_step,
            "overload fade broke the waveform at sample " + std::to_string(i));
}

void test_overload_pause_skips_model(const std::filesystem::path &fixtures) {
  auto processor = make_processor(
      (fixtures / "runtime_nonfinite_spectrum_output.onnx").string());
  require(processor.disable_for_realtime_overload("test overload"),
          "overload pause did not open");
  // The model stays finite on silence, which carries the pause's 100 ms
  // fade-out. After it, input the model would reject must pass untouched.
  std::vector<float> silence(960, 0.0f);
  std::vector<float> data(960, 0.1f);
  DpdfnetAudioPacket packet;
  packet.frames = static_cast<uint32_t>(data.size());
  size_t checked = 0;
  for (uint64_t index = 0; index < 18; ++index) {
    packet.data[0] = index < 6 ? silence.data() : data.data();
    packet.timestamp = NS_PER_SECOND + index * 20'000'000;
    const auto result = processor.process(packet);
    require(result.disposition != DpdfnetDisposition::Passthrough &&
                result.event == DpdfnetEvent::None,
            "overload pause ran the model after its fade-out");
    // Output lags the input by a packet and a hop, and the switch from
    // silence fades in through overlap-add.
    if (index >= 9 && result.disposition == DpdfnetDisposition::Processed) {
      ++checked;
      for (uint32_t frame = 0; frame < result.frames; ++frame)
        require(nearly_equal(result.data[0][frame], 0.1f, 1e-5f),
                "overload pause did not pass the delayed input through");
    }
  }
  require(checked > 6, "overload pause produced no delayed audio");
  require(processor.resume_after_overload(), "overload pause did not close");
  packet.timestamp = NS_PER_SECOND + 18 * 20'000'000;
  require(processor.process(packet).event == DpdfnetEvent::ProcessingFailure,
          "resume did not run the model again");
}

void test_lfe_channel(const std::filesystem::path &fixtures) {
  const std::string model = (fixtures / "valid_identity.onnx").string();
  const std::pair<size_t, size_t> layouts[] = {{2, 2}, {3, 2}, {4, 4},
                                               {5, 3}, {6, 3}, {8, 3}};
  for (const auto &[channels, lfe] : layouts) {
    auto bypass = make_processor(model, 48000, channels);
    auto wet = make_processor(model, 48000, channels);
    auto half = make_processor(model, 48000, channels);
    DpdfnetControls controls;
    controls.bypass = true;
    bypass.set_controls(controls);
    controls.bypass = false;
    wet.set_controls(controls);
    controls.wet_mix = 0.5;
    half.set_controls(controls);

    std::array<std::vector<float>, DPDFNET_MAX_AUDIO_PLANES> input;
    size_t processed = 0;
    for (uint64_t index = 0; index < 30; ++index) {
      DpdfnetAudioPacket packet;
      packet.frames = 480;
      packet.timestamp = NS_PER_SECOND + index * 10'000'000;
      for (size_t channel = 0; channel < channels; ++channel) {
        input[channel].resize(packet.frames);
        for (size_t i = 0; i < packet.frames; ++i)
          input[channel][i] = static_cast<float>(
              0.05 * std::sin((index * packet.frames + i) * 0.07 + channel));
        packet.data[channel] = input[channel].data();
      }
      const auto dry = bypass.process(packet);
      const auto full = wet.process(packet);
      const auto mixed = half.process(packet);
      if (full.disposition != DpdfnetDisposition::Processed)
        continue;
      require(dry.frames == full.frames && mixed.frames == full.frames,
              "LFE test processors drifted apart");
      ++processed;
      for (size_t channel = 0; channel < channels; ++channel) {
        for (uint32_t frame = 0; frame < full.frames; ++frame) {
          const float expected_wet =
              channel == lfe ? 0.0f : full.data[0][frame];
          require(full.data[channel][frame] == expected_wet,
                  "enhanced voice reached the wrong channels with " +
                      std::to_string(channels) + " channels");
          if (channel == lfe)
            require(nearly_equal(mixed.data[channel][frame],
                                 0.5f * dry.data[channel][frame]),
                    "LFE channel lost its dry share");
        }
      }
    }
    require(processed > 20, "LFE test produced no processed audio");
  }
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

void test_resampled_stream(const std::filesystem::path &fixtures,
                           uint32_t sample_rate) {
  // An identity model leaves the resamplers as the only change to the audio.
  DpdfnetProcessor processor = make_processor(
      (fixtures / "valid_identity.onnx").string(), sample_rate);
  const uint32_t packet_frames = sample_rate / 100;
  std::vector<float> packet_data(packet_frames, 0.025f);
  uint64_t timestamp = NS_PER_SECOND;
  size_t processed = 0;
  size_t emitted = 0;
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
      // The resamplers settle within the first 20 ms of output.
      for (uint32_t frame = 0; frame < result.frames; ++frame, ++emitted)
        require(emitted < sample_rate / 50 ||
                    nearly_equal(result.data[0][frame], 0.025f, 1e-5f),
                "resampled path changed the input level");
    }
    timestamp += 10'000'000;
  }
  require(processed > 60, "resampled path failed to reach steady state");

  const auto snapshot = processor.snapshot();
  processor.replace_resamplers(prepare_dpdfnet_resamplers(
      snapshot.sample_rate, snapshot.model_rate, snapshot.hop_size));
  processor.reset_state();
  std::fill(packet_data.begin(), packet_data.end(), 0.0f);
  processed = 0;
  for (size_t i = 0; i < 8; ++i) {
    DpdfnetAudioPacket packet;
    packet.data[0] = packet_data.data();
    packet.frames = packet_frames;
    packet.timestamp = 3 * NS_PER_SECOND + i * 10'000'000;
    const auto result = processor.process(packet);
    if (result.disposition == DpdfnetDisposition::Processed) {
      ++processed;
      for (uint32_t frame = 0; frame < result.frames; ++frame)
        require(std::fabs(result.data[0][frame]) < 1e-6f,
                "reset leaked earlier audio into the resampled path");
    }
  }
  require(processed > 4, "resampled path produced no audio after reset");
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
    require(first == second, "reset did not produce deterministic output");

    double input = 0.0;
    double output = 0.0;
    for (size_t i = 0; i < signal.size(); ++i) {
      input += static_cast<double>(signal[i]) * signal[i];
      output += static_cast<double>(first[i]) * first[i];
    }
    if (kind == 0) {
      require(output < 1e-9, "silence test produced audible output");
      continue;
    }
    // None of the signals is speech, so the model suppresses each one to
    // about the attenuation limit, and the limit stops it going further.
    const double gain_db = 10.0 * std::log10(output / input);
    require(gain_db > -controls.attenuation_limit_db - 1.0 &&
                gain_db < -controls.attenuation_limit_db / 2.0,
            "signal kind " + std::to_string(kind) + " changed level by " +
                std::to_string(gain_db) + " dB");
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
  passed = run_test("overload retry schedule", test_overload_retry_schedule) &&
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
  passed = run_test("overload pause continuity",
                    [&] { test_overload_pause_continuity(fixtures); }) &&
           passed;
  passed = run_test("overload pause fades",
                    [&] { test_overload_pause_fades(low_cpu_model); }) &&
           passed;
  passed = run_test("overload fade envelope",
                    [&] { test_overload_fade_envelope(fixtures); }) &&
           passed;
  passed = run_test("interrupted overload fade",
                    [&] { test_overload_fade_interrupted(fixtures); }) &&
           passed;
  passed = run_test("overload pause during startup",
                    [&] { test_overload_pause_during_startup(fixtures); }) &&
           passed;
  passed = run_test("failure reports inference",
                    [&] { test_failure_reports_inference(fixtures); }) &&
           passed;
  passed = run_test("overload fade-out inference",
                    [&] { test_overload_fade_out_inference(fixtures); }) &&
           passed;
  passed = run_test("overload fade gain bound",
                    [&] { test_overload_fade_gain_bound(low_cpu_model); }) &&
           passed;
  passed = run_test("overload fade of model residual",
                    [&] { test_overload_fade_model_residual(fixtures); }) &&
           passed;
  passed = run_test("overload fade waveform",
                    [&] { test_overload_fade_waveform(fixtures); }) &&
           passed;
  passed = run_test("overload pause skips the model",
                    [&] { test_overload_pause_skips_model(fixtures); }) &&
           passed;
  passed =
      run_test("LFE channel", [&] { test_lfe_channel(fixtures); }) && passed;
  passed = run_test("returned audio survives format update",
                    [&] {
                      test_output_storage_survives_format_update(low_cpu_model);
                    }) &&
           passed;
  passed = run_test("variable packets and aligned bypass",
                    [&] { test_variable_packets_and_bypass(quality_model); }) &&
           passed;
  passed = run_test("packet size transitions",
                    [&] { test_packet_size_transitions(low_cpu_model); }) && passed;
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
                    [&] { test_resampled_stream(fixtures, 44100); }) &&
           passed;
  passed = run_test("96 kHz resampling",
                    [&] { test_resampled_stream(fixtures, 96000); }) &&
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
