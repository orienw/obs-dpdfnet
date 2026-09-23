// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
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

    const uint64_t max_debt_ns = probe_ ? PROBE_MAX_DEBT_NS : MAX_DEBT_NS;
    const uint64_t min_observed_ns =
        probe_ ? PROBE_MIN_OBSERVED_AUDIO_NS : MIN_OBSERVED_AUDIO_NS;
    return {budget_ns, debt_ns_,
            over_budget && debt_ns_ >= max_debt_ns &&
                observed_audio_ns_ >= min_observed_ns};
  }

  void reset() noexcept {
    debt_ns_ = 0;
    observed_audio_ns_ = 0;
  }

  // Probe mode trips on the old, tight thresholds. It covers the first
  // seconds after an automatic retry so a machine that still cannot keep
  // up pauses again before OBS buffering grows much.
  void set_probe(bool probe) noexcept { probe_ = probe; }
  bool probe() const noexcept { return probe_; }

  uint64_t debt_ns() const noexcept { return debt_ns_; }
  uint64_t observed_audio_ns() const noexcept { return observed_audio_ns_; }

private:
  static constexpr uint64_t NS_PER_SECOND = 1'000'000'000ULL;
  static constexpr uint64_t MAX_DEBT_NS = 500'000'000ULL;
  static constexpr uint64_t MIN_OBSERVED_AUDIO_NS = 2'000'000'000ULL;
  static constexpr uint64_t PROBE_MAX_DEBT_NS = 100'000'000ULL;
  static constexpr uint64_t PROBE_MIN_OBSERVED_AUDIO_NS = 100'000'000ULL;

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
  bool probe_ = false;
};

// Delays before processing is retried after a realtime overload. Once the
// schedule is exhausted, processing stays off until a manual reset.
class DpdfnetOverloadRetrySchedule {
public:
  static constexpr size_t MAX_ATTEMPTS = 3;

  uint64_t next_delay_ns() noexcept {
    if (attempts_ >= DELAYS_NS.size())
      return 0;
    return DELAYS_NS[attempts_++];
  }

  size_t attempts() const noexcept { return attempts_; }
  void reset() noexcept { attempts_ = 0; }

private:
  static constexpr std::array<uint64_t, MAX_ATTEMPTS> DELAYS_NS = {
      10'000'000'000ULL, 30'000'000'000ULL, 60'000'000'000ULL};

  size_t attempts_ = 0;
};
