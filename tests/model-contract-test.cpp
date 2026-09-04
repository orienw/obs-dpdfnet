// SPDX-License-Identifier: GPL-2.0-or-later

#include "../src/dpdfnet-model.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

struct ContractCase {
  const char *file_name;
  const char *expected_error;
};

constexpr ContractCase kCases[] = {
    {"valid_identity.onnx", nullptr},
    {"valid_delayed_identity.onnx", nullptr},
    {"missing_output_delay.onnx", "must declare output_delay_hops"},
    {"negative_output_delay.onnx", "output_delay_hops is out of supported range"},
    {"excessive_output_delay.onnx", "output_delay_hops is out of supported range"},
    {"valid_extreme_capacity.onnx", nullptr},
    {"valid_dynamic_arbitrary_names.onnx", nullptr},
    {"missing_input.onnx",
     "DPDFNet ONNX model must expose exactly two inputs and two outputs"},
    {"extra_input.onnx",
     "DPDFNet ONNX model must expose exactly two inputs and two outputs"},
    {"missing_output.onnx",
     "DPDFNet ONNX model must expose exactly two inputs and two outputs"},
    {"extra_output.onnx",
     "DPDFNet ONNX model must expose exactly two inputs and two outputs"},
    {"wrong_type.onnx", "DPDFNet spectrum input must use float32 values"},
    {"wrong_rank.onnx", "DPDFNet spectrum input has an incompatible rank"},
    {"wrong_dimension.onnx",
     "DPDFNet spectrum input has an incompatible shape"},
    {"state_arithmetic_overflow.onnx",
     "DPDFNet model metadata has inconsistent state dimensions"},
    {"empty_initializer_token.onnx",
     "DPDFNet metadata key contains an empty float: erb_norm_init"},
    {"nan_initializer.onnx",
     "DPDFNet metadata key contains an invalid float: erb_norm_init"},
    {"infinite_initializer.onnx",
     "DPDFNet metadata key contains an invalid float: erb_norm_init"},
    {"oversized_initializer.onnx",
     "DPDFNet metadata key contains an invalid float: erb_norm_init"},
    {"nonfinite_spectrum_output.onnx",
     "DPDFNet model produced non-finite spectrum output"},
    {"nonfinite_state_output.onnx",
     "DPDFNet model produced non-finite state output"},
    {"runtime_nonfinite_spectrum_output.onnx",
     "DPDFNet model produced non-finite spectrum output"},
};

void run_model(const std::filesystem::path &path) {
  DpdfnetModel model(path);
  std::fill_n(model.input_spectrum(), model.spectrum_size(), 0.125f);
  model.enhance();

  const float *output = model.output_spectrum();
  if (!std::all_of(output, output + model.spectrum_size(),
                   [](float value) { return std::isfinite(value); })) {
    throw std::runtime_error("model returned non-finite output");
  }
}

bool run_case(const std::filesystem::path &directory,
              const ContractCase &test_case) {
  const auto path = directory / test_case.file_name;
  try {
    run_model(path);
    if (test_case.expected_error) {
      std::cerr << "FAIL " << test_case.file_name
                << ": model unexpectedly succeeded\n";
      return false;
    }
  } catch (const std::exception &ex) {
    if (!test_case.expected_error ||
        std::string(ex.what()).find(test_case.expected_error) ==
            std::string::npos) {
      std::cerr << "FAIL " << test_case.file_name << ": " << ex.what() << "\n";
      return false;
    }
  }

  std::cout << "PASS " << test_case.file_name << "\n";
  return true;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "usage: dpdfnet-model-contract-test <fixture-directory> "
                 "[bundled-model.onnx ...]\n";
    return 2;
  }

  bool passed = true;
  const std::filesystem::path fixture_directory(argv[1]);
  for (const auto &test_case : kCases)
    passed = run_case(fixture_directory, test_case) && passed;

  for (int i = 2; i < argc; ++i) {
    try {
      run_model(argv[i]);
      std::cout << "PASS " << std::filesystem::path(argv[i]).filename().string()
                << "\n";
    } catch (const std::exception &ex) {
      std::cerr << "FAIL " << argv[i] << ": " << ex.what() << "\n";
      passed = false;
    }
  }

  return passed ? 0 : 1;
}
