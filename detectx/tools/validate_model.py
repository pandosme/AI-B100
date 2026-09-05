#!/usr/bin/env python3
"""Validate the supported DetectX YOLOv5 TFLite contract and emit metadata."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
import tensorflow as tf


MAX_LABEL_LENGTH = 60


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_labels(path: Path) -> list[str]:
    labels = path.read_text(encoding="utf-8").splitlines()
    if not labels or any(not label or len(label) > MAX_LABEL_LENGTH for label in labels):
        raise ValueError("labels must be non-empty and at most 60 characters")
    if len(labels) != len(set(labels)):
        raise ValueError("labels must be unique")
    return labels


def tensor_type(dtype: np.dtype) -> str:
    mapping = {np.dtype(np.uint8): "uint8", np.dtype(np.int8): "int8", np.dtype(np.float32): "float32"}
    if np.dtype(dtype) not in mapping:
        raise ValueError(f"unsupported tensor dtype: {dtype}")
    return mapping[np.dtype(dtype)]


def validate(model_path: Path, labels_path: Path, description: str, targets: list[str]) -> dict[str, object]:
    interpreter = tf.lite.Interpreter(model_path=str(model_path))
    input_details = interpreter.get_input_details()
    output_details = interpreter.get_output_details()
    if len(input_details) != 1 or len(output_details) != 1:
        raise ValueError("model must have exactly one input and one output")
    input_tensor = input_details[0]
    output_tensor = output_details[0]
    input_shape = [int(value) for value in input_tensor["shape"]]
    output_shape = [int(value) for value in output_tensor["shape"]]
    if len(input_shape) != 4 or input_shape[0] != 1 or input_shape[3] != 3 or input_shape[1] % 8 or input_shape[2] % 8:
        raise ValueError("input must be uint8 NHWC [1,H,W,3] with H/W divisible by 8")
    if tensor_type(input_tensor["dtype"]) != "uint8":
        raise ValueError("input tensor must be uint8")
    if len(output_shape) != 3 or output_shape[0] != 1 or output_shape[1] <= 0 or output_shape[2] <= 5:
        raise ValueError("output must use YOLOv5 [1,N,5+C] layout")
    labels = read_labels(labels_path)
    if output_shape[2] - 5 != len(labels):
        raise ValueError("label count must equal output classes")
    output_dtype = tensor_type(output_tensor["dtype"])
    scale, zero_point = output_tensor["quantization"]
    quantization = output_tensor["quantization_parameters"]
    if output_dtype != "float32":
        scales = quantization["scales"]
        if len(scales) != 1 or float(scale) <= 0:
            raise ValueError("quantized output must use valid per-tensor quantization")
    else:
        scale, zero_point = 1.0, 0
    if "artpec8" in targets:
        per_channel_tensors = [
            detail["name"]
            for detail in interpreter.get_tensor_details()
            if len(detail["quantization_parameters"]["scales"]) > 1
        ]
        if per_channel_tensors:
            raise ValueError(
                "ARTPEC-8 requires per-tensor quantization; per-channel tensors found: "
                + ", ".join(per_channel_tensors[:5])
            )
    return {
        "schemaVersion": 1,
        "format": "yolov5-1-n-5+c",
        "description": description,
        "targets": targets,
        "input": {"shape": input_shape, "dtype": "uint8"},
        "output": {
            "shape": output_shape,
            "dtype": output_dtype,
            "scale": float(scale),
            "zeroPoint": int(zero_point),
        },
        "modelSha256": sha256(model_path),
        "labelsSha256": sha256(labels_path),
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("model", type=Path)
    parser.add_argument("labels", type=Path)
    parser.add_argument("metadata", type=Path)
    parser.add_argument("--description", default="")
    parser.add_argument("--targets", nargs="+", choices=("artpec8", "artpec9"), required=True)
    args = parser.parse_args()
    metadata = validate(args.model, args.labels, args.description, args.targets)
    args.metadata.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()