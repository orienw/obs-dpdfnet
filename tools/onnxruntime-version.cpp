// SPDX-License-Identifier: GPL-2.0-or-later

#include <onnxruntime_cxx_api.h>

#include <iostream>

int main() {
  std::cout << Ort::GetVersionString() << '\n';
  return 0;
}
