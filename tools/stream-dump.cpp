// SPDX-License-Identifier: GPL-2.0-or-later

#include "../src/dpdfnet-processor.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
constexpr uint64_t NS_PER_SECOND = 1000000000ULL;

std::vector<float> read_f32(const char *path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input)
    throw std::runtime_error("failed to open input file");
  const std::streamsize bytes = input.tellg();
  if (bytes < 0 || bytes % static_cast<std::streamsize>(sizeof(float)) != 0)
    throw std::runtime_error("input is not raw float32 audio");
  input.seekg(0);
  std::vector<float> samples(static_cast<size_t>(bytes) / sizeof(float));
  input.read(reinterpret_cast<char *>(samples.data()), bytes);
  return samples;
}

void write_f32(const char *path, const std::vector<float> &samples) {
  std::ofstream output(path, std::ios::binary);
  if (!output)
    throw std::runtime_error("failed to open output file");
  output.write(reinterpret_cast<const char *>(samples.data()),
               static_cast<std::streamsize>(samples.size() * sizeof(float)));
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 4) {
    std::cerr << "usage: dpdfnet-stream-dump <model.onnx> <input.f32> "
                 "<output.f32>\n";
    return 2;
  }

  try {
    const std::vector<float> input = read_f32(argv[2]);
    DpdfnetProcessor processor;
    auto model = prepare_dpdfnet_model(argv[1]);
    const uint32_t sample_rate =
        static_cast<uint32_t>(model.model->sample_rate());
    processor.set_format(sample_rate, 1);
    processor.replace_model(std::move(model));
    processor.set_controls({});

    const uint32_t packet_sizes[] = {137, 441, 480, 512, 960, 1024};
    std::vector<float> output;
    output.reserve(input.size());
    std::vector<float> padding(1024, 0.0f);
    size_t offset = 0;
    size_t packet_index = 0;
    uint64_t timestamp = NS_PER_SECOND;
    while (output.size() < input.size() && packet_index < 100000) {
      const uint32_t requested =
          packet_sizes[packet_index % std::size(packet_sizes)];
      const uint32_t frames = offset < input.size()
                                  ? static_cast<uint32_t>(std::min<size_t>(
                                        requested, input.size() - offset))
                                  : requested;
      DpdfnetAudioPacket packet;
      packet.frames = frames;
      packet.timestamp = timestamp;
      packet.data[0] =
          offset < input.size() ? input.data() + offset : padding.data();
      const auto result = processor.process(packet);
      if (result.event != DpdfnetEvent::None)
        throw std::runtime_error(result.message.data());
      if (result.disposition == DpdfnetDisposition::Passthrough)
        throw std::runtime_error("processor unexpectedly passed through");
      if (result.disposition == DpdfnetDisposition::Processed)
        output.insert(output.end(), result.data[0],
                      result.data[0] + result.frames);
      if (offset < input.size())
        offset += frames;
      timestamp += static_cast<uint64_t>(static_cast<double>(frames) /
                                         static_cast<double>(sample_rate) *
                                         NS_PER_SECOND);
      ++packet_index;
    }
    if (output.size() < input.size())
      throw std::runtime_error("processor failed to drain input");
    output.resize(input.size());
    write_f32(argv[3], output);
    std::cout << "input_frames=" << input.size() << "\n"
              << "output_frames=" << output.size() << "\n";
    return 0;
  } catch (const std::exception &ex) {
    std::cerr << "error: " << ex.what() << "\n";
    return 1;
  }
}
