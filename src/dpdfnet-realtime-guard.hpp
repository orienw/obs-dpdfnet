// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

struct DpdfnetRealtimeObservation {
  uint64_t budget_ns = 0;
  uint64_t debt_ns = 0;
  bool tripped = false;
};

class DpdfnetRealtimeBudgetGuard {
public:
  DpdfnetRealtimeObservation observe(uint64_t processing_ns,
                                     size_t processed_hops, int hop_size,
                                     int model_rate) noexcept {
    const uint64_t budget_ns =
        audio_budget_ns(processed_hops, hop_size, model_rate);
    if (!budget_ns)
      return {};

    const bool over_budget = processing_ns > budget_ns;
    if (over_budget) {
      debt_ns_ = saturating_add(debt_ns_, processing_ns - budget_ns);
    } else {
      const uint64_t headroom_ns = budget_ns - processing_ns;
      debt_ns_ = headroom_ns >= debt_ns_ ? 0 : debt_ns_ - headroom_ns;
    }

    if (!debt_ns_) {
      observed_audio_ns_ = 0;
    } else {
      observed_audio_ns_ = saturating_add(observed_audio_ns_, budget_ns);
    }

    return {budget_ns, debt_ns_,
            over_budget && debt_ns_ >= MAX_DEBT_NS &&
                observed_audio_ns_ >= MIN_OBSERVED_AUDIO_NS};
  }

  void reset() noexcept {
    debt_ns_ = 0;
    observed_audio_ns_ = 0;
  }

  uint64_t debt_ns() const noexcept { return debt_ns_; }
  uint64_t observed_audio_ns() const noexcept { return observed_audio_ns_; }

private:
  static constexpr uint64_t NS_PER_SECOND = 1'000'000'000ULL;
  static constexpr uint64_t MAX_DEBT_NS = 100'000'000ULL;
  static constexpr uint64_t MIN_OBSERVED_AUDIO_NS = 100'000'000ULL;

  static uint64_t saturating_add(uint64_t left, uint64_t right) noexcept {
    const uint64_t max = std::numeric_limits<uint64_t>::max();
    return right > max - left ? max : left + right;
  }

  static uint64_t audio_budget_ns(size_t processed_hops, int hop_size,
                                  int model_rate) noexcept {
    if (!processed_hops || hop_size <= 0 || model_rate <= 0)
      return 0;

    const uint64_t max = std::numeric_limits<uint64_t>::max();
    const uint64_t hops = static_cast<uint64_t>(processed_hops);
    const uint64_t hop = static_cast<uint64_t>(hop_size);
    const uint64_t rate = static_cast<uint64_t>(model_rate);
    const uint64_t frames = hops > max / hop ? max : hops * hop;
    const uint64_t seconds = frames / rate;
    const uint64_t remainder = frames % rate;
    if (seconds > max / NS_PER_SECOND)
      return max;
    const uint64_t whole_ns = seconds * NS_PER_SECOND;
    const uint64_t partial_ns = remainder * NS_PER_SECOND / rate;
    return saturating_add(whole_ns, partial_ns);
  }

  uint64_t debt_ns_ = 0;
  uint64_t observed_audio_ns_ = 0;
};
