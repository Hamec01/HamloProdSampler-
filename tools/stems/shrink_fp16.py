"""Lossless size halving of an fp32 ONNX model whose weights are exactly representable in fp16.

HTDemucs is published with fp16 weights, so every large fp32 initializer that round-trips through
fp16 unchanged is stored as fp16 and followed by a Cast to fp32. ONNX Runtime folds the Casts at
load time, and the computation stays fp32 and bit-identical. Initializers that do not round-trip
(constants folded during export) stay fp32.

Usage:  python -I shrink_fp16.py <in.onnx> <out.onnx>
"""
import sys

import numpy as np
import onnx
from onnx import helper, numpy_helper, TensorProto


def shrink(src, dst):
    m = onnx.load(src)
    g = m.graph
    keep, casts, moved, kept = [], [], 0, 0
    for init in g.initializer:
        a = numpy_helper.to_array(init)
        if a.dtype == np.float32 and a.size >= 256 and np.array_equal(a.astype(np.float16).astype(np.float32), a):
            half = numpy_helper.from_array(a.astype(np.float16), init.name + "__fp16")
            keep.append(half)
            casts.append(helper.make_node("Cast", [half.name], [init.name], to=TensorProto.FLOAT, name=init.name + "__cast"))
            moved += a.size
        else:
            keep.append(init)
            kept += a.size
    del g.initializer[:]
    g.initializer.extend(keep)
    nodes = list(g.node)
    del g.node[:]
    g.node.extend(casts + nodes)
    onnx.checker.check_model(m)
    onnx.save(m, dst)
    print("fp16 weights:", moved, " fp32 kept:", kept, flush=True)


if __name__ == "__main__":
    shrink(sys.argv[1], sys.argv[2])
