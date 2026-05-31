// SPDX-License-Identifier: GPL-2.0-or-later

#include "../src/dpdfnet-model.hpp"
#include "../src/stft.hpp"

#include <cmath>
#include <iostream>
#include <vector>

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: dpdfnet-model-smoke <model.onnx>\n";
    return 2;
  }

  try {
    DpdfnetModel model(argv[1]);
    StreamingStft stft(model.n_fft(), model.hop_size());

    std::vector<float> enhanced_hop;

    const size_t window_size = static_cast<size_t>(model.n_fft());
    const size_t hop_size = static_cast<size_t>(model.hop_size());

    std::vector<float> input(window_size * 4);
    for (size_t i = 0; i < input.size(); ++i) {
      const double t =
          static_cast<double>(i) / static_cast<double>(model.sample_rate());
      input[i] = static_cast<float>(0.04 * std::sin(2.0 * kPi * 180.0 * t));
    }

    for (size_t start = 0; start + window_size <= input.size();
         start += hop_size) {
      std::vector<float> frame(input.begin() + start,
                               input.begin() + start + window_size);
      stft.analysis(frame, model.input_spectrum());
      model.enhance();
      stft.synthesis(model.output_spectrum(), enhanced_hop);
    }

    double energy = 0.0;
    for (float sample : enhanced_hop)
      energy += static_cast<double>(sample) * static_cast<double>(sample);
    const double rms =
        std::sqrt(energy / static_cast<double>(enhanced_hop.size()));

    std::cout << "model_name=" << model.name() << "\n";
    std::cout << "metadata_profile=" << model.profile() << "\n";
    std::cout << "sample_rate=" << model.sample_rate() << "\n";
    std::cout << "n_fft=" << model.n_fft() << "\n";
    std::cout << "hop_size=" << model.hop_size() << "\n";
    std::cout << "freq_bins=" << model.freq_bins() << "\n";
    std::cout << "output_frames=" << enhanced_hop.size() << "\n";
    std::cout << "output_rms=" << rms << "\n";
    return enhanced_hop.size() == static_cast<size_t>(model.hop_size()) &&
                   std::isfinite(rms)
               ? 0
               : 1;
  } catch (const std::exception &ex) {
    std::cerr << "error: " << ex.what() << "\n";
    return 1;
  }
}
