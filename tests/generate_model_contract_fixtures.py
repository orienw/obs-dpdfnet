# SPDX-License-Identifier: GPL-2.0-or-later

import argparse
from dataclasses import dataclass
from pathlib import Path

import onnx
from onnx import TensorProto, helper


@dataclass(frozen=True)
class Case:
    name: str
    options: dict


BASE_METADATA = {
    "profile": "contract-test",
    "sample_rate": "48000",
    "n_fft": "960",
    "hop_length": "480",
    "freq_bins": "481",
    "state_size": "1",
    "erb_norm_state_size": "1",
    "spec_norm_state_size": "0",
    "erb_norm_init": "0",
    "spec_norm_init": "",
    "output_delay_hops": "0",
}

CASES = (
    Case("valid_identity", {}),
    Case("valid_delayed_identity", {"delay_hops": 4}),
    Case("missing_output_delay", {"omit_delay": True}),
    Case("negative_output_delay", {"metadata": {"output_delay_hops": "-1"}}),
    Case("excessive_output_delay", {"metadata": {"output_delay_hops": "17"}}),
    Case(
        "valid_extreme_capacity",
        {
            "spec_shape": (1, 1, 4097, 2),
            "metadata": {
                "sample_rate": "8000",
                "n_fft": "8192",
                "hop_length": "4096",
                "freq_bins": "4097",
            },
        },
    ),
    Case(
        "valid_dynamic_arbitrary_names",
        {
            "spec_shape": (1, 1, "freq", 2),
            "state_shape": ("state",),
            "arbitrary_names": True,
        },
    ),
    Case("missing_input", {"input_count": 1}),
    Case("extra_input", {"input_count": 3}),
    Case("missing_output", {"output_count": 1}),
    Case("extra_output", {"output_count": 3}),
    Case(
        "wrong_type",
        {"element_type": TensorProto.DOUBLE},
    ),
    Case(
        "wrong_rank",
        {"spec_shape": (1, 481, 2)},
    ),
    Case(
        "wrong_dimension",
        {"spec_shape": (1, 1, 480, 2)},
    ),
    Case(
        "state_arithmetic_overflow",
        {
            "state_shape": (10,),
            "metadata": {
                "state_size": "10",
                "erb_norm_state_size": "2147483647",
                "spec_norm_state_size": "2147483647",
            },
        },
    ),
    Case(
        "empty_initializer_token",
        {
            "state_shape": (2,),
            "metadata": {
                "state_size": "2",
                "erb_norm_state_size": "2",
                "erb_norm_init": "0,",
            },
        },
    ),
    Case(
        "nan_initializer",
        {"metadata": {"erb_norm_init": "nan"}},
    ),
    Case(
        "infinite_initializer",
        {"metadata": {"erb_norm_init": "inf"}},
    ),
    Case(
        "oversized_initializer",
        {"metadata": {"erb_norm_init": "1000001"}},
    ),
    Case(
        "nonfinite_spectrum_output",
        {"nonfinite_spectrum": True},
    ),
    Case(
        "nonfinite_state_output",
        {"nonfinite_state": True},
    ),
    Case(
        "runtime_nonfinite_spectrum_output",
        {"nonfinite_nonzero_spectrum": True},
    ),
)


def make_value(name: str, element_type: int, shape: tuple) -> onnx.ValueInfoProto:
    return helper.make_tensor_value_info(name, element_type, list(shape))


def constant_node(name: str, output: str, element_type: int, shape: tuple):
    size = 1
    for dimension in shape:
        if not isinstance(dimension, int) or dimension < 1:
            raise ValueError("constant output shapes must be fixed and positive")
        size *= dimension
    value = float("nan") if name.startswith("nonfinite") else 0.0
    tensor = helper.make_tensor(name, element_type, list(shape), [value] * size)
    return helper.make_node("Constant", [], [output], value=tensor)


def make_fixture(path: Path, **options) -> None:
    spec_shape = options.get("spec_shape", (1, 1, 481, 2))
    delay_hops = options.get("delay_hops", 0)
    spectrum_size = 962
    state_shape = (
        (delay_hops * spectrum_size,)
        if delay_hops else options.get("state_shape", (1,))
    )
    element_type = options.get("element_type", TensorProto.FLOAT)
    input_count = options.get("input_count", 2)
    output_count = options.get("output_count", 2)

    if options.get("arbitrary_names"):
        spec_in, state_in = "spectral_data", "memory_in"
        spec_out, state_out = "enhanced_data", "memory_out"
    else:
        spec_in, state_in = "spec", "state_in"
        spec_out, state_out = "spec_e", "state_out"

    inputs = [make_value(spec_in, element_type, spec_shape)]
    if input_count >= 2:
        inputs.append(make_value(state_in, element_type, state_shape))
    if input_count >= 3:
        inputs.append(make_value("extra_input", element_type, (1,)))

    outputs = [make_value(spec_out, element_type, spec_shape)]
    if output_count >= 2:
        outputs.append(make_value(state_out, element_type, state_shape))
    if output_count >= 3:
        outputs.append(make_value("diagnostic", element_type, state_shape))

    if delay_hops:
        nodes = []
        for name, values in {
            "first": [0],
            "next": [spectrum_size],
            "last": [state_shape[0]],
            "spec_shape": list(spec_shape),
            "flat_shape": [spectrum_size],
        }.items():
            tensor = helper.make_tensor(name, TensorProto.INT64, [len(values)], values)
            nodes.append(helper.make_node("Constant", [], [name], value=tensor))
        nodes.extend([
            helper.make_node("Slice", [state_in, "first", "next"], ["oldest"]),
            helper.make_node("Reshape", ["oldest", "spec_shape"], [spec_out]),
            helper.make_node("Slice", [state_in, "next", "last"], ["history"]),
            helper.make_node("Reshape", [spec_in, "flat_shape"], ["current"]),
            helper.make_node("Concat", ["history", "current"], [state_out], axis=0),
        ])
    elif options.get("nonfinite_spectrum"):
        nodes = [
            constant_node(
                "nonfinite_spectrum", spec_out, element_type, spec_shape
            )
        ]
    elif options.get("nonfinite_nonzero_spectrum"):
        nodes = [
            helper.make_node("Abs", [spec_in], ["absolute_spectrum"]),
            constant_node("finite_zero", "zero_spectrum", element_type, spec_shape),
            helper.make_node(
                "Greater",
                ["absolute_spectrum", "zero_spectrum"],
                ["nonzero_spectrum"],
            ),
            constant_node(
                "nonfinite_runtime", "nan_spectrum", element_type, spec_shape
            ),
            helper.make_node(
                "Where",
                ["nonzero_spectrum", "nan_spectrum", spec_in],
                [spec_out],
            ),
        ]
    else:
        nodes = [helper.make_node("Identity", [spec_in], [spec_out])]

    if output_count >= 2 and not delay_hops:
        if options.get("nonfinite_state"):
            nodes.append(
                constant_node(
                    "nonfinite_state", state_out, element_type, state_shape
                )
            )
        elif input_count >= 2:
            nodes.append(helper.make_node("Identity", [state_in], [state_out]))
        else:
            nodes.append(
                constant_node("finite_state", state_out, element_type, state_shape)
            )
    if output_count >= 3:
        source = state_in if input_count >= 2 else state_out
        nodes.append(helper.make_node("Identity", [source], ["diagnostic"]))

    graph = helper.make_graph(nodes, path.stem, inputs, outputs)
    model = helper.make_model(
        graph, opset_imports=[helper.make_opsetid("", 13)]
    )
    model.ir_version = 9

    metadata = BASE_METADATA | options.get("metadata", {})
    if delay_hops:
        metadata |= {
            "state_size": str(state_shape[0]),
            "erb_norm_state_size": "0",
            "erb_norm_init": "",
            "output_delay_hops": str(delay_hops),
        }
    if options.get("omit_delay"):
        del metadata["output_delay_hops"]
    for key, value in metadata.items():
        entry = model.metadata_props.add()
        entry.key = key
        entry.value = value

    onnx.checker.check_model(model)
    onnx.save(model, path)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("output_directory", type=Path)
    args = parser.parse_args()

    args.output_directory.mkdir(parents=True, exist_ok=True)
    for path in args.output_directory.glob("*.onnx"):
        path.unlink()
    for case in CASES:
        path = args.output_directory / f"{case.name}.onnx"
        make_fixture(path, **case.options)
        print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
