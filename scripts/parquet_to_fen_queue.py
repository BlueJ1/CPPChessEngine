#!/usr/bin/env python3
"""Build an Lc0 teacher-evaluation FEN queue from parquet board shards."""

from __future__ import annotations

import argparse
import json
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable

import pyarrow.parquet as pq


def fen_ply(fen: str) -> int:
    """Return the zero-based game ply represented by a standard FEN."""
    parts = fen.split()
    if len(parts) != 6:
        raise ValueError(f"expected 6 FEN fields, got {len(parts)}: {fen!r}")
    fullmove = int(parts[5])
    if fullmove <= 0:
        raise ValueError(f"invalid fullmove in FEN: {fen!r}")
    return (fullmove - 1) * 2 + (1 if parts[1] == "b" else 0)


def iter_fens(path: Path) -> Iterable[str]:
    parquet = pq.ParquetFile(path)
    for row_group in range(parquet.metadata.num_row_groups):
        table = parquet.read_row_group(row_group, columns=["fen"])
        for fen in table.column("fen").to_pylist():
            if fen:
                yield fen


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Extract a sampled, deduplicated FEN queue from parquet files."
    )
    parser.add_argument("inputs", nargs="+", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--min-ply", type=int, default=12)
    parser.add_argument("--max-ply", type=int, default=160)
    parser.add_argument("--take-every", type=int, default=4)
    parser.add_argument("--max-positions", type=int, default=0)
    parser.add_argument("--no-dedupe", action="store_true")
    parser.add_argument(
        "--source-url",
        default="https://huggingface.co/datasets/lczerolens/tcec-boards",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.min_ply < 0 or args.max_ply < args.min_ply:
        raise SystemExit("--min-ply must be non-negative and <= --max-ply")
    if args.take_every <= 0:
        raise SystemExit("--take-every must be positive")
    if args.max_positions < 0:
        raise SystemExit("--max-positions must be non-negative")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    manifest_path = args.manifest or args.out.with_suffix(args.out.suffix + ".manifest.json")
    manifest_path.parent.mkdir(parents=True, exist_ok=True)

    stats = {
        "created_at": datetime.now(timezone.utc).isoformat(),
        "source_url": args.source_url,
        "inputs": [],
        "sampling": {
            "min_ply": args.min_ply,
            "max_ply": args.max_ply,
            "take_every": args.take_every,
            "max_positions": args.max_positions,
            "dedupe": not args.no_dedupe,
        },
        "rows_seen": 0,
        "bad_fens": 0,
        "out_of_window": 0,
        "stride_skipped": 0,
        "duplicate_skipped": 0,
        "positions_emitted": 0,
    }

    seen: set[str] = set()
    with args.out.open("w", encoding="utf-8", newline="\n") as out:
        for path in args.inputs:
            parquet = pq.ParquetFile(path)
            stats["inputs"].append(
                {
                    "path": str(path),
                    "rows": parquet.metadata.num_rows,
                    "row_groups": parquet.metadata.num_row_groups,
                }
            )
            for row_group in range(parquet.metadata.num_row_groups):
                table = parquet.read_row_group(row_group, columns=["fen"])
                for fen in table.column("fen").to_pylist():
                    stats["rows_seen"] += 1
                    try:
                        ply = fen_ply(fen)
                    except Exception:
                        stats["bad_fens"] += 1
                        continue
                    if ply < args.min_ply or ply > args.max_ply:
                        stats["out_of_window"] += 1
                        continue
                    if (ply - args.min_ply) % args.take_every != 0:
                        stats["stride_skipped"] += 1
                        continue
                    if not args.no_dedupe:
                        if fen in seen:
                            stats["duplicate_skipped"] += 1
                            continue
                        seen.add(fen)
                    out.write(fen)
                    out.write("\n")
                    stats["positions_emitted"] += 1
                    if (
                        args.max_positions
                        and stats["positions_emitted"] >= args.max_positions
                    ):
                        break
                if args.max_positions and stats["positions_emitted"] >= args.max_positions:
                    break
            if args.max_positions and stats["positions_emitted"] >= args.max_positions:
                break
            print(
                f"[fen-queue] {path}: emitted {stats['positions_emitted']}",
                file=sys.stderr,
            )

    manifest_path.write_text(json.dumps(stats, indent=2) + "\n", encoding="utf-8")
    print(
        f"[fen-queue] wrote {stats['positions_emitted']} positions to {args.out}",
        file=sys.stderr,
    )
    print(f"[fen-queue] wrote manifest to {manifest_path}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
