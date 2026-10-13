#!/usr/bin/env python3
# ============================================================================
# web/server.py  --  the NEMO-C web frontend's server
#
# A small HTTP server (Python standard library only, like plot.py it sits
# outside the compiler) that lets a browser drive the real ./nemoc binary:
#
#   GET  /                     the single-page frontend (web/index.html)
#   GET  /api/status           is ./nemoc built? is Graphviz installed?
#   GET  /api/files            the example programs (benchmarks/, tests/, lab/)
#   GET  /api/file?path=...    one example program's source
#   POST /api/run              write the editor's source to build/web/<name>.nmc,
#                              run  ./nemoc <file> <flags for one operation>,
#                              return stdout, stderr, exit code and any files
#                              the run generated (Graphviz .dot, .pim, host .c)
#   POST /api/make             run  make | make run-all | make test
#   GET  /api/results          results/results.csv, decisions.md, transformations.md
#   GET  /results/<chart>.png  the matplotlib charts, if they exist
#
# Nothing is ever passed to a shell: every operation maps to a fixed argument
# list, and the only free-form inputs (a kernel name, a register count) are
# checked against a pattern first.  The server binds to localhost.
#
#   make web                 (or:  python3 web/server.py [--port 8080])
#   then open  http://localhost:8080  in a browser on the same machine
# ============================================================================
import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEB = os.path.join(ROOT, "web")
NEMOC = os.path.join(ROOT, "nemoc")
WORK = os.path.join(ROOT, "build", "web")
DOTDIR = os.path.join(ROOT, "build", "dot")

RUN_TIMEOUT = 300          # seconds for one nemoc run (--simulate takes up to ~20 s)
MAKE_TIMEOUT = 1800        # make test takes ~8 minutes
MAX_SOURCE = 200 * 1024    # bytes

# nemoc writes to fixed paths under build/, so runs are serialised
LOCK = threading.Lock()

# operation id -> the nemoc flags that perform it ("{arg}" = the kernel name)
ACTIONS = {
    "tokens":          ["--dump", "tokens"],
    "ast":             ["--dump", "ast"],
    "check":           ["--check"],
    "ir":              ["--dump", "ir"],
    "cfg":             ["--emit-dot", "cfg"],
    "opt":             ["--dump", "opt"],
    "constfold":       ["--dump", "constfold"],
    "cse":             ["--dump", "cse"],
    "dce":             ["--dump", "dce"],
    "indvar":          ["--dump", "indvar"],
    "strength":        ["--dump", "strength"],
    "affine":          ["--dump", "affine"],
    "deps":            ["--dump", "deps"],
    "depsdot":         ["--emit-dot", "deps"],
    "decisions":       ["--decisions"],
    "explain":         ["--explain", "{arg}"],
    "transformations": ["--transformations"],
    "partition":       ["--partition"],
    "coherence":       ["--coherence"],
    "codegen":         ["--codegen"],
    "regalloc":        ["--dump", "regalloc"],
    "interference":    ["--emit-dot", "interference"],
    "simulate":        ["--simulate"],
}

MAKE_TARGETS = {"all": ["make"], "run-all": ["make", "run-all"], "test": ["make", "test"]}

NAME_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_#]{0,63}$")


def example_files():
    """Relative paths of every example program the frontend may open."""
    out = []
    for d in ("benchmarks", "tests", "lab"):
        for p in sorted(glob.glob(os.path.join(ROOT, d, "*.nmc"))):
            out.append(d + "/" + os.path.basename(p))
    return out


def read_text(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def dot_to_svg(text):
    """Render Graphviz server-side when `dot` is installed; else the browser does it."""
    if not shutil.which("dot"):
        return None
    try:
        r = subprocess.run(["dot", "-Tsvg"], input=text, capture_output=True, text=True, timeout=30)
        return r.stdout if r.returncode == 0 else None
    except (OSError, subprocess.TimeoutExpired):
        return None


def generated_files(since):
    """Files a nemoc run produced: Graphviz graphs, DPU programs, host C."""
    found = []
    patterns = [(os.path.join(DOTDIR, "*.dot"), "dot"),
                (os.path.join(ROOT, "build", "*_dpu.pim"), "pim"),
                (os.path.join(ROOT, "build", "*_host.c"), "c")]
    for pattern, kind in patterns:
        for p in sorted(glob.glob(pattern)):
            if os.path.getmtime(p) >= since:
                text = read_text(p)
                entry = {"name": os.path.relpath(p, ROOT).replace(os.sep, "/"), "kind": kind, "text": text}
                if kind == "dot":
                    entry["svg"] = dot_to_svg(text)
                found.append(entry)
    return found


def run_nemoc(req):
    action = req.get("action", "")
    if action not in ACTIONS:
        return 400, {"error": "unknown operation '%s'" % action}
    source = req.get("source", "")
    if not isinstance(source, str) or len(source.encode("utf-8")) > MAX_SOURCE:
        return 400, {"error": "source missing or larger than %d bytes" % MAX_SOURCE}
    name = re.sub(r"[^A-Za-z0-9_]", "_", str(req.get("name", "program")))[:48] or "program"
    if not os.path.exists(NEMOC):
        return 409, {"error": "./nemoc is not built yet: press Build (or run make)"}

    args = []
    for a in ACTIONS[action]:
        if a == "{arg}":
            arg = str(req.get("arg", ""))
            if not NAME_RE.match(arg):
                return 400, {"error": "choose a kernel or nest name to explain"}
            args.append(arg)
        else:
            args.append(a)
    opts = req.get("options", {}) or {}
    if opts.get("noOpt"):
        args.append("--no-opt")
    if opts.get("dist") in ("block", "cyclic", "bcyclic"):
        args += ["--dist", opts["dist"]]
    regs = opts.get("regs")
    if regs not in (None, ""):
        try:
            k = int(regs)
        except (TypeError, ValueError):
            return 400, {"error": "registers must be a number"}
        if not 3 <= k <= 64:
            return 400, {"error": "registers must be between 3 and 64"}
        args += ["--regs", str(k)]

    os.makedirs(WORK, exist_ok=True)
    os.makedirs(DOTDIR, exist_ok=True)
    rel = "build/web/%s.nmc" % name
    with open(os.path.join(ROOT, rel), "w", encoding="utf-8", newline="\n") as f:
        f.write(source)

    cmd = ["./nemoc", rel] + args
    with LOCK:
        start = time.time() - 1.0
        t0 = time.time()
        try:
            r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=RUN_TIMEOUT)
            code, out, err = r.returncode, r.stdout, r.stderr
        except subprocess.TimeoutExpired:
            code, out, err = -1, "", "nemoc: stopped after %d seconds" % RUN_TIMEOUT
        files = generated_files(start)
        ms = int((time.time() - t0) * 1000)
    return 200, {"cmd": " ".join(cmd), "exit": code, "stdout": out, "stderr": err,
                 "files": files, "ms": ms, "source_path": rel}


def run_make(req):
    target = req.get("target", "")
    if target not in MAKE_TARGETS:
        return 400, {"error": "unknown make target"}
    cmd = MAKE_TARGETS[target]
    with LOCK:
        t0 = time.time()
        try:
            r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=MAKE_TIMEOUT)
            code, out = r.returncode, r.stdout + r.stderr
        except subprocess.TimeoutExpired:
            code, out = -1, "make: stopped after %d seconds" % MAKE_TIMEOUT
        ms = int((time.time() - t0) * 1000)
    return 200, {"cmd": " ".join(cmd), "exit": code, "output": out, "ms": ms}


def results():
    def opt(rel):
        p = os.path.join(ROOT, rel)
        return read_text(p) if os.path.exists(p) else ""
    charts = [c for c in ("cycles", "bytes", "energy")
              if os.path.exists(os.path.join(ROOT, "results", c + ".png"))]
    return {"csv": opt("results/results.csv"), "decisions": opt("results/decisions.md"),
            "transformations": opt("results/transformations.md"), "charts": charts}


STATIC_TYPES = {".html": "text/html; charset=utf-8", ".css": "text/css; charset=utf-8",
                ".js": "application/javascript; charset=utf-8", ".png": "image/png",
                ".svg": "image/svg+xml"}


class Handler(BaseHTTPRequestHandler):
    server_version = "nemoc-web/1.0"

    def log_message(self, fmt, *args):
        sys.stderr.write("[web] %s\n" % (fmt % args))

    def send_json(self, code, obj):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def send_file(self, path):
        ext = os.path.splitext(path)[1].lower()
        if ext not in STATIC_TYPES or not os.path.isfile(path):
            self.send_json(404, {"error": "not found"})
            return
        with open(path, "rb") as f:
            body = f.read()
        self.send_response(200)
        self.send_header("Content-Type", STATIC_TYPES[ext])
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(url.query)
        if url.path == "/":
            self.send_file(os.path.join(WEB, "index.html"))
        elif url.path in ("/app.js", "/style.css"):
            self.send_file(os.path.join(WEB, url.path[1:]))
        elif url.path == "/api/status":
            self.send_json(200, {"built": os.path.exists(NEMOC), "graphviz": bool(shutil.which("dot"))})
        elif url.path == "/api/files":
            self.send_json(200, {"files": example_files()})
        elif url.path == "/api/file":
            rel = (q.get("path") or [""])[0]
            if rel not in example_files():
                self.send_json(404, {"error": "no such example"})
            else:
                self.send_json(200, {"path": rel, "source": read_text(os.path.join(ROOT, rel))})
        elif url.path == "/api/results":
            self.send_json(200, results())
        elif re.match(r"^/results/(cycles|bytes|energy)\.png$", url.path):
            self.send_file(os.path.join(ROOT, url.path[1:]))
        else:
            self.send_json(404, {"error": "not found"})

    def do_POST(self):
        url = urllib.parse.urlparse(self.path)
        try:
            n = int(self.headers.get("Content-Length", "0"))
            req = json.loads(self.rfile.read(n).decode("utf-8") or "{}")
        except (ValueError, UnicodeDecodeError):
            self.send_json(400, {"error": "bad JSON"})
            return
        if url.path == "/api/run":
            self.send_json(*run_nemoc(req))
        elif url.path == "/api/make":
            self.send_json(*run_make(req))
        else:
            self.send_json(404, {"error": "not found"})


def main():
    ap = argparse.ArgumentParser(description="NEMO-C web frontend")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--host", default="127.0.0.1")
    a = ap.parse_args()
    httpd = ThreadingHTTPServer((a.host, a.port), Handler)
    print("NEMO-C web frontend: open  http://localhost:%d  (Ctrl+C to stop)" % a.port, flush=True)
    if not os.path.exists(NEMOC):
        print("note: ./nemoc is not built yet; the page has a Build button", flush=True)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
