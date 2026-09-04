// SPDX-License-Identifier: GPL-2.0-or-later

#include "../src/dpdfnet-processor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr uint32_t kSampleRate = 48000;
constexpr uint64_t kNanosecondsPerSecond = 1000000000ULL;

uint16_t read_u16(std::istream &input) {
  uint8_t bytes[2] = {};
  input.read(reinterpret_cast<char *>(bytes), sizeof(bytes));
  return static_cast<uint16_t>(bytes[0]) | static_cast<uint16_t>(bytes[1] << 8);
}

uint32_t read_u32(std::istream &input) {
  uint8_t bytes[4] = {};
  input.read(reinterpret_cast<char *>(bytes), sizeof(bytes));
  return static_cast<uint32_t>(bytes[0]) |
         (static_cast<uint32_t>(bytes[1]) << 8) |
         (static_cast<uint32_t>(bytes[2]) << 16) |
         (static_cast<uint32_t>(bytes[3]) << 24);
}

void write_u16(std::ostream &output, uint16_t value) {
  const uint8_t bytes[] = {static_cast<uint8_t>(value),
                           static_cast<uint8_t>(value >> 8)};
  output.write(reinterpret_cast<const char *>(bytes), sizeof(bytes));
}

void write_u32(std::ostream &output, uint32_t value) {
  const uint8_t bytes[] = {
      static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8),
      static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24)};
  output.write(reinterpret_cast<const char *>(bytes), sizeof(bytes));
}

std::vector<float> read_mono_wav(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw std::runtime_error("could not open WAV file: " + path.string());

  char id[4] = {};
  input.read(id, 4);
  if (std::string(id, 4) != "RIFF")
    throw std::runtime_error("WAV is not RIFF: " + path.string());
  read_u32(input);
  input.read(id, 4);
  if (std::string(id, 4) != "WAVE")
    throw std::runtime_error("WAV is not WAVE: " + path.string());

  uint16_t format = 0;
  uint16_t channels = 0;
  uint32_t sample_rate = 0;
  uint16_t bits = 0;
  std::vector<uint8_t> data;
  while (input && data.empty()) {
    input.read(id, 4);
    if (!input)
      break;
    const uint32_t size = read_u32(input);
    if (std::string(id, 4) == "fmt ") {
      format = read_u16(input);
      channels = read_u16(input);
      sample_rate = read_u32(input);
      read_u32(input);
      read_u16(input);
      bits = read_u16(input);
      if (size > 16)
        input.seekg(size - 16, std::ios::cur);
    } else if (std::string(id, 4) == "data") {
      data.resize(size);
      input.read(reinterpret_cast<char *>(data.data()), size);
    } else {
      input.seekg(size, std::ios::cur);
    }
    if (size & 1)
      input.seekg(1, std::ios::cur);
  }

  if (channels != 1 || sample_rate != kSampleRate)
    throw std::runtime_error("WAV must be mono 48 kHz: " + path.string());

  std::vector<float> samples;
  if (format == 1 && bits == 16) {
    samples.resize(data.size() / 2);
    for (size_t i = 0; i < samples.size(); ++i) {
      const uint16_t raw = static_cast<uint16_t>(data[i * 2]) |
                           static_cast<uint16_t>(data[i * 2 + 1] << 8);
      samples[i] = static_cast<int16_t>(raw) / 32768.0f;
    }
  } else if (format == 3 && bits == 32) {
    samples.resize(data.size() / sizeof(float));
    std::memcpy(samples.data(), data.data(), samples.size() * sizeof(float));
  } else {
    throw std::runtime_error("WAV must be PCM16 or float32: " + path.string());
  }
  if (samples.empty())
    throw std::runtime_error("WAV contains no audio: " + path.string());
  return samples;
}

void write_float_wav(const std::filesystem::path &path,
                     const std::vector<float> &samples) {
  std::ofstream output(path, std::ios::binary);
  if (!output)
    throw std::runtime_error("could not create WAV file: " + path.string());
  const uint32_t data_size =
      static_cast<uint32_t>(samples.size() * sizeof(float));
  output.write("RIFF", 4);
  write_u32(output, 36 + data_size);
  output.write("WAVEfmt ", 8);
  write_u32(output, 16);
  write_u16(output, 3);
  write_u16(output, 1);
  write_u32(output, kSampleRate);
  write_u32(output, kSampleRate * sizeof(float));
  write_u16(output, sizeof(float));
  write_u16(output, 32);
  output.write("data", 4);
  write_u32(output, data_size);
  output.write(reinterpret_cast<const char *>(samples.data()), data_size);
}

double energy(const std::vector<float> &samples) {
  double result = 0.0;
  for (float sample : samples)
    result += static_cast<double>(sample) * sample;
  return result;
}

double rms(const std::vector<float> &samples) {
  return std::sqrt(energy(samples) / static_cast<double>(samples.size()));
}

double db_ratio(double numerator, double denominator) {
  constexpr double floor = 1e-20;
  return 20.0 *
         std::log10(std::max(numerator, floor) / std::max(denominator, floor));
}

double si_sdr(const std::vector<float> &estimate,
              const std::vector<float> &reference) {
  double estimate_mean = 0.0;
  double reference_mean = 0.0;
  for (size_t i = 0; i < estimate.size(); ++i) {
    estimate_mean += estimate[i];
    reference_mean += reference[i];
  }
  estimate_mean /= estimate.size();
  reference_mean /= reference.size();

  double dot = 0.0;
  double reference_energy = 0.0;
  for (size_t i = 0; i < estimate.size(); ++i) {
    const double ref = reference[i] - reference_mean;
    dot += (estimate[i] - estimate_mean) * ref;
    reference_energy += ref * ref;
  }
  if (reference_energy < 1e-20)
    return -std::numeric_limits<double>::infinity();
  const double scale = dot / reference_energy;
  double target_energy = 0.0;
  double residual_energy = 0.0;
  for (size_t i = 0; i < estimate.size(); ++i) {
    const double target = scale * (reference[i] - reference_mean);
    const double residual = (estimate[i] - estimate_mean) - target;
    target_energy += target * target;
    residual_energy += residual * residual;
  }
  return 10.0 * std::log10(std::max(target_energy, 1e-20) /
                           std::max(residual_energy, 1e-20));
}

std::vector<float> process(const std::string &model_path,
                           const std::vector<float> &input) {
  DpdfnetProcessor processor;
  processor.set_format(kSampleRate, 1);
  processor.replace_model(prepare_dpdfnet_model(model_path));
  DpdfnetControls controls;
  controls.attenuation_limit_db = 24.0;
  controls.wet_mix = 1.0;
  controls.output_gain = 1.0f;
  processor.set_controls(controls);

  const uint32_t packet_sizes[] = {137, 441, 480, 512, 960, 1024};
  std::vector<float> output;
  output.reserve(input.size());
  std::vector<float> padding(1024, 0.0f);
  size_t input_offset = 0;
  size_t packet_index = 0;
  uint64_t timestamp = kNanosecondsPerSecond;
  while (output.size() < input.size() && packet_index < 100000) {
    const uint32_t requested =
        packet_sizes[packet_index % std::size(packet_sizes)];
    const uint32_t frames = input_offset < input.size()
                                ? static_cast<uint32_t>(std::min<size_t>(
                                      requested, input.size() - input_offset))
                                : requested;
    DpdfnetAudioPacket packet;
    packet.frames = frames;
    packet.timestamp = timestamp;
    packet.data[0] = input_offset < input.size() ? input.data() + input_offset
                                                 : padding.data();
    const auto result = processor.process(packet);
    if (result.event != DpdfnetEvent::None)
      throw std::runtime_error(result.message.data());
    if (result.disposition == DpdfnetDisposition::Passthrough)
      throw std::runtime_error("quality processor unexpectedly passed through");
    if (result.disposition == DpdfnetDisposition::Processed)
      output.insert(output.end(), result.data[0],
                    result.data[0] + result.frames);
    if (input_offset < input.size())
      input_offset += frames;
    timestamp += static_cast<uint64_t>(static_cast<double>(frames) /
                                       kSampleRate * kNanosecondsPerSecond);
    ++packet_index;
  }
  if (output.size() < input.size())
    throw std::runtime_error("quality processor failed to drain");
  output.resize(input.size());
  return output;
}

struct SignalStats {
  float peak = 0.0f;
  double dc = 0.0;
  size_t clipped = 0;
  size_t nonfinite = 0;
};

SignalStats stats(const std::vector<float> &samples) {
  SignalStats result;
  for (float sample : samples) {
    if (!std::isfinite(sample)) {
      ++result.nonfinite;
      continue;
    }
    result.peak = std::max(result.peak, std::fabs(sample));
    result.dc += sample;
    if (std::fabs(sample) > 1.0f)
      ++result.clipped;
  }
  result.dc /= static_cast<double>(samples.size());
  return result;
}

void print_stats(const char *prefix, const std::vector<float> &samples) {
  const SignalStats value = stats(samples);
  std::cout << prefix << "_rms_dbfs=" << db_ratio(rms(samples), 1.0) << "\n"
            << prefix << "_peak=" << value.peak << "\n"
            << prefix << "_dc=" << value.dc << "\n"
            << prefix << "_clipped_samples=" << value.clipped << "\n"
            << prefix << "_nonfinite_samples=" << value.nonfinite << "\n";
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 6) {
    std::cerr << "usage: dpdfnet-quality-benchmark <model.onnx> <clean.wav> "
                 "<noise.wav> <snr-db> <output-directory>\n";
    return 2;
  }

  try {
    const std::string model_path = argv[1];
    std::vector<float> clean = read_mono_wav(argv[2]);
    std::vector<float> noise = read_mono_wav(argv[3]);
    const double snr_db = std::stod(argv[4]);
    const size_t frames = std::min(clean.size(), noise.size());
    clean.resize(frames);
    noise.resize(frames);

    const double clean_rms = rms(clean);
    const double noise_rms = rms(noise);
    if (clean_rms < 1e-8 || noise_rms < 1e-8)
      throw std::runtime_error("clean and noise WAVs must both contain signal");
    const double noise_scale =
        clean_rms / (noise_rms * std::pow(10.0, snr_db / 20.0));
    for (float &sample : noise)
      sample = static_cast<float>(sample * noise_scale);

    std::vector<float> mixture(frames);
    float peak = 0.0f;
    for (size_t i = 0; i < frames; ++i) {
      mixture[i] = clean[i] + noise[i];
      peak = std::max(peak, std::fabs(mixture[i]));
    }
    const float headroom = peak > 0.95f ? 0.95f / peak : 1.0f;
    for (size_t i = 0; i < frames; ++i) {
      clean[i] *= headroom;
      noise[i] *= headroom;
      mixture[i] *= headroom;
    }

    const auto enhanced_mixture = process(model_path, mixture);
    const auto enhanced_clean = process(model_path, clean);
    const auto enhanced_noise = process(model_path, noise);
    const std::filesystem::path output_directory(argv[5]);
    std::filesystem::create_directories(output_directory);
    write_float_wav(output_directory / "clean-reference.wav", clean);
    write_float_wav(output_directory / "scaled-noise.wav", noise);
    write_float_wav(output_directory / "mixture.wav", mixture);
    write_float_wav(output_directory / "enhanced-mixture.wav",
                    enhanced_mixture);
    write_float_wav(output_directory / "enhanced-clean.wav", enhanced_clean);
    write_float_wav(output_directory / "enhanced-noise.wav", enhanced_noise);

    DpdfnetModel model(model_path);
    std::cout << std::fixed << std::setprecision(6)
              << "model_name=" << model.name() << "\n"
              << "model_output_delay_hops=" << model.output_delay_hops() << "\n"
              << "model_startup_delay_compensated=true\n"
              << "frames=" << frames << "\n"
              << "sample_rate=" << kSampleRate << "\n"
              << "snr_db=" << snr_db << "\n"
              << "noise_scale=" << noise_scale << "\n"
              << "headroom_scale=" << headroom << "\n"
              << "settings=max_suppression_24db,wet_100pct,gain_0db\n"
              << "packet_pattern=137,441,480,512,960,1024\n"
              << "mixture_si_sdr_db=" << si_sdr(mixture, clean) << "\n"
              << "enhanced_si_sdr_db=" << si_sdr(enhanced_mixture, clean)
              << "\n"
              << "si_sdr_improvement_db="
              << si_sdr(enhanced_mixture, clean) - si_sdr(mixture, clean)
              << "\n"
              << "clean_si_sdr_db=" << si_sdr(enhanced_clean, clean) << "\n"
              << "clean_rms_delta_db="
              << db_ratio(rms(enhanced_clean), rms(clean)) << "\n"
              << "noise_attenuation_db="
              << db_ratio(rms(noise), rms(enhanced_noise)) << "\n";
    print_stats("enhanced_mixture", enhanced_mixture);
    print_stats("enhanced_clean", enhanced_clean);
    print_stats("enhanced_noise", enhanced_noise);
    return stats(enhanced_mixture).nonfinite == 0 &&
                   stats(enhanced_clean).nonfinite == 0 &&
                   stats(enhanced_noise).nonfinite == 0
               ? 0
               : 1;
  } catch (const std::exception &ex) {
    std::cerr << "error: " << ex.what() << "\n";
    return 1;
  }
}
