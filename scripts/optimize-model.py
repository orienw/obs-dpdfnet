# SPDX-License-Identifier: GPL-2.0-or-later
"""Export the pinned, single-frame DPDFNet models with frequency-major DPRNN stacks."""

import argparse
import hashlib
import json
from pathlib import Path

import onnx
from onnx import TensorProto, helper, numpy_helper


def optimize(model):
    graph = model.graph
    nodes = {node.name: node for node in graph.node}
    initializers = {tensor.name: tensor for tensor in graph.initializer}
    after = {}
    shapes = {}

    def shape(*dims):
        if dims not in shapes:
            name = "dpdfnet_shape_" + "_".join(map(str, dims))
            graph.initializer.append(helper.make_tensor(name, TensorProto.INT64, [len(dims)], dims))
            shapes[dims] = name
        return shapes[dims]

    for branch in ("erb", "df"):
        stack = f"/model/enc/dprnn_{branch}/blocks."
        blocks = [node.name.removesuffix("/intra_gru/GRU") for node in graph.node
                  if node.name.startswith(stack) and node.name.endswith("/intra_gru/GRU")]
        features = None
        for block in blocks:
            def node(suffix):
                return nodes[f"{block}/{suffix}"]

            matrix_shape = node("Reshape_2").input[1]
            bins, channels = map(int, numpy_helper.to_array(initializers[matrix_shape]))
            if features is None:
                # The convolution supplies [1, C, 1, F]. Keep its entry transpose
                # and share the resulting [F, C] matrix with the residual path.
                entry = node("Reshape")
                entry.input[1] = matrix_shape
                features = entry.output[0]

            gru = node("intra_gru/GRU")
            to_sequence = node("intra_gru/Transpose")
            to_sequence.CopyFrom(helper.make_node(
                "Reshape", [features, shape(bins, 1, channels)], to_sequence.output,
                name=to_sequence.name))
            # GRU output is [F, 2 directions, 1 batch, C]. With batch one
            # each row already contains the forward and backward features.
            directions = node("intra_gru/Reshape")
            directions.input[:] = [gru.output[0], shape(bins, 2 * channels)]
            node("fc_intra/MatMul").input[0] = directions.output[0]

            intra = node("Add")
            intra.input[:] = [features, node("ln_intra/LayerNormalization").output[0]]
            node("inter_gru/grucell/Gemm").input[0] = intra.output[0]
            inter = node("Add_1")
            inter.input[:] = [intra.output[0], node("ln_inter/LayerNormalization").output[0]]
            features = inter.output[0]

        # Restore the layout expected by the rest of the encoder just once.
        output = inter.output[0]
        inter.output[0] = block + "/features"
        expanded = block + "/expanded"
        after[inter.name] = [
            helper.make_node("Reshape", [inter.output[0], shape(1, bins, 1, channels)],
                             [expanded], name=block + "/expand_features"),
            helper.make_node("Transpose", [expanded], [output],
                             perm=[0, 3, 2, 1], name=block + "/restore_layout"),
        ]

    # Drop the superseded layout operations, their constants and old shape annotations.
    ordered = []
    for node in graph.node:
        ordered.append(node)
        ordered.extend(after.get(node.name, ()))
    needed = {value.name for value in graph.output}
    live = []
    for node in reversed(ordered):
        if needed.intersection(node.output):
            live.append(node)
            needed.update(node.input)
    del graph.node[:]
    graph.node.extend(reversed(live))
    used = [tensor for tensor in graph.initializer if tensor.name in needed]
    del graph.initializer[:]
    graph.initializer.extend(used)
    del graph.value_info[:]
    model.doc_string += "\nModified by obs-dpdfnet: frequency-major DPRNN layout for single-frame inference."
    # The original exports predate the output-delay metadata used by the plugin.
    model.metadata_props.add(key="output_delay_hops", value="4")
    onnx.checker.check_model(model, full_check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="original Ceva ONNX file")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    manifest = json.loads((Path(__file__).resolve().parents[1] / "models/manifest.json").read_text())
    source = args.source.read_bytes()
    digest = hashlib.sha256(source).hexdigest()
    if not any(entry["upstreamSha256"] == digest for entry in manifest["models"]):
        parser.error("input does not match either pinned upstream model")
    model = onnx.load_model_from_string(source)
    optimize(model)
    args.output.write_bytes(model.SerializeToString(deterministic=True))


if __name__ == "__main__":
    main()
