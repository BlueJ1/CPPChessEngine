#!/usr/bin/env python3
"""Evaluate a FEN queue with Lc0 through UCI and write JSONL labels."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
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
    parser.add_argument("--multipv", type=int, default=1)
    parser.add_argument("--score-type", default="WDL_mu")
    parser.add_argument("--no-verbose-move-stats", action="store_true")
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
    return None if "-" in value else float(value)


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


def main() -> int:
    args = parse_args()
    if args.nodes <= 0:
        raise SystemExit("--nodes must be positive")
    if args.threads <= 0:
        raise SystemExit("--threads must be positive")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    manifest_path = args.manifest or args.out.with_suffix(args.out.suffix + ".manifest.json")
    manifest_path.parent.mkdir(parents=True, exist_ok=True)

    skip = existing_count(args.out) if args.resume else 0
    mode = "a" if args.resume and args.out.exists() else "w"

    command = [str(args.lc0), f"--weights={args.weights}", f"--threads={args.threads}"]
    if args.backend:
        command.append(f"--backend={args.backend}")
    if args.multipv != 1:
        command.append(f"--multipv={args.multipv}")
    if args.score_type:
        command.append(f"--score-type={args.score_type}")
    if not args.no_verbose_move_stats:
        command.append("--verbose-move-stats")

    proc = subprocess.Popen(
        command,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1,
    )

    started = datetime.now(timezone.utc).isoformat()
    evaluated = 0
    try:
        send(proc, "uci")
        uci_lines = read_until(proc, "uciok")
        send(proc, "isready")
        read_until(proc, "readyok")

        with args.input.open("r", encoding="utf-8") as fens, args.out.open(
            mode, encoding="utf-8", newline="\n"
        ) as out:
            for index, fen in enumerate(fens):
                if index < skip:
                    continue
                fen = fen.strip()
                if not fen:
                    continue
                send(proc, f"position fen {fen}")
                send(proc, f"go nodes {args.nodes}")

                info_lines: list[str] = []
                bestmove = None
                while True:
                    line = proc.stdout.readline()
                    if line == "":
                        raise RuntimeError("lc0 exited during search")
                    line = line.rstrip("\n")
                    if line.startswith("info "):
                        info_lines.append(line)
                    elif line.startswith("bestmove "):
                        bestmove = line.split()[1]
                        break

                parsed_infos = [parse_info(line) for line in info_lines]
                move_stats = [
                    stat for line in info_lines if (stat := parse_move_stat(line))
                ]
                record = {
                    "index": index,
                    "fen": fen,
                    "nodes": args.nodes,
                    "bestmove": bestmove,
                    "info": parsed_infos,
                    "move_stats": move_stats,
                }
                out.write(json.dumps(record, separators=(",", ":")) + "\n")
                evaluated += 1
                if evaluated % 100 == 0:
                    print(f"[lc0-eval] evaluated {evaluated}", file=sys.stderr)
                if args.limit and evaluated >= args.limit:
                    break
    finally:
        try:
            send(proc, "quit")
        except Exception:
            pass
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

    manifest = {
        "created_at": started,
        "completed_at": datetime.now(timezone.utc).isoformat(),
        "lc0": str(args.lc0),
        "weights": str(args.weights),
        "input": str(args.input),
        "output": str(args.out),
        "nodes": args.nodes,
        "threads": args.threads,
        "backend": args.backend,
        "multipv": args.multipv,
        "score_type": args.score_type,
        "verbose_move_stats": not args.no_verbose_move_stats,
        "resume_skipped": skip,
        "evaluated": evaluated,
        "limit": args.limit,
    }
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"[lc0-eval] wrote {evaluated} records to {args.out}", file=sys.stderr)
    print(f"[lc0-eval] wrote manifest to {manifest_path}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
