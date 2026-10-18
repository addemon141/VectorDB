#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
查询时嵌入 sidecar（常驻 HTTP 服务）。

【为什么要单独进程】
  SBERT 在 Python/PyTorch 里最方便；C++ 的 query_server 通过 HTTP POST /embed
  把 query 文本换成向量，再去本地 HNSW 检索。模型只加载一次，避免每请求冷启动。

【协议】
  POST http://host:port/embed
  Body JSON: {"text":"..."}
  响应 JSON: {"dim":384,"values":[...]}  （已 L2 归一化）

【关键 Python 语法】
  - BaseHTTPRequestHandler : 标准库里的简易 HTTP 服务器基类（类似自己写 socket，但封装了 HTTP）
  - do_POST : 处理 POST；方法名是约定，框架会回调
  - serve_forever() : 阻塞死循环接请求（类似 while(true) accept）
"""

from __future__ import annotations

import argparse
import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

import numpy as np

DEFAULT_MODEL = "sentence-transformers/paraphrase-multilingual-MiniLM-L12-v2"


def l2_normalize(vec: np.ndarray) -> np.ndarray:
    n = float(np.linalg.norm(vec))
    if n < 1e-12:
        return vec.astype(np.float32)
    return (vec / n).astype(np.float32)


class ModelHolder:
    """把模型加载与 encode 封在一起，启动时构造一次，之后复用。"""

    def __init__(self, model_name: str, backend: str) -> None:
        self.backend = backend
        self.model_name = model_name
        self._sbert = None
        self._tok = None
        self._model = None
        if backend == "sbert":
            from sentence_transformers import SentenceTransformer

            self._sbert = SentenceTransformer(model_name)
        else:
            from transformers import AutoModel, AutoTokenizer
            import torch

            self._torch = torch
            self._tok = AutoTokenizer.from_pretrained(model_name)
            self._model = AutoModel.from_pretrained(model_name)
            self._model.eval()

    def encode(self, text: str) -> list[float]:
        if self._sbert is not None:
            emb = self._sbert.encode([text], convert_to_numpy=True, normalize_embeddings=False)[0]
            return l2_normalize(np.asarray(emb, dtype=np.float32)).tolist()

        encoded = self._tok(
            [text], padding=True, truncation=True, max_length=256, return_tensors="pt"
        )
        with self._torch.no_grad():
            out = self._model(**encoded)
            mask = encoded["attention_mask"].unsqueeze(-1).expand(out[0].size()).float()
            summed = self._torch.sum(out[0] * mask, dim=1)
            counts = self._torch.clamp(mask.sum(dim=1), min=1e-9)
            emb = (summed / counts)[0].cpu().numpy().astype(np.float32)
        return l2_normalize(emb).tolist()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    # --host / --port：监听地址。默认本机回环，只给本机 query_server 用。
    ap.add_argument("--host", default="127.0.0.1", help="绑定网卡地址")
    ap.add_argument("--port", type=int, default=50053, help="监听端口")
    ap.add_argument("--model", default=DEFAULT_MODEL, help="与离线 embed_sbert 相同的模型")
    ap.add_argument(
        "--backend",
        choices=("auto", "sbert", "transformers"),
        default="auto",
        help="推理后端选择",
    )
    args = ap.parse_args()

    backend = args.backend
    if backend == "auto":
        try:
            import sentence_transformers  # noqa: F401

            backend = "sbert"
        except Exception:
            backend = "transformers"

    print(f"loading embed model ({backend}) {args.model} ...", flush=True)
    holder = ModelHolder(args.model, backend)
    print(f"embed_server on http://{args.host}:{args.port}/embed", flush=True)

    class Handler(BaseHTTPRequestHandler):
        def do_POST(self):  # noqa: N802  # 框架要求的方法名，不能改成 do_post
            if self.path.rstrip("/") != "/embed":
                self.send_response(404)
                self.end_headers()
                return
            # Content-Length：HTTP 头里声明 body 字节数
            length = int(self.headers.get("Content-Length", "0"))
            raw = self.rfile.read(length)
            try:
                obj = json.loads(raw.decode("utf-8"))
                text = str(obj.get("text", ""))
                values = holder.encode(text)
                body = json.dumps({"dim": len(values), "values": values}).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            except Exception as ex:  # noqa: BLE001
                err = json.dumps({"error": str(ex)}).encode("utf-8")
                self.send_response(500)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(err)))
                self.end_headers()
                self.wfile.write(err)

        def log_message(self, fmt: str, *a) -> None:
            sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % a))

    # 元组 (host, port) 传给 HTTPServer；serve_forever 阻塞直到进程被杀
    HTTPServer((args.host, args.port), Handler).serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
