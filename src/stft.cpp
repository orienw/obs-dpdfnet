// SPDX-License-Identifier: GPL-2.0-or-later

#include "stft.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <type_traits>

// The interleaved [r,i] spectrum the model consumes and produces is bit-for-bit
// a kiss_fft_cpx array, so analysis()/synthesis() transform straight in and out
// of the caller's float buffer with no interleave copy. These asserts fail the
// build if a non-float kissfft (e.g. a double-typed find_package fallback) is
// ever linked, which would silently corrupt that aliasing.
static_assert(sizeof(kiss_fft_scalar) == sizeof(float),
              "kissfft must be built with float scalars");
static_assert(sizeof(kiss_fft_cpx) == 2 * sizeof(float),
              "kiss_fft_cpx must be two contiguous floats");

namespace {
std::vector<float> vorbis_window(int n_fft) {
  std::vector<float> window(static_cast<size_t>(n_fft));
  const double half = static_cast<double>(n_fft) / 2.0;

  for (int i = 0; i < n_fft; ++i) {
    const double s = std::sin(0.5 * kPi * (static_cast<double>(i) + 0.5) / half);
    window[static_cast<size_t>(i)] =
        static_cast<float>(std::sin(0.5 * kPi * s * s));
  }

  return window;
}
} // namespace

StreamingStft::StreamingStft(int n_fft, int hop_size)
    : n_fft_(n_fft), hop_size_(hop_size), freq_bins_(n_fft / 2 + 1),
      window_(vorbis_window(n_fft)),
      time_frame_(static_cast<size_t>(n_fft), 0.0f),
      ola_buffer_(static_cast<size_t>(n_fft), 0.0f) {
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
  std::fill(ola_buffer_.begin(), ola_buffer_.end(), 0.0f);
}

void StreamingStft::analysis(const std::vector<float> &frame, float *spec) {
  if (frame.size() != static_cast<size_t>(n_fft_))
    throw std::runtime_error("DPDFNet STFT received an unexpected frame size");

  for (int i = 0; i < n_fft_; ++i)
    time_frame_[static_cast<size_t>(i)] =
        frame[static_cast<size_t>(i)] * window_[static_cast<size_t>(i)];

  kiss_fftr(forward_, time_frame_.data(), reinterpret_cast<kiss_fft_cpx *>(spec));
}

void StreamingStft::synthesis(const float *spec, std::vector<float> &hop) {
  kiss_fftri(inverse_, reinterpret_cast<const kiss_fft_cpx *>(spec),
             time_frame_.data());

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

  if (hop.size() != static_cast<size_t>(hop_size_))
    hop.resize(static_cast<size_t>(hop_size_));
  std::copy(ola_buffer_.begin(), ola_buffer_.begin() + hop_size_, hop.begin());
}
