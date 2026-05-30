// SPDX-License-Identifier: GPL-2.0-or-later

#include "stft.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {
std::vector<float> vorbis_window(int n_fft) {
  constexpr double pi = 3.141592653589793238462643383279502884;
  std::vector<float> window(static_cast<size_t>(n_fft));
  const double half = static_cast<double>(n_fft) / 2.0;

  for (int i = 0; i < n_fft; ++i) {
    const double s = std::sin(0.5 * pi * (static_cast<double>(i) + 0.5) / half);
    window[static_cast<size_t>(i)] =
        static_cast<float>(std::sin(0.5 * pi * s * s));
  }

  return window;
}
} // namespace

StreamingStft::StreamingStft(int n_fft, int hop_size)
    : n_fft_(n_fft), hop_size_(hop_size), freq_bins_(n_fft / 2 + 1),
      window_(vorbis_window(n_fft)),
      input_buffer_(static_cast<size_t>(n_fft), 0.0f),
      time_frame_(static_cast<size_t>(n_fft), 0.0f),
      ola_buffer_(static_cast<size_t>(n_fft), 0.0f),
      spectrum_(static_cast<size_t>(freq_bins_)) {
  if (n_fft_ <= 0 || hop_size_ <= 0 || hop_size_ * 2 != n_fft_)
    throw std::runtime_error(
        "DPDFNet STFT requires a positive 50 percent overlap configuration");

  forward_ = kiss_fftr_alloc(n_fft_, 0, nullptr, nullptr);
  inverse_ = kiss_fftr_alloc(n_fft_, 1, nullptr, nullptr);

  if (!forward_ || !inverse_)
    throw std::runtime_error("Failed to allocate KissFFT plans");
}

StreamingStft::~StreamingStft() {
  kiss_fftr_free(forward_);
  kiss_fftr_free(inverse_);
}

void StreamingStft::reset() {
  std::fill(input_buffer_.begin(), input_buffer_.end(), 0.0f);
  std::fill(ola_buffer_.begin(), ola_buffer_.end(), 0.0f);
}

void StreamingStft::analysis(const std::vector<float> &hop,
                             std::vector<float> &spec_ri) {
  if (hop.size() != static_cast<size_t>(hop_size_))
    throw std::runtime_error("DPDFNet STFT received an unexpected hop size");

  std::move(input_buffer_.begin() + hop_size_, input_buffer_.end(),
            input_buffer_.begin());
  std::copy(hop.begin(), hop.end(),
            input_buffer_.begin() + (n_fft_ - hop_size_));

  for (int i = 0; i < n_fft_; ++i)
    time_frame_[static_cast<size_t>(i)] =
        input_buffer_[static_cast<size_t>(i)] * window_[static_cast<size_t>(i)];

  kiss_fftr(forward_, time_frame_.data(), spectrum_.data());

  spec_ri.resize(static_cast<size_t>(freq_bins_) * 2);
  for (int i = 0; i < freq_bins_; ++i) {
    spec_ri[static_cast<size_t>(i) * 2] = spectrum_[static_cast<size_t>(i)].r;
    spec_ri[static_cast<size_t>(i) * 2 + 1] =
        spectrum_[static_cast<size_t>(i)].i;
  }
}

void StreamingStft::analysis_frame(const std::vector<float> &frame,
                                   std::vector<float> &spec_ri) {
  if (frame.size() != static_cast<size_t>(n_fft_))
    throw std::runtime_error("DPDFNet STFT received an unexpected frame size");

  for (int i = 0; i < n_fft_; ++i)
    time_frame_[static_cast<size_t>(i)] =
        frame[static_cast<size_t>(i)] * window_[static_cast<size_t>(i)];

  kiss_fftr(forward_, time_frame_.data(), spectrum_.data());

  spec_ri.resize(static_cast<size_t>(freq_bins_) * 2);
  for (int i = 0; i < freq_bins_; ++i) {
    spec_ri[static_cast<size_t>(i) * 2] = spectrum_[static_cast<size_t>(i)].r;
    spec_ri[static_cast<size_t>(i) * 2 + 1] =
        spectrum_[static_cast<size_t>(i)].i;
  }
}

void StreamingStft::synthesis(const std::vector<float> &spec_ri,
                              std::vector<float> &hop) {
  if (spec_ri.size() != static_cast<size_t>(freq_bins_) * 2)
    throw std::runtime_error(
        "DPDFNet ISTFT received an unexpected spectrum size");

  for (int i = 0; i < freq_bins_; ++i) {
    spectrum_[static_cast<size_t>(i)].r = spec_ri[static_cast<size_t>(i) * 2];
    spectrum_[static_cast<size_t>(i)].i =
        spec_ri[static_cast<size_t>(i) * 2 + 1];
  }

  kiss_fftri(inverse_, spectrum_.data(), time_frame_.data());

  const float scale = 1.0f / static_cast<float>(n_fft_);
  for (int i = 0; i < n_fft_; ++i) {
    time_frame_[static_cast<size_t>(i)] *=
        scale * window_[static_cast<size_t>(i)];
  }

  std::move(ola_buffer_.begin() + hop_size_, ola_buffer_.end(),
            ola_buffer_.begin());
  std::fill(ola_buffer_.begin() + (n_fft_ - hop_size_), ola_buffer_.end(),
            0.0f);

  for (int i = 0; i < n_fft_; ++i)
    ola_buffer_[static_cast<size_t>(i)] += time_frame_[static_cast<size_t>(i)];

  hop.assign(ola_buffer_.begin(), ola_buffer_.begin() + hop_size_);
}
