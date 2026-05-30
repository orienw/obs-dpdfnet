// SPDX-License-Identifier: GPL-2.0-or-later

#include "../src/dpdfnet-model.hpp"
#include "../src/stft.hpp"

#include <cmath>
#include <iostream>
#include <numeric>
#include <vector>

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: dpdfnet-model-smoke <model.onnx>\n";
    return 2;
  }

  try {
    DpdfnetModel model(argv[1]);
    StreamingStft stft(model.n_fft(), model.hop_size());

    std::vector<float> spec;
    std::vector<float> enhanced_spec;
    std::vector<float> enhanced_hop;

    for (int block = 0; block < 4; ++block) {
      std::vector<float> hop(static_cast<size_t>(model.hop_size()));
      for (size_t i = 0; i < hop.size(); ++i) {
        const size_t absolute = static_cast<size_t>(block) * hop.size() + i;
        const double t = static_cast<double>(absolute) /
                         static_cast<double>(model.sample_rate());
        hop[i] = static_cast<float>(
            0.04 * std::sin(2.0 * 3.14159265358979323846 * 180.0 * t));
      }

      stft.analysis(hop, spec);
      model.enhance_spectrum(spec, enhanced_spec);
      stft.synthesis(enhanced_spec, enhanced_hop);
    }

    double energy = 0.0;
    for (float sample : enhanced_hop)
      energy += static_cast<double>(sample) * static_cast<double>(sample);
    const double rms =
        std::sqrt(energy / static_cast<double>(enhanced_hop.size()));

    std::cout << "profile=" << model.profile() << "\n";
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
