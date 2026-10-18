#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
二期离线嵌入脚本：contents.jsonl → embeddings.f32 / embeddings.ids / meta.json

【在流水线中的位置】
  一期 exact_index_build 写出 contents.jsonl（每行 {"id","content"}）
  → 本脚本把每篇文档编成 384 维浮点向量
  → 二期 hnsw_build 读入这些文件建 HNSW

【关键 Python 语法（对照 C++）】
  - from __future__ import annotations : 允许类型注解里写 list[int] 等新写法
  - def f(...) -> int : 返回类型注解（类似 C++ 的尾随返回类型说明，运行期不强制）
  - Path : pathlib 路径对象，比纯字符串更方便拼路径 / 读写
  - argparse : 解析命令行参数（类似自己写 argv 循环，但更标准）
  - if __name__ == "__main__" : 仅当“直接运行本文件”时进入，被 import 时不执行

【依赖】
  pip install -r scripts/requirements.txt
  若 HuggingFace 拉模型失败：export HF_ENDPOINT=https://hf-mirror.com
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

import numpy as np
import torch
from torch import Tensor

# HuggingFace 上的多语言 MiniLM；输出维度 384
DEFAULT_MODEL = "sentence-transformers/paraphrase-multilingual-MiniLM-L12-v2"


def load_jsonl(path: Path) -> tuple[list[int], list[str]]:
    """读取一期导出的 jsonl，返回 (id 列表, 正文列表)，二者等长。"""
    ids: list[int] = []
    texts: list[str] = []
    with path.open("r", encoding="utf-8") as f:
        for line_no, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            obj = json.loads(line)
            if "id" not in obj or "content" not in obj:
                raise ValueError(f"{path}:{line_no}: missing id/content")
            ids.append(int(obj["id"]))
            texts.append(str(obj["content"]))
    return ids, texts


def l2_normalize(mat: np.ndarray) -> np.ndarray:
    """按行做 L2 归一化。归一化后用 L2 距离 ≈ 余弦距离。"""
    norms = np.linalg.norm(mat, axis=1, keepdims=True)
    norms = np.maximum(norms, 1e-12)
    return (mat / norms).astype(np.float32)


def mean_pooling(model_output: tuple, attention_mask: Tensor) -> Tensor:
    """transformers 后端：对 token 向量按 attention_mask 做平均池化，得到句向量。"""
    token_embeddings = model_output[0]
    mask = attention_mask.unsqueeze(-1).expand(token_embeddings.size()).float()
    summed = torch.sum(token_embeddings * mask, dim=1)
    counts = torch.clamp(mask.sum(dim=1), min=1e-9)
    return summed / counts


def encode_with_transformers(
    texts: list[str], model_name: str, batch_size: int, max_length: int
) -> np.ndarray:
    from transformers import AutoModel, AutoTokenizer

    print(f"loading transformers model {model_name} ...", flush=True)
    tokenizer = AutoTokenizer.from_pretrained(model_name)
    model = AutoModel.from_pretrained(model_name)
    model.eval()
    device = torch.device("cpu")
    model.to(device)

    chunks: list[np.ndarray] = []
    with torch.no_grad():
        for i in range(0, len(texts), batch_size):
            batch = texts[i : i + batch_size]
            encoded = tokenizer(
                batch,
                padding=True,
                truncation=True,
                max_length=max_length,
                return_tensors="pt",
            )
            encoded = {k: v.to(device) for k, v in encoded.items()}
            out = model(**encoded)
            emb = mean_pooling(out, encoded["attention_mask"])
            chunks.append(emb.cpu().numpy().astype(np.float32))
            if (i // batch_size) % 10 == 0:
                print(f"  encoded {min(i + batch_size, len(texts))}/{len(texts)}", flush=True)
    return np.vstack(chunks)


def encode_with_sbert(
    texts: list[str], model_name: str, batch_size: int, max_length: int
) -> np.ndarray:
    from sentence_transformers import SentenceTransformer

    print(f"loading sentence-transformers model {model_name} ...", flush=True)
    model = SentenceTransformer(model_name)
    model.max_seq_length = max_length
    emb = model.encode(
        texts,
        batch_size=batch_size,
        show_progress_bar=True,
        convert_to_numpy=True,
        normalize_embeddings=False,  # 我们自己做 L2，保证与 HNSW 约定一致
    )
    return np.asarray(emb, dtype=np.float32)


def main() -> int:
    # ArgumentParser：声明“本程序接受哪些命令行开关”
    # add_argument 的 help= 会出现在 python3 embed_sbert.py -h
    ap = argparse.ArgumentParser(description=__doc__)
    # required=True：缺少该参数则报错退出（类似 C++ 里 argc 检查）
    ap.add_argument("--input", required=True, help="一期产物 contents.jsonl 路径")
    ap.add_argument("--out-dir", required=True, help="输出目录（写 embeddings.*）")
    ap.add_argument("--model", default=DEFAULT_MODEL, help="HuggingFace 模型名")
    # type=int：把字符串参数转成 int；default= 为缺省值
    ap.add_argument("--batch-size", type=int, default=32, help="批大小，越大越吃内存")
    ap.add_argument("--max-seq-length", type=int, default=256, help="截断长度（token）")
    ap.add_argument(
        "--backend",
        choices=("auto", "sbert", "transformers"),  # 只能是这三个字符串之一
        default="auto",
        help="auto=优先 sentence-transformers，失败则用 transformers",
    )
    args = ap.parse_args()  # 真正解析 sys.argv

    in_path = Path(args.input)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    ids, texts = load_jsonl(in_path)
    if not ids:
        print("no documents in input", file=sys.stderr)
        return 1

    backend = args.backend
    if backend == "auto":
        try:
            import sentence_transformers  # noqa: F401

            backend = "sbert"
        except Exception as ex:
            print(f"sentence-transformers unavailable ({ex}); using transformers", flush=True)
            backend = "transformers"

    if backend == "sbert":
        emb = encode_with_sbert(texts, args.model, args.batch_size, args.max_seq_length)
    else:
        emb = encode_with_transformers(
            texts, args.model, args.batch_size, args.max_seq_length
        )

    emb = l2_normalize(emb)
    ids_arr = np.asarray(ids, dtype=np.int32)
    f32_path = out_dir / "embeddings.f32"
    ids_path = out_dir / "embeddings.ids"
    meta_path = out_dir / "embeddings.meta.json"

    # tofile：按 C 连续内存写出 float32，供 C++ 直接 mmap/read
    emb.tofile(f32_path)
    # struct.pack('<Ni', ...)：小端 int32 数组，与 C++ int32_t 一致
    with ids_path.open("wb") as f:
        f.write(struct.pack(f"<{ids_arr.size}i", *ids_arr.tolist()))

    meta = {
        "n": int(emb.shape[0]),
        "dim": int(emb.shape[1]),
        "model": args.model,
        "backend": backend,
        "normalized": True,
        "metric_hint": "l2_on_normalized_equals_cosine",
        "dtype": "float32",
        "ids_dtype": "int32",
        "endian": "little",
    }
    meta_path.write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")

    print(
        f"wrote {f32_path} ({emb.shape[0]} x {emb.shape[1]}), {ids_path}, {meta_path}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    # SystemExit(main())：把 main 的返回值当作进程退出码（0 成功，非 0 失败）
    raise SystemExit(main())
