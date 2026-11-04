#!/usr/bin/env python3
"""Import a validated wakeword.py bundle as aligned, const firmware data."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import tempfile


def prepare(bundle, output):
    import numpy as np
    import tensorflow as tf
    model_path = bundle / "model_int8.tflite"
    meta = json.loads((bundle / "metadata.json").read_text(encoding="utf-8"))
    data = model_path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if meta.get("format_version") != 1 or meta.get("frontend") != "periodic_hann_logmel_timepool_v1":
        raise ValueError("仅支持此前 wakeword.py 导出的 periodic_hann_logmel_timepool_v1 模型包")
    if digest != meta.get("model_sha256") or meta.get("classes") != ["negative", "positive"]:
        raise ValueError("模型 SHA256 / 类别顺序与 metadata.json 不一致")
    c = meta["config"]
    integer_keys = ["sample_rate", "frame_length", "frame_step", "fft_length", "mel_bins", "time_bins"]
    for key in integer_keys:
        if type(c[key]) is not int or c[key] <= 0:
            raise ValueError(f"{key} 必须是正整数")
    for key in ["window_seconds", "hop_seconds", "fmin", "fmax", "db_floor", "min_rms", "cooldown_seconds", "hysteresis"]:
        if not math.isfinite(c[key]):
            raise ValueError(f"{key} 必须是有限数值")
    size = round(c["sample_rate"] * c["window_seconds"])
    hop = round(c["sample_rate"] * c["hop_seconds"])
    fft, length, step = c["fft_length"], c["frame_length"], c["frame_step"]
    if c["sample_rate"] != 16000 or not length <= size <= 160000 or not 0 < hop <= size:
        raise ValueError("板端要求 16 kHz、窗口 <=10 秒、0 < hop <= window")
    if not length <= fft <= 4096 or fft & (fft - 1):
        raise ValueError("板端 FFT 必须为 2 的幂，且 frame_length <= FFT <= 4096")
    frames = (size - length) // step + 1
    if not 1 <= c["time_bins"] <= frames or not 1 <= c["mel_bins"] <= 128:
        raise ValueError("time_bins / mel_bins 超出板端前端支持范围")
    if not 0 <= c["fmin"] < c["fmax"] <= 8000 or not -200 <= c["db_floor"] < 0:
        raise ValueError("mel 频率或 db_floor 不合法")
    threshold = meta["threshold"]
    if not math.isfinite(threshold) or not 0 < threshold <= 1 or c["min_rms"] < 0 or c["cooldown_seconds"] < 0 or not 0 <= c["hysteresis"] < 1:
        raise ValueError("threshold / min_rms / cooldown / hysteresis 不合法")
    interpreter = tf.lite.Interpreter(model_path=str(model_path), num_threads=1,
        experimental_op_resolver_type=tf.lite.experimental.OpResolverType.BUILTIN_WITHOUT_DEFAULT_DELEGATES)
    interpreter.allocate_tensors()
    ins, outs = interpreter.get_input_details(), interpreter.get_output_details()
    shape = [1, c["time_bins"], c["mel_bins"], 1]
    if len(ins) != 1 or len(outs) != 1:
        raise ValueError("必须只有一个输入和一个输出")
    inp, out = ins[0], outs[0]
    if inp["dtype"] != np.int8 or out["dtype"] != np.int8 or inp["shape"].tolist() != shape or out["shape"].tolist() != [1, 2]:
        raise ValueError("必须是 INT8 输入 [1,time_bins,mel_bins,1]，INT8 输出 [1,2]")
    for tensor in [inp, out]:
        q = tensor["quantization_parameters"]
        if len(q["scales"]) != 1 or not np.isfinite(q["scales"][0]) or q["scales"][0] <= 0:
            raise ValueError("输入/输出必须是有效的 per-tensor 量化")
    # No delegates: all graph operators must be supported by the Micro resolver.
    operations = {op["op_name"] for op in interpreter._get_ops_details()}
    if not operations <= {"RESHAPE", "FULLY_CONNECTED", "SOFTMAX"}:
        raise ValueError(f"当前板端 resolver 不支持这些算子: {operations}")
    if any(t["dtype"] in (np.float32, np.float64) for t in interpreter.get_tensor_details()):
        raise ValueError("需要全整数模型，不能含浮点中间张量")
    hann = np.hanning(length + 1)[:-1].astype(np.float32)
    h2m = lambda x: 2595.0 * np.log10(1.0 + np.asarray(x) / 700.0)
    points = np.linspace(h2m(c["fmin"]), h2m(c["fmax"]), c["mel_bins"] + 2)
    freq = h2m(np.fft.rfftfreq(fft, 1 / 16000))[:, None]
    weights = np.maximum(0, np.minimum((freq - points[:-2]) / (points[1:-1] - points[:-2]),
                                      (points[2:] - freq) / (points[2:] - points[1:-1]))).astype(np.float32)
    starts, lengths, offsets, packed = [], [], [], []
    for m in range(c["mel_bins"]):
        nz = np.flatnonzero(weights[:, m])
        if len(nz) == 0:
            raise ValueError("存在空 mel 滤波器")
        a, b = int(nz[0]), int(nz[-1]) + 1
        starts.append(a); lengths.append(b-a); offsets.append(len(packed))
        packed.extend(weights[a:b, m])
    edges = np.linspace(0, frames, c["time_bins"] + 1, dtype=np.int32)
    golden = np.zeros(shape, dtype=np.float32)
    ref = bundle / "frontend_reference.npz"
    if ref.exists():
        with np.load(ref, allow_pickle=False) as reference:
            golden = np.asarray(reference["features"], dtype=np.float32).reshape(shape)
        if not np.all(np.isfinite(golden)):
            raise ValueError("frontend_reference.npz 含非有限值")
    test_input = np.clip(np.rint(golden / inp["quantization"][0] + inp["quantization"][1]), -128, 127).astype(np.int8)
    interpreter.set_tensor(inp["index"], test_input)
    interpreter.invoke()
    test_output = interpreter.get_tensor(out["index"]).reshape(-1)
    def scalar(x):
        value = format(float(np.float32(x)), ".9g")
        return value + ("f" if "." in value or "e" in value.lower() else ".0f")
    def array(name, dtype, values):
        values = list(values)
        tokens = [scalar(v) if dtype == "float" else str(int(v)) for v in values]
        rows = ["  " + ", ".join(tokens[i:i+12]) for i in range(0, len(tokens), 12)]
        return f"const {dtype} {name}[] = {{\n" + ",\n".join(rows) + "\n};\n"
    text = '// Generated by prepare_wakeword_model.py. Model and frontend are one unit.\n#include "robot_wakeword_model.h"\n'
    text += 'alignas(16) ' + array("g_robot_wakeword_model", "unsigned char", data)
    text += f"const size_t g_robot_wakeword_model_size = {len(data)};\n"
    text += f'const char g_robot_wakeword_model_sha256[] = "{digest}";\n'
    for name, dtype, values in [
        ("hann", "float", hann),
        ("twiddle_real", "float", np.cos(-2*np.pi*np.arange(fft//2)/fft)),
        ("twiddle_imag", "float", np.sin(-2*np.pi*np.arange(fft//2)/fft)),
        ("mel_start", "uint16_t", starts), ("mel_length", "uint16_t", lengths),
        ("mel_offset", "uint32_t", offsets), ("time_edges", "uint32_t", edges),
        ("mel_weights", "float", packed)]:
        text += "static " + array(name, dtype, values)
    text += "const struct robot_wakeword_model_config_s g_robot_wakeword_model_config = {\n"
    text += f"  16000, {size}, {hop}, {length}, {step}, {fft}, {c['mel_bins']}, {c['time_bins']},\n"
    text += "  " + ", ".join(scalar(x) for x in [c["db_floor"], 1 / hann.sum(), threshold, c["min_rms"], c["cooldown_seconds"], c["hysteresis"]]) + ",\n"
    text += "  hann, twiddle_real, twiddle_imag, mel_start, mel_length, mel_offset, time_edges, mel_weights\n};\n"
    text += array("g_robot_wakeword_test_input", "int8_t", test_input.ravel())
    text += f"const size_t g_robot_wakeword_test_input_size = {test_input.size};\n"
    text += array("g_robot_wakeword_test_output", "int8_t", test_output)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=output.parent, delete=False) as f:
            temporary = Path(f.name); f.write(text)
        os.replace(temporary, output)
    finally:
        if temporary is not None and temporary.exists():
            temporary.unlink()
    print(f"已导入模型: {output.resolve()}")
    print(f"SHA256={digest}\n输入={shape}, INT8; window={size/16000:g}s hop={hop/16000:g}s threshold={threshold:g}")
    print(f"算子={sorted(operations)}; golden INT8 输出={test_output.tolist()}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", required=True, type=Path, help="含 model_int8.tflite、metadata.json 的训练输出目录")
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parent.parent / "robot_wakeword_model_data.cc")
    args = parser.parse_args()
    try:
        prepare(args.bundle, args.output)
    except (OSError, ValueError, KeyError, TypeError, ImportError, RuntimeError) as e:
        parser.exit(2, f"导入失败: {e}\n请在已安装原训练包 requirements.txt 的 Python 环境运行。\n")


if __name__ == "__main__":
    main()
