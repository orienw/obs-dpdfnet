// SPDX-License-Identifier: GPL-2.0-or-later

#include "../src/dpdfnet-model.hpp"
#include "../src/stft.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>

namespace {
constexpr double kPi = 3.141592653589793238462643383279502884;

std::vector<float> synthetic_input(int sample_rate, int seconds) {
  const size_t frames = static_cast<size_t>(sample_rate) *
                        static_cast<size_t>(seconds);
  std::vector<float> input(frames, 0.0f);
  std::mt19937 rng(3);
  std::normal_distribution<float> noise(0.0f, 1.0f);

  for (size_t i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i) / static_cast<double>(sample_rate);
    float sample = 0.015f * noise(rng);
    sample += static_cast<float>(0.02 * std::sin(2.0 * kPi * 120.0 * t));

    if (t >= 1.0 && t < 3.0) {
      const double tm = t - 1.0;
      const double fade_in = std::min(1.0, tm / 0.15);
      const double fade_out = std::min(1.0, (2.0 - tm) / 0.15);
      const double env = fade_in * fade_out;
      const double voice = std::sin(2.0 * kPi * 150.0 * tm) +
                           0.45 * std::sin(2.0 * kPi * 300.0 * tm) +
                           0.22 * std::sin(2.0 * kPi * 450.0 * tm);
      sample += static_cast<float>(0.07 * env * voice);
    }

    input[i] = sample;
  }

  return input;
}

void write_f32(const char *path, const std::vector<float> &samples) {
  std::ofstream out(path, std::ios::binary);
  if (!out)
    throw std::runtime_error("failed to open output file");
  out.write(reinterpret_cast<const char *>(samples.data()),
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
    DpdfnetModel model(argv[1]);
    StreamingStft stft(model.n_fft(), model.hop_size());

    const std::vector<float> input =
        synthetic_input(model.sample_rate(), 4);
    std::vector<float> output;
    output.reserve(input.size());
    write_f32(argv[2], input);

    std::vector<float> input_buffer;
    input_buffer.reserve(static_cast<size_t>(model.n_fft()) + 1024);

    const size_t packet_size = 1024;
    const size_t window_size = static_cast<size_t>(model.n_fft());
    const size_t hop_size = static_cast<size_t>(model.hop_size());

    for (size_t packet = 0; packet < input.size(); packet += packet_size) {
      const size_t end = std::min(input.size(), packet + packet_size);
      input_buffer.insert(input_buffer.end(), input.begin() + packet,
                          input.begin() + end);

      while (input_buffer.size() >= window_size) {
        std::vector<float> frame(input_buffer.begin(),
                                 input_buffer.begin() + window_size);
        std::vector<float> noisy_spec;
        std::vector<float> enhanced_spec;
        std::vector<float> enhanced_hop;

        stft.analysis_frame(frame, noisy_spec);
        model.enhance_spectrum(noisy_spec, enhanced_spec);
        stft.synthesis(enhanced_spec, enhanced_hop);

        output.insert(output.end(), enhanced_hop.begin(), enhanced_hop.end());
        input_buffer.erase(
            input_buffer.begin(),
            input_buffer.begin() +
                static_cast<std::vector<float>::difference_type>(hop_size));
      }
    }

    if (!input_buffer.empty()) {
      input_buffer.resize(window_size, 0.0f);
      std::vector<float> noisy_spec;
      std::vector<float> enhanced_spec;
      std::vector<float> enhanced_hop;

      stft.analysis_frame(input_buffer, noisy_spec);
      model.enhance_spectrum(noisy_spec, enhanced_spec);
      stft.synthesis(enhanced_spec, enhanced_hop);
      output.insert(output.end(), enhanced_hop.begin(), enhanced_hop.end());
    }

    if (output.size() > input.size())
      output.resize(input.size());

    write_f32(argv[3], output);
    std::cout << "input_frames=" << input.size() << "\n";
    std::cout << "output_frames=" << output.size() << "\n";
    return 0;
  } catch (const std::exception &ex) {
    std::cerr << "error: " << ex.what() << "\n";
    return 1;
  }
}
