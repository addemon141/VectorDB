#!/usr/bin/env python3
"""冒烟测试用的假 embedding 服务（只依赖标准库）。

接口与 embed_server.py 相同：
  POST http://host:port/embed   请求 {"text": "..."}   响应 {"dim": D, "values": [...]}

向量 = 文本字符 bigram 哈希到 D 维后 L2 归一化。确定性：同一文本在任何节点得到同一向量；
字面相近的文本距离也近，足以验证向量召回链路，但没有语义能力。
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def embed(text: str, dim: int) -> list[float]:
    vec = [0.0] * dim
    chars = [c for c in text if not c.isspace()]
    grams = [a + b for a, b in zip(chars, chars[1:])] or chars
    for g in grams:
        h = int.from_bytes(hashlib.md5(g.encode("utf-8")).digest()[:4], "little")
        vec[h % dim] += 1.0
    norm = math.sqrt(sum(v * v for v in vec)) or 1.0
    return [v / norm for v in vec]


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=50053)
    ap.add_argument("--dim", type=int, default=384)
    args = ap.parse_args()

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            if self.path.rstrip("/") != "/embed":
                self.send_error(404)
                return
            try:
                n = int(self.headers.get("Content-Length", "0"))
                text = json.loads(self.rfile.read(n).decode("utf-8"))["text"]
                body = json.dumps({"dim": args.dim, "values": embed(text, args.dim)}).encode()
                self.send_response(200)
            except Exception as ex:  # noqa: BLE001
                body = json.dumps({"error": str(ex)}).encode()
                self.send_response(400)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    print(f"fake_embed_server on http://{args.host}:{args.port}/embed dim={args.dim}", flush=True)
    ThreadingHTTPServer((args.host, args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
