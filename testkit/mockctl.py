#!/usr/bin/env python3
"""mock 媒体控制面（测试资产，实施计划 P0.4）。

实现《媒体协议契约 v1》的控制面语义 + 故障注入开关 + 统计口径：
  POST /v1/media/init        三种回答：instant / challenge / upload（幂等键 client_task_id）
  POST /v1/media/part_urls   按批领取预签名 PUT URL（短时效）
  PUT  /store/{uid}/{pno}    数据面：分片字节（验 sig/exp/CRC32C）
  POST /v1/media/complete    封箱：全量 SHA-256 校验（幂等）
  POST /v1/media/challenge   秒传持有证明：HMAC(nonce, 区间字节)
  GET  /v1/media/{id}        状态机：PROCESSING → AVAILABLE / REJECTED
  POST /v1/media/dl_url      下载凭证（AVAILABLE 才发放）
  GET  /store/obj/{id}       数据面：对象下载（支持 Range）
  POST /admin/faults | GET /admin/stats | POST /admin/reset

生产服务端必须通过与本 mock 相同的契约测试集（tests/e2e_main.cpp）。
"""

import argparse
import hashlib
import hmac as hmac_mod
import json
import math
import os
import re
import secrets
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SECRET = b"mockctl-secret"

# ---------------- CRC32C（Castagnoli，与内核对拍）----------------
_CRC_TABLE = []
for _i in range(256):
    _c = _i
    for _ in range(8):
        _c = (0x82F63B78 ^ (_c >> 1)) if (_c & 1) else (_c >> 1)
    _CRC_TABLE.append(_c)


def crc32c(data: bytes) -> int:
    c = 0xFFFFFFFF
    for b in data:
        c = _CRC_TABLE[(c ^ b) & 0xFF] ^ (c >> 8)
    return c ^ 0xFFFFFFFF


class State:
    def __init__(self, root: str):
        self.lock = threading.RLock()
        self.root = root
        os.makedirs(os.path.join(root, "sessions"), exist_ok=True)
        os.makedirs(os.path.join(root, "objects"), exist_ok=True)
        self.reset()

    def reset(self):
        with self.lock:
            self.sessions = {}        # upload_id -> dict(size, part_size, nparts, sha256, parts:set, task_id)
            self.by_task = {}         # client_task_id -> upload_id | ("media", media_id)
            self.dedup = {}           # (sha256,size) -> media_id
            self.media = {}           # media_id -> dict(path,size,sha256,available_at,rejected)
            self.challenges = {}      # challenge_id -> dict(media_id, nonce, ranges)
            self.faults = {
                "fail_part_puts": 0,      # 接下来 N 次分片 PUT 返回 500
                "http_500_next": 0,       # 接下来 N 次控制面 POST 返回 500
                "expire_urls": False,     # 发放已过期 URL（测换新）
                "url_ttl_s": 900,
                "slow_ms": 0,
                "bandwidth_bps": 0,       # 数据面带宽整形（分片体按此速率慢读，>0 生效）
                "drop_connections": 0,    # 接下来 N 次分片 PUT 半途断连（TCP RST 语义）
                "skip_crc": False,        # 关 CRC 校验（bench 拆分"回归基线/传输极限"口径）
                "challenge_on_dedup": True,
                "processing_delay_ms": 0,
                "reject_next_complete": False,
                "forget_parts_on_complete": 0,  # complete 时"忘掉"N 个分片 → 409 回补路径
            }
            self.stats = {
                "init_count": 0,
                "complete_count": 0,
                "init_suggests": [],      # 每次 init 的 suggest_part_size（自适应闭环断言口径）
                "part_put_ok": {},        # "uid/pno" -> count（零重传断言的数据源）
                "part_put_total": 0,
                "bytes_received": 0,
                "challenge_count": 0,
                "dl_url_count": 0,
            }


S: State = None  # main() 里初始化


def sign(path: str, exp: int) -> str:
    return hmac_mod.new(SECRET, f"{path}|{exp}".encode(), hashlib.sha256).hexdigest()[:16]


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):  # 静默访问日志
        pass

    # ---------- 基础 ----------
    def _body(self) -> bytes:
        n = int(self.headers.get("Content-Length", 0))
        return self.rfile.read(n) if n > 0 else b""

    def _send(self, code: int, payload, extra=None):
        data = payload if isinstance(payload, bytes) else json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/octet-stream" if isinstance(payload, bytes) else "application/json")
        self.send_header("Content-Length", str(len(data)))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def _slow(self):
        ms = S.faults["slow_ms"]
        if ms > 0:
            time.sleep(ms / 1000.0)

    def _maybe_500(self) -> bool:
        with S.lock:
            if S.faults["http_500_next"] > 0:
                S.faults["http_500_next"] -= 1
                return True
        return False

    def _issue_url(self, path: str) -> str:
        ttl = S.faults["url_ttl_s"]
        exp = int(time.time()) + (-10 if S.faults["expire_urls"] else int(ttl))
        host = self.headers.get("Host", "127.0.0.1")
        return f"http://{host}{path}?exp={exp}&sig={sign(path, exp)}"

    def _check_sig(self, path: str, q: dict) -> bool:
        try:
            exp = int(q.get("exp", ["0"])[0])
            sig = q.get("sig", [""])[0]
        except ValueError:
            return False
        if exp < time.time():
            return False
        return hmac_mod.compare_digest(sig, sign(path, exp))

    # ---------- 路由 ----------
    # 铁律：任何 handler 必须先消费完请求体再响应——HTTP/1.1 keep-alive 下残留字节会
    # 污染复用连接，下一个请求被解析成 "{}POST ..." → 501（真实踩坑，测试断言逼出来的）。
    def do_POST(self):
        self._slow()
        raw = self._body()
        try:
            if self.path == "/v1/media/init":
                return self.h_init(raw)
            if self.path == "/v1/media/part_urls":
                return self.h_part_urls(raw)
            if self.path == "/v1/media/complete":
                return self.h_complete(raw)
            if self.path == "/v1/media/challenge":
                return self.h_challenge(raw)
            if self.path == "/v1/media/dl_url":
                return self.h_dl_url(raw)
            if self.path == "/admin/faults":
                with S.lock:
                    S.faults.update(json.loads(raw or b"{}"))
                return self._send(200, {"ok": True})
            if self.path == "/admin/reset":
                S.reset()
                return self._send(200, {"ok": True})
            return self._send(404, {"error": "no route"})
        except Exception as e:  # mock 不允许静默挂死
            return self._send(500, {"error": f"mock exception: {e!r}"})

    def _read_body_shaped(self):
        """数据面慢读：响应在读完+睡完后才发出 → 客户端测得的分片耗时被真实拉长（EWMA 输入）。"""
        n = int(self.headers.get("Content-Length", 0))
        if n <= 0:
            return b""
        bps = S.faults.get("bandwidth_bps", 0)
        if bps <= 0:
            return self.rfile.read(n)
        chunks, remaining = [], n
        while remaining > 0:
            take = min(16384, remaining)
            chunks.append(self.rfile.read(take))
            remaining -= take
            time.sleep(take / bps)
        return b"".join(chunks)

    def do_PUT(self):
        self._slow()
        with S.lock:
            drop = S.faults.get("drop_connections", 0)
            if drop > 0 and self.path.startswith("/store/"):
                S.faults["drop_connections"] = drop - 1
                # 读一小段后硬断连（不回响应）：客户端视角 = 网络错误 → 走分片重试路径
                self.rfile.read(min(4096, int(self.headers.get("Content-Length", 0))))
                raise ConnectionAbortedError("injected connection drop")
        body = self._read_body_shaped()  # 先消费（见 do_POST 注释）
        required = S.faults.get("require_data_authorization")
        if required and self.headers.get("Authorization") != required:
            return self._send(403, {"error": "data authorization required"})
        m = re.match(r"^/store/([^/]+)/(\d+)\?(.*)$", self.path)
        if not m:
            return self._send(404, {"error": "bad store path"})
        uid, pno = m.group(1), int(m.group(2))
        from urllib.parse import parse_qs
        q = parse_qs(m.group(3))
        path_no_q = f"/store/{uid}/{pno}"
        if not self._check_sig(path_no_q, q):
            return self._send(403, {"error": "bad or expired sig"})
        with S.lock:
            if S.faults["fail_part_puts"] > 0:
                S.faults["fail_part_puts"] -= 1
                return self._send(500, {"error": "injected fault"})
            sess = S.sessions.get(uid)
        if sess is None:
            return self._send(404, {"error": "no session"})
        want_crc = self.headers.get("x-part-crc32c", "")
        if want_crc and not S.faults.get("skip_crc") and int(want_crc, 16) != crc32c(body):
            return self._send(400, {"error": "crc mismatch"})
        # 分片大小校验：流式会话只约束上限（末片长度未知）；定长会话精确校验（末片除外）
        if sess.get("streaming"):
            if len(body) == 0 or len(body) > sess["part_size"] or pno >= 100000:
                return self._send(400, {"error": f"bad streaming part size {len(body)}"})
        else:
            expect = sess["part_size"] if pno < sess["nparts"] - 1 else sess["size"] - sess["part_size"] * (sess["nparts"] - 1)
            if pno >= sess["nparts"] or len(body) != expect:
                return self._send(400, {"error": f"bad part size {len(body)} want {expect}"})
        d = os.path.join(S.root, "sessions", uid)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, str(pno)), "wb") as f:
            f.write(body)
        with S.lock:
            sess["parts"].add(pno)
            key = f"{uid}/{pno}"
            S.stats["part_put_ok"][key] = S.stats["part_put_ok"].get(key, 0) + 1
            S.stats["part_put_total"] += 1
            S.stats["bytes_received"] += len(body)
        return self._send(200, {"ok": True})

    def do_GET(self):
        self._slow()
        if self.path == "/test/redirect":
            return self._send(302, {}, {"Location": S.faults.get("test_redirect_to", "/test/sink")})
        if self.path == "/test/sink":
            with S.lock:
                S.stats["test_sink_hits"] = S.stats.get("test_sink_hits", 0) + 1
            return self._send(200, {})
        if self.path == "/admin/stats":
            with S.lock:
                return self._send(200, {"stats": S.stats, "faults": S.faults,
                                        "sessions": {k: sorted(v["parts"]) for k, v in S.sessions.items()}})
        m = re.match(r"^/v1/media/([^/?]+)$", self.path)
        if m:
            return self.h_media_status(m.group(1))
        m = re.match(r"^/store/obj/([^/?]+)\?(.*)$", self.path)
        if m:
            return self.h_obj_get(m.group(1), m.group(2))
        return self._send(404, {"error": "no route"})

    # ---------- 控制面 ----------
    def h_init(self, raw):
        if self._maybe_500():
            return self._send(500, {"error": "injected"})
        req = json.loads(raw)
        task_id = req["client_task_id"]
        sha, size = req.get("sha256", ""), int(req["size"])
        with S.lock:
            S.stats["init_count"] += 1
            S.stats["init_suggests"].append(int(req.get("suggest_part_size", 0)))
            # 幂等：同 task_id 拿回同一单（含最新 received_parts）
            prior = S.by_task.get(task_id)
            if prior and isinstance(prior, tuple):
                return self._send(200, {"result": "instant", "media_id": prior[1]})
            if prior and prior in S.sessions:
                sess = S.sessions[prior]
                return self._send(200, {"result": "upload", "upload_id": prior,
                                        "part_size": sess["part_size"], "nparts": sess["nparts"],
                                        "received_parts": sorted(sess["parts"])})
            # 流式开单（语音边录边传）：无 hash/size，不走秒传，complete 补报
            if req.get("streaming"):
                part_size = int(req.get("suggest_part_size", 256 * 1024))
                part_size = max(16 * 1024, min(part_size, 8 * 1024 * 1024))
                uid = "u_" + secrets.token_hex(8)
                S.sessions[uid] = {"size": None, "part_size": part_size, "nparts": None,
                                   "sha256": "", "parts": set(), "task_id": task_id,
                                   "streaming": True}
                S.by_task[task_id] = uid
                return self._send(200, {"result": "upload", "upload_id": uid,
                                        "part_size": part_size, "nparts": 0,
                                        "received_parts": []})
            # 秒传：租户内 (sha256,size) 命中
            hit = S.dedup.get((sha, size)) if sha else None
            if hit and not S.media[hit]["rejected"]:
                if S.faults["challenge_on_dedup"]:
                    cid, nonce = "c_" + secrets.token_hex(8), secrets.token_hex(16)
                    obj = S.media[hit]
                    ranges = []
                    for _ in range(2):
                        ln = min(4096, obj["size"])
                        off = secrets.randbelow(max(1, obj["size"] - ln + 1))
                        ranges.append([off, ln])
                    S.challenges[cid] = {"media_id": hit, "nonce": nonce, "ranges": ranges, "task_id": task_id}
                    S.stats["challenge_count"] += 1
                    return self._send(200, {"result": "challenge", "challenge_id": cid,
                                            "nonce": nonce, "ranges": ranges})
                S.by_task[task_id] = ("media", hit)
                return self._send(200, {"result": "instant", "media_id": hit})
            # 新上传会话
            part_size = int(req.get("suggest_part_size", 4 * 1024 * 1024))
            part_size = max(64 * 1024, min(part_size, 8 * 1024 * 1024))
            nparts = max(1, math.ceil(size / part_size))
            uid = "u_" + secrets.token_hex(8)
            S.sessions[uid] = {"size": size, "part_size": part_size, "nparts": nparts,
                               "sha256": sha, "parts": set(), "task_id": task_id}
            S.by_task[task_id] = uid
            return self._send(200, {"result": "upload", "upload_id": uid, "part_size": part_size,
                                    "nparts": nparts, "received_parts": []})

    def h_part_urls(self, raw):
        if self._maybe_500():
            return self._send(500, {"error": "injected"})
        req = json.loads(raw)
        uid = req["upload_id"]
        with S.lock:
            if uid not in S.sessions:
                return self._send(404, {"error": "no session"})
        urls = {str(p): self._issue_url(f"/store/{uid}/{p}") for p in req["parts"]}
        return self._send(200, {"urls": urls})

    def h_complete(self, raw):
        if self._maybe_500():
            return self._send(500, {"error": "injected"})
        req = json.loads(raw)
        uid, sha = req["upload_id"], req["sha256"]
        with S.lock:
            S.stats["complete_count"] += 1
            sess = S.sessions.get(uid)
            if sess is None:
                return self._send(404, {"error": "no session"})
            if sess.get("streaming"):
                # 流式补报：complete 携带最终 size/nparts，服务端校验连续性与总量
                sess["nparts"] = int(req.get("nparts", 0))
                sess["size"] = int(req.get("size", 0))
                if sess["nparts"] <= 0 or sess["size"] <= 0:
                    return self._send(400, {"error": "streaming complete needs size/nparts"})
                total = 0
                for p in range(sess["nparts"]):
                    fp = os.path.join(S.root, "sessions", uid, str(p))
                    if p in sess["parts"] and os.path.exists(fp):
                        total += os.path.getsize(fp)
                if total != sess["size"]:
                    return self._send(400, {"error": f"streaming size mismatch {total} != {sess['size']}"})
            n_forget = S.faults.get("forget_parts_on_complete", 0)
            if n_forget > 0 and sess["parts"]:
                for d in sorted(sess["parts"])[:n_forget]:
                    sess["parts"].discard(d)
                S.faults["forget_parts_on_complete"] = 0
            missing = [p for p in range(sess["nparts"]) if p not in sess["parts"]]
            if missing:
                return self._send(409, {"error": "missing parts", "missing": missing})
        # 组装 + 全量校验
        h = hashlib.sha256()
        obj_path = os.path.join(S.root, "objects", "obj_" + uid)
        with open(obj_path, "wb") as out:
            for p in range(sess["nparts"]):
                with open(os.path.join(S.root, "sessions", uid, str(p)), "rb") as f:
                    data = f.read()
                h.update(data)
                out.write(data)
        if h.hexdigest() != sha:
            os.remove(obj_path)
            return self._send(400, {"error": "sha256 mismatch"})
        with S.lock:
            media_id = "m_" + sha[:32]
            final = os.path.join(S.root, "objects", media_id)
            os.replace(obj_path, final)
            rejected = S.faults["reject_next_complete"]
            S.faults["reject_next_complete"] = False
            S.media[media_id] = {"path": final, "size": sess["size"], "sha256": sha,
                                 "available_at": time.time() + S.faults["processing_delay_ms"] / 1000.0,
                                 "rejected": rejected}
            S.dedup[(sha, sess["size"])] = media_id
            S.by_task[sess["task_id"]] = ("media", media_id)
        return self._send(200, {"media_id": media_id, "status": "PROCESSING"})

    def h_challenge(self, raw):
        req = json.loads(raw)
        cid, proof = req["challenge_id"], req.get("proof", "")
        with S.lock:
            ch = S.challenges.pop(cid, None)
        if ch is None:
            return self._send(404, {"error": "no challenge"})
        obj = S.media[ch["media_id"]]
        blob = b""
        with open(obj["path"], "rb") as f:
            for off, ln in ch["ranges"]:
                f.seek(off)
                blob += f.read(ln)
        want = hmac_mod.new(ch["nonce"].encode(), blob, hashlib.sha256).hexdigest()
        if not hmac_mod.compare_digest(want, proof):
            return self._send(403, {"error": "bad proof"})
        with S.lock:
            S.by_task[ch["task_id"]] = ("media", ch["media_id"])
        return self._send(200, {"result": "instant", "media_id": ch["media_id"]})

    def h_media_status(self, media_id):
        with S.lock:
            m = S.media.get(media_id)
            if m is None:
                return self._send(404, {"error": "no media"})
            if m["rejected"]:
                return self._send(200, {"status": "REJECTED"})
            st = "AVAILABLE" if time.time() >= m["available_at"] else "PROCESSING"
            return self._send(200, {"status": st, "size": m["size"], "sha256": m["sha256"]})

    def h_dl_url(self, raw):
        if self._maybe_500():
            return self._send(500, {"error": "injected"})
        req = json.loads(raw)
        media_id = req["media_id"]
        with S.lock:
            m = S.media.get(media_id)
            if m is None:
                return self._send(404, {"error": "no media"})
            if m["rejected"]:
                return self._send(403, {"error": "REJECTED"})
            if time.time() < m["available_at"]:
                return self._send(409, {"error": "PROCESSING"})
            S.stats["dl_url_count"] += 1
        return self._send(200, {"url": self._issue_url(f"/store/obj/{media_id}"),
                                "size": m["size"], "sha256": m["sha256"]})

    # ---------- 数据面下载 ----------
    def h_obj_get(self, media_id, query):
        required = S.faults.get("require_data_authorization")
        if required and self.headers.get("Authorization") != required:
            return self._send(403, {"error": "data authorization required"})
        from urllib.parse import parse_qs
        q = parse_qs(query)
        if not self._check_sig(f"/store/obj/{media_id}", q):
            return self._send(403, {"error": "bad or expired sig"})
        with S.lock:
            m = S.media.get(media_id)
        if m is None:
            return self._send(404, {"error": "no media"})
        with open(m["path"], "rb") as f:
            data = f.read()
        rng = self.headers.get("Range")
        if rng:
            mm = re.match(r"bytes=(\d+)-(\d+)", rng)
            if not mm:
                return self._send(416, {"error": "bad range"})
            a, b = int(mm.group(1)), int(mm.group(2))
            if a > b or b >= len(data):
                return self._send(416, {"error": "range out of bounds"})
            chunk = data[a : b + 1]
            return self._send(206, chunk, {"Content-Range": f"bytes {a}-{b}/{len(data)}"})
        return self._send(200, data)


class QuietHTTPServer(ThreadingHTTPServer):
    def server_bind(self):
        # 跳过 HTTPServer.server_bind() 里的 socket.getfqdn()：
        # 它做反向 DNS 查询，DNS 不通的环境下挂 30s+（实测 35s），测试基建不允许这种启动抖动。
        import socketserver
        socketserver.TCPServer.server_bind(self)
        self.server_name = "mockctl"
        self.server_port = self.server_address[1]


def main():
    global S
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--root", required=True)
    args = ap.parse_args()
    os.makedirs(args.root, exist_ok=True)
    S = State(args.root)
    srv = QuietHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"mockctl listening on 127.0.0.1:{args.port}", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()

