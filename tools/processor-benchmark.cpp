// SPDX-License-Identifier: GPL-2.0-or-later

#include "../src/dpdfnet-processor.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
uint64_t percentile(const std::vector<uint64_t> &values, double fraction) {
  if (values.empty())
    return 0;
  const size_t index =
      std::min(values.size() - 1,
               static_cast<size_t>(std::ceil(values.size() * fraction) - 1));
  return values[index];
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 3) {
    std::cerr << "usage: dpdfnet-processor-benchmark <model.onnx> "
                 "<sample-rate>\n";
    return 2;
  }

  try {
    const std::string model_path = argv[1];
    const uint32_t sample_rate = static_cast<uint32_t>(std::stoul(argv[2]));
    if (sample_rate < 8000 || sample_rate > 384000)
      throw std::runtime_error("sample rate is outside the benchmark range");

    DpdfnetProcessor processor;
    processor.set_format(sample_rate, 1);
    auto model = prepare_dpdfnet_model(model_path);
    const std::string model_name = model.model->name();
    const int model_rate = model.model->sample_rate();
    const int model_hop = model.model->hop_size();
    processor.replace_model(std::move(model));
    if (sample_rate != static_cast<uint32_t>(model_rate)) {
      processor.replace_resamplers(
          prepare_dpdfnet_resamplers(sample_rate, model_rate, model_hop));
    }
    processor.set_controls({});

    const uint32_t frames = sample_rate / 100;
    const uint64_t deadline_ns = 10'000'000;
    constexpr size_t warmup_packets = 200;
    constexpr size_t measured_packets = 2000;
    std::vector<float> audio(frames);
    std::vector<uint64_t> durations;
    durations.reserve(measured_packets);
    uint64_t timestamp = 1'000'000'000;
    size_t measured_processed_packets = 0;
    size_t measured_processed_hops = 0;
    size_t deadline_misses = 0;

    for (size_t packet_index = 0;
         packet_index < warmup_packets + measured_packets; ++packet_index) {
      for (uint32_t frame = 0; frame < frames; ++frame) {
        const double t = static_cast<double>(packet_index * frames + frame) /
                         static_cast<double>(sample_rate);
        audio[frame] =
            static_cast<float>(0.04 * std::sin(2.0 * kPi * 170.0 * t) +
                               0.01 * std::sin(2.0 * kPi * 60.0 * t));
      }
      DpdfnetAudioPacket packet;
      packet.data[0] = audio.data();
      packet.frames = frames;
      packet.timestamp = timestamp;
      const auto start = std::chrono::steady_clock::now();
      const auto result = processor.process(packet);
      const auto stop = std::chrono::steady_clock::now();
      if (result.event != DpdfnetEvent::None)
        throw std::runtime_error(result.message.data());
      if (result.disposition == DpdfnetDisposition::Passthrough)
        throw std::runtime_error("benchmark unexpectedly passed audio through");

      if (packet_index >= warmup_packets) {
        if (result.disposition == DpdfnetDisposition::Processed)
          ++measured_processed_packets;
        measured_processed_hops += result.processed_hops;
        const uint64_t elapsed = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start)
                .count());
        durations.push_back(elapsed);
        if (elapsed > deadline_ns)
          ++deadline_misses;
      }
      timestamp += deadline_ns;
    }

    std::sort(durations.begin(), durations.end());
    std::cout << "model=" << model_name << "\n"
              << "sample_rate=" << sample_rate << "\n"
              << "packet_frames=" << frames << "\n"
              << "warmup_packets=" << warmup_packets << "\n"
              << "measured_packets=" << measured_packets << "\n"
              << "measured_processed_packets=" << measured_processed_packets
              << "\n"
              << "measured_processed_hops=" << measured_processed_hops << "\n"
              << "p50_us=" << percentile(durations, 0.50) / 1000.0 << "\n"
              << "p95_us=" << percentile(durations, 0.95) / 1000.0 << "\n"
              << "p99_us=" << percentile(durations, 0.99) / 1000.0 << "\n"
              << "p999_us=" << percentile(durations, 0.999) / 1000.0 << "\n"
              << "max_us=" << durations.back() / 1000.0 << "\n"
              << "deadline_us=" << deadline_ns / 1000.0 << "\n"
              << "deadline_misses=" << deadline_misses << "\n";
    return 0;
  } catch (const std::exception &ex) {
    std::cerr << "error: " << ex.what() << "\n";
    return 1;
  }
}
