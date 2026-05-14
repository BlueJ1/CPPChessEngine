#!/usr/bin/env python3
"""Evaluate a FEN queue with Lc0 through UCI and write JSONL labels."""

from __future__ import annotations

import argparse
from collections import deque
from dataclasses import dataclass
import json
import queue
import re
import subprocess
import sys
import threading
import time
from datetime import datetime, timezone
from pathlib import Path

MOVE_STAT_RE = re.compile(
    r"^(?P<move>\S+)\s+\(\s*(?P<policy_index>\d+)\s*\)\s+"
    r"N:\s*(?P<visits>\d+)\s+\(\+\s*(?P<collisions>\d+)\)\s+"
    r"\(P:\s*(?P<policy>[-.\d]+)%\)\s+"
    r"\(WL:\s*(?P<wl>[-.\d]+)\)\s+"
    r"\(D:\s*(?P<draw>[-.\d]+)\)\s+"
    r"\(M:\s*(?P<moves_left>[-.\d]+)\)\s+"
    r"\(Q:\s*(?P<q>[-.\d]+)\)\s+"
    r"(?:\(U:\s*(?P<u>[-.\d]+)\)\s+)?"
    r"(?:\(S:\s*(?P<s>[-.\d]+)\)\s+)?"
    r"\(V:\s*(?P<v>[-.\d]+)\)"
)


@dataclass(frozen=True)
class WorkerConfig:
    name: str
    backend: str
    backend_opts: str
    threads: int


@dataclass(frozen=True)
class Task:
    ordinal: int
    input_index: int
    fen: str


@dataclass(frozen=True)
class WorkerResult:
    ordinal: int
    line: str


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Evaluate FENs with Lc0.")
    parser.add_argument("--lc0", type=Path, required=True)
    parser.add_argument("--weights", type=Path, required=True)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--nodes", type=int, default=128)
    parser.add_argument("--threads", type=int, default=1)
    parser.add_argument("--backend", default="")
    parser.add_argument("--backend-opts", default="")
    parser.add_argument("--workers", type=int, default=1)
    parser.add_argument("--cpu-workers", type=int, default=0)
    parser.add_argument("--cpu-backend", default="blas")
    parser.add_argument("--cpu-backend-opts", default="")
    parser.add_argument("--cpu-threads", type=int)
    parser.add_argument("--gpu-workers", type=int, default=0)
    parser.add_argument("--gpu-backend", default="metal")
    parser.add_argument("--gpu-backend-opts", action="append", default=[])
    parser.add_argument("--gpu-threads", type=int)
    parser.add_argument("--queue-size", type=int, default=128)
    parser.add_argument("--minibatch-size", type=int)
    parser.add_argument("--max-prefetch", type=int)
    parser.add_argument("--nncache-size", type=int)
    parser.add_argument("--max-concurrent-searchers", type=int)
    parser.add_argument("--multipv", type=int, default=1)
    parser.add_argument("--score-type", default="WDL_mu")
    parser.add_argument("--no-verbose-move-stats", action="store_true")
    parser.add_argument("--progress-interval", type=int, default=100)
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--resume", action="store_true")
    return parser.parse_args()


def read_until(proc: subprocess.Popen[str], prefix: str) -> list[str]:
    lines: list[str] = []
    while True:
        line = proc.stdout.readline()
        if line == "":
            raise RuntimeError(f"lc0 exited before {prefix!r}")
        line = line.rstrip("\n")
        lines.append(line)
        if line.startswith(prefix):
            return lines


def send(proc: subprocess.Popen[str], command: str) -> None:
    assert proc.stdin is not None
    proc.stdin.write(command + "\n")
    proc.stdin.flush()


def parse_info(line: str) -> dict[str, object]:
    tokens = line.split()
    data: dict[str, object] = {"raw": line}
    i = 1
    while i < len(tokens):
        key = tokens[i]
        if key in {"depth", "seldepth", "multipv", "nodes", "nps", "hashfull", "tbhits"}:
            if i + 1 < len(tokens):
                try:
                    data[key] = int(tokens[i + 1])
                except ValueError:
                    data[key] = tokens[i + 1]
                i += 2
                continue
        if key == "score" and i + 2 < len(tokens):
            data["score_type"] = tokens[i + 1]
            try:
                data["score"] = int(tokens[i + 2])
            except ValueError:
                data["score"] = tokens[i + 2]
            i += 3
            continue
        if key == "wdl" and i + 3 < len(tokens):
            try:
                data["wdl"] = [int(tokens[i + 1]), int(tokens[i + 2]), int(tokens[i + 3])]
            except ValueError:
                data["wdl"] = tokens[i + 1 : i + 4]
            i += 4
            continue
        if key == "pv" and i + 1 < len(tokens):
            data["pv"] = tokens[i + 1 :]
            break
        if key == "string" and i + 1 < len(tokens):
            data.setdefault("strings", []).append(" ".join(tokens[i + 1 :]))
            break
        i += 1
    return data


def maybe_float(value: str) -> float | None:
    try:
        return float(value)
    except ValueError:
        return None


def parse_move_stat(line: str) -> dict[str, object] | None:
    if not line.startswith("info string "):
        return None
    payload = " ".join(line[len("info string ") :].split())
    if payload.startswith("node "):
        return None
    match = MOVE_STAT_RE.match(payload)
    if not match:
        return None
    groups = match.groupdict()
    out: dict[str, object] = {
        "move": groups["move"],
        "policy_index": int(groups["policy_index"]),
        "visits": int(groups["visits"]),
        "collisions": int(groups["collisions"]),
        "policy": float(groups["policy"]) / 100.0,
        "wl": maybe_float(groups["wl"]),
        "draw": maybe_float(groups["draw"]),
        "moves_left": maybe_float(groups["moves_left"]),
        "q": maybe_float(groups["q"]),
        "v": maybe_float(groups["v"]),
    }
    if groups.get("u") is not None:
        out["u"] = maybe_float(groups["u"])
    if groups.get("s") is not None:
        out["s"] = maybe_float(groups["s"])
    return out


def existing_count(path: Path) -> int:
    if not path.exists():
        return 0
    with path.open("rb") as f:
        return sum(1 for _ in f)


def build_worker_configs(args: argparse.Namespace) -> list[WorkerConfig]:
    configs: list[WorkerConfig] = []
    if args.cpu_workers or args.gpu_workers:
        cpu_threads = args.cpu_threads if args.cpu_threads is not None else args.threads
        gpu_threads = args.gpu_threads if args.gpu_threads is not None else args.threads
        for i in range(args.cpu_workers):
            configs.append(
                WorkerConfig(
                    name=f"cpu-{i}",
                    backend=args.cpu_backend,
                    backend_opts=args.cpu_backend_opts,
                    threads=cpu_threads,
                )
            )
        for i in range(args.gpu_workers):
            backend_opts = ""
            if args.gpu_backend_opts:
                backend_opts = args.gpu_backend_opts[i % len(args.gpu_backend_opts)]
            configs.append(
                WorkerConfig(
                    name=f"gpu-{i}",
                    backend=args.gpu_backend,
                    backend_opts=backend_opts,
                    threads=gpu_threads,
                )
            )
    else:
        for i in range(args.workers):
            configs.append(
                WorkerConfig(
                    name=f"worker-{i}",
                    backend=args.backend,
                    backend_opts=args.backend_opts,
                    threads=args.threads,
                )
            )
    return configs


def lc0_command(args: argparse.Namespace, config: WorkerConfig) -> list[str]:
    command = [
        str(args.lc0),
        f"--weights={args.weights}",
        f"--threads={config.threads}",
    ]
    if config.backend:
        command.append(f"--backend={config.backend}")
    if config.backend_opts:
        command.append(f"--backend-opts={config.backend_opts}")
    if args.minibatch_size is not None:
        command.append(f"--minibatch-size={args.minibatch_size}")
    if args.max_prefetch is not None:
        command.append(f"--max-prefetch={args.max_prefetch}")
    if args.nncache_size is not None:
        command.append(f"--nncache={args.nncache_size}")
    if args.max_concurrent_searchers is not None:
        command.append(f"--max-concurrent-searchers={args.max_concurrent_searchers}")
    if args.multipv != 1:
        command.append(f"--multipv={args.multipv}")
    if args.score_type:
        command.append(f"--score-type={args.score_type}")
    if not args.no_verbose_move_stats:
        command.append("--verbose-move-stats")
    return command


class Lc0Worker:
    def __init__(self, args: argparse.Namespace, config: WorkerConfig):
        self.args = args
        self.config = config
        self.proc: subprocess.Popen[str] | None = None
        self.stderr_tail: deque[str] = deque(maxlen=80)
        self.stderr_thread: threading.Thread | None = None

    def start(self) -> None:
        self.proc = subprocess.Popen(
            lc0_command(self.args, self.config),
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        self.stderr_thread = threading.Thread(
            target=self._drain_stderr,
            name=f"{self.config.name}-stderr",
            daemon=True,
        )
        self.stderr_thread.start()
        send(self.proc, "uci")
        try:
            read_until(self.proc, "uciok")
        except RuntimeError as exc:
            raise RuntimeError(f"{self.config.name}: {exc}\n{self.stderr()}") from exc
        send(self.proc, "isready")
        try:
            read_until(self.proc, "readyok")
        except RuntimeError as exc:
            raise RuntimeError(f"{self.config.name}: {exc}\n{self.stderr()}") from exc

    def _drain_stderr(self) -> None:
        if self.proc is None or self.proc.stderr is None:
            return
        for line in self.proc.stderr:
            self.stderr_tail.append(line.rstrip("\n"))

    def stderr(self) -> str:
        if not self.stderr_tail:
            return "stderr: <empty>"
        return "stderr tail:\n" + "\n".join(self.stderr_tail)

    def evaluate(self, task: Task) -> dict[str, object]:
        if self.proc is None:
            raise RuntimeError("worker was not started")
        send(self.proc, f"position fen {task.fen}")
        send(self.proc, f"go nodes {self.args.nodes}")

        info_lines: list[str] = []
        bestmove = None
        while True:
            line = self.proc.stdout.readline()
            if line == "":
                raise RuntimeError(
                    f"{self.config.name}: lc0 exited during search\n{self.stderr()}"
                )
            line = line.rstrip("\n")
            if line.startswith("info "):
                info_lines.append(line)
            elif line.startswith("bestmove "):
                bestmove = line.split()[1]
                break

        parsed_infos = [parse_info(line) for line in info_lines]
        move_stats = [stat for line in info_lines if (stat := parse_move_stat(line))]
        return {
            "ordinal": task.ordinal,
            "index": task.input_index,
            "fen": task.fen,
            "nodes": self.args.nodes,
            "bestmove": bestmove,
            "worker": {
                "name": self.config.name,
                "backend": self.config.backend,
                "threads": self.config.threads,
            },
            "info": parsed_infos,
            "move_stats": move_stats,
        }

    def close(self) -> None:
        if self.proc is None:
            return
        try:
            send(self.proc, "quit")
        except Exception:
            pass
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        if self.stderr_thread is not None:
            self.stderr_thread.join(timeout=1)


def worker_loop(
    args: argparse.Namespace,
    config: WorkerConfig,
    tasks: queue.Queue[Task | None],
    results: queue.Queue[WorkerResult],
    errors: queue.Queue[BaseException],
    stop_event: threading.Event,
) -> None:
    worker = Lc0Worker(args, config)
    try:
        worker.start()
        while not stop_event.is_set():
            try:
                task = tasks.get(timeout=0.25)
            except queue.Empty:
                continue
            try:
                if task is None:
                    return
                record = worker.evaluate(task)
                line = json.dumps(record, separators=(",", ":"))
                results.put(WorkerResult(task.ordinal, line))
            finally:
                tasks.task_done()
    except BaseException as exc:
        errors.put(exc)
        stop_event.set()
    finally:
        worker.close()


def produce_tasks(
    args: argparse.Namespace,
    tasks: queue.Queue[Task | None],
    worker_count: int,
    skip: int,
    stop_event: threading.Event,
    state: dict[str, int | bool],
) -> None:
    produced = 0
    ordinal = 0
    try:
        with args.input.open("r", encoding="utf-8") as fens:
            for input_index, fen in enumerate(fens):
                if stop_event.is_set():
                    break
                fen = fen.strip()
                if not fen:
                    continue
                if ordinal < skip:
                    ordinal += 1
                    continue
                if args.limit and produced >= args.limit:
                    break
                task = Task(ordinal=ordinal, input_index=input_index, fen=fen)
                while not stop_event.is_set():
                    try:
                        tasks.put(task, timeout=0.25)
                        break
                    except queue.Full:
                        continue
                if stop_event.is_set():
                    break
                produced += 1
                state["produced"] = produced
                ordinal += 1
    finally:
        state["done"] = True
        if stop_event.is_set():
            return
        for _ in range(worker_count):
            while True:
                try:
                    tasks.put(None, timeout=0.25)
                    break
                except queue.Full:
                    continue


def main() -> int:
    args = parse_args()
    if args.nodes <= 0:
        raise SystemExit("--nodes must be positive")
    if args.threads < 0:
        raise SystemExit("--threads must be non-negative")
    if not (args.cpu_workers or args.gpu_workers) and args.workers <= 0:
        raise SystemExit("--workers must be positive")
    if args.cpu_workers < 0 or args.gpu_workers < 0:
        raise SystemExit("--cpu-workers and --gpu-workers must be non-negative")
    if args.cpu_threads is not None and args.cpu_threads < 0:
        raise SystemExit("--cpu-threads must be non-negative")
    if args.gpu_threads is not None and args.gpu_threads < 0:
        raise SystemExit("--gpu-threads must be non-negative")
    if args.queue_size <= 0:
        raise SystemExit("--queue-size must be positive")
    if args.minibatch_size is not None and args.minibatch_size < 0:
        raise SystemExit("--minibatch-size must be non-negative")
    if args.max_prefetch is not None and args.max_prefetch < 0:
        raise SystemExit("--max-prefetch must be non-negative")
    if args.nncache_size is not None and args.nncache_size < 0:
        raise SystemExit("--nncache-size must be non-negative")
    if (
        args.max_concurrent_searchers is not None
        and args.max_concurrent_searchers < 0
    ):
        raise SystemExit("--max-concurrent-searchers must be non-negative")
    if args.progress_interval < 0:
        raise SystemExit("--progress-interval must be non-negative")

    worker_configs = build_worker_configs(args)
    if not worker_configs:
        raise SystemExit("no workers configured")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    manifest_path = args.manifest or args.out.with_suffix(args.out.suffix + ".manifest.json")
    manifest_path.parent.mkdir(parents=True, exist_ok=True)

    skip = existing_count(args.out) if args.resume else 0
    mode = "a" if args.resume and args.out.exists() else "w"

    started = datetime.now(timezone.utc).isoformat()
    start_time = time.monotonic()
    evaluated = 0
    stop_event = threading.Event()
    task_queue: queue.Queue[Task | None] = queue.Queue(maxsize=args.queue_size)
    result_queue: queue.Queue[WorkerResult] = queue.Queue()
    error_queue: queue.Queue[BaseException] = queue.Queue()
    state: dict[str, int | bool] = {"produced": 0, "done": False}
    worker_threads = [
        threading.Thread(
            target=worker_loop,
            args=(args, config, task_queue, result_queue, error_queue, stop_event),
            name=f"lc0-{config.name}",
            daemon=True,
        )
        for config in worker_configs
    ]
    producer = threading.Thread(
        target=produce_tasks,
        args=(args, task_queue, len(worker_threads), skip, stop_event, state),
        name="fen-producer",
        daemon=True,
    )

    try:
        for thread in worker_threads:
            thread.start()
        producer.start()

        pending: dict[int, str] = {}
        next_ordinal = skip
        with args.out.open(mode, encoding="utf-8", newline="\n") as out:
            while True:
                if not error_queue.empty():
                    raise error_queue.get()
                if state["done"] and evaluated >= int(state["produced"]):
                    break
                try:
                    result = result_queue.get(timeout=0.25)
                except queue.Empty:
                    continue

                pending[result.ordinal] = result.line
                while next_ordinal in pending:
                    out.write(pending.pop(next_ordinal) + "\n")
                    evaluated += 1
                    next_ordinal += 1
                    if args.progress_interval and evaluated % args.progress_interval == 0:
                        elapsed = max(time.monotonic() - start_time, 0.001)
                        rate = evaluated / elapsed
                        print(
                            f"[lc0-eval] evaluated {evaluated} "
                            f"({rate:.2f} pos/s)",
                            file=sys.stderr,
                        )
    finally:
        stop_event.set()
        producer.join(timeout=5)
        for thread in worker_threads:
            thread.join(timeout=10)

    total_output_records = existing_count(args.out)
    manifest = {
        "created_at": started,
        "completed_at": datetime.now(timezone.utc).isoformat(),
        "lc0": str(args.lc0),
        "weights": str(args.weights),
        "input": str(args.input),
        "output": str(args.out),
        "nodes": args.nodes,
        "worker_count": len(worker_configs),
        "workers": [
            {
                "name": config.name,
                "backend": config.backend,
                "backend_opts": config.backend_opts,
                "threads": config.threads,
            }
            for config in worker_configs
        ],
        "multipv": args.multipv,
        "score_type": args.score_type,
        "verbose_move_stats": not args.no_verbose_move_stats,
        "minibatch_size": args.minibatch_size,
        "max_prefetch": args.max_prefetch,
        "nncache_size": args.nncache_size,
        "max_concurrent_searchers": args.max_concurrent_searchers,
        "resume_skipped": skip,
        "evaluated": evaluated,
        "total_output_records": total_output_records,
        "limit": args.limit,
        "seconds": round(time.monotonic() - start_time, 3),
    }
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"[lc0-eval] wrote {evaluated} records to {args.out}", file=sys.stderr)
    print(f"[lc0-eval] wrote manifest to {manifest_path}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
