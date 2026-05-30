// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <kiss_fftr.h>

#include <vector>

class StreamingStft {
public:
  StreamingStft(int n_fft, int hop_size);
  ~StreamingStft();

  StreamingStft(const StreamingStft &) = delete;
  StreamingStft &operator=(const StreamingStft &) = delete;

  void reset();
  void analysis(const std::vector<float> &hop, std::vector<float> &spec_ri);
  void analysis_frame(const std::vector<float> &frame,
                      std::vector<float> &spec_ri);
  void synthesis(const std::vector<float> &spec_ri, std::vector<float> &hop);

private:
  int n_fft_ = 0;
  int hop_size_ = 0;
  int freq_bins_ = 0;

  kiss_fftr_cfg forward_ = nullptr;
  kiss_fftr_cfg inverse_ = nullptr;

  std::vector<float> window_;
  std::vector<float> input_buffer_;
  std::vector<float> time_frame_;
  std::vector<float> ola_buffer_;
  std::vector<kiss_fft_cpx> spectrum_;
};
