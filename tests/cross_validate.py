#!/usr/bin/env python3
"""Cross-validate pgn2pack against python-chess on real Lichess games.

Pipeline:
  1. Stream-extract a bounded slice (~100 MB uncompressed, configurable) from
     tests/data/lichess_db_standard_rated_2017-05.pgn.zst, stopping at a game
     boundary. Never unpacks the full file.
  2. Run the pgn2pack binary on that slice to produce a .bin of 40-byte records.
  3. Parse the same PGN slice with python-chess, game by game, producing the
     expected FEN at every half-move.
  4. For each game pgn2pack emitted, compare every decoded FEN to
     python-chess's FEN. Fails loud on the first mismatch with full context.

pgn2pack's internal game_id is a zero-based counter over games in the input
file, incremented even when a game is skipped due to bad SAN. So game_id == k
corresponds to the k-th [Event ...] header in the PGN.
"""

import argparse
import io
import os
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import chess
import chess.pgn
import zstandard

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_ZST = REPO_ROOT / "tests" / "data" / "lichess_db_standard_rated_2017-05.pgn.zst"
DEFAULT_BIN = REPO_ROOT / "build" / "pgn2pack"

RECORD_FMT = "<Q16sBBHIHBBI"  # 40 bytes; see src/pack/record.h
RECORD_SIZE = struct.calcsize(RECORD_FMT)
assert RECORD_SIZE == 40, RECORD_SIZE

PIECE_CHAR = "PNBRQK"


def extract_pgn_slice(zst_path: Path, out_path: Path, budget_bytes: int) -> int:
    """Stream-decompress until we've emitted >= budget_bytes AND we're sitting
    on a game boundary (blank line followed by a '[' header).

    Returns the number of bytes written.
    """
    dctx = zstandard.ZstdDecompressor(max_window_size=2**31)
    written = 0
    # Games are separated by blank lines. We want to cut at a boundary so the
    # last game is complete. Strategy: once budget hit, keep buffering until we
    # see "\n\n[" — then cut right before the '['.
    tail = b""
    with zst_path.open("rb") as src, dctx.stream_reader(src) as reader, \
         out_path.open("wb") as dst:
        while True:
            chunk = reader.read(1 << 20)  # 1 MiB
            if not chunk:
                dst.write(tail)
                written += len(tail)
                break
            if written < budget_bytes:
                dst.write(tail)
                written += len(tail)
                tail = chunk
            else:
                # Look for a game boundary inside (tail + chunk).
                buf = tail + chunk
                # Find first occurrence of "\n\n[" after position 0.
                idx = buf.find(b"\n\n[")
                if idx == -1:
                    dst.write(tail)
                    written += len(tail)
                    tail = chunk
                else:
                    # Emit up to and including the blank line separating games.
                    emit = buf[: idx + 2]  # keep "\n\n"
                    dst.write(emit)
                    written += len(emit)
                    break
    return written


def iter_records(bin_path: Path):
    """Yield (game_id, ply_in_game, fen, rule50, fullmove) per record."""
    with bin_path.open("rb") as f:
        while True:
            data = f.read(RECORD_SIZE)
            if not data:
                return
            if len(data) != RECORD_SIZE:
                raise ValueError(f"truncated record at EOF: {len(data)} bytes")
            occ, nibbles, stm_castle_ep, rule50, fullmove, game_id, ply, result, flags, reserved = struct.unpack(
                RECORD_FMT, data
            )
            fen = decode_fen(occ, nibbles, stm_castle_ep, flags, rule50, fullmove)
            yield game_id, ply, fen, rule50, fullmove, result, flags


def decode_fen(occupancy: int, nibbles: bytes, scef: int, flags: int,
               rule50: int, fullmove: int) -> str:
    """Port of pack::DecodeFen — byte-exact."""
    squares = [None] * 64
    bits = occupancy
    nib_idx = 0
    while bits:
        lsb = (bits & -bits).bit_length() - 1
        bits &= bits - 1
        byte = nibbles[nib_idx >> 1]
        nibble = (byte >> 4) if (nib_idx & 1) else (byte & 0x0F)
        squares[lsb] = nibble
        nib_idx += 1

    rows = []
    for row in range(7, -1, -1):
        s = ""
        empty = 0
        for col in range(8):
            sq = row * 8 + col
            n = squares[sq]
            if n is None:
                empty += 1
            else:
                if empty:
                    s += str(empty)
                    empty = 0
                code = n & 0x07
                ch = PIECE_CHAR[code]
                if n & 0x08:
                    ch = ch.lower()
                s += ch
        if empty:
            s += str(empty)
        rows.append(s)
    board_part = "/".join(rows)

    stm = scef & 0x1
    castle = (scef >> 1) & 0xF
    ep_file = (scef >> 5) & 0x7
    stm_str = "b" if stm else "w"
    if castle == 0:
        cs = "-"
    else:
        cs = ""
        if castle & 1: cs += "K"
        if castle & 2: cs += "Q"
        if castle & 4: cs += "k"
        if castle & 8: cs += "q"
    ep_present = (flags & 0x2) != 0
    if not ep_present:
        ep = "-"
    else:
        ep_row = 2 if stm else 5
        ep = f"{'abcdefgh'[ep_file]}{ep_row + 1}"
    return f"{board_part} {stm_str} {cs} {ep} {rule50} {fullmove}"


def normalize_fen_for_compare(fen: str) -> str:
    """Compare FENs on the fields we trust — piece placement, stm, castling,
    ep, plus rule50 clamped to 255 (pgn2pack stores it as uint8_t). Fullmove
    is also compared since our test corpus all starts from startpos where the
    fullmove bug does not manifest.
    """
    parts = fen.split()
    if len(parts) != 6:
        return fen
    parts[4] = str(min(int(parts[4]), 255))
    return " ".join(parts)


def run_pgn2pack(binary: Path, pgn_path: Path, out_bin: Path) -> None:
    proc = subprocess.run(
        [str(binary), "--out", str(out_bin), str(pgn_path)],
        capture_output=True, text=True,
    )
    if proc.returncode != 0:
        raise RuntimeError(f"pgn2pack failed: {proc.stderr}")
    # Surface stderr lines count for visibility.
    err_lines = proc.stderr.strip().splitlines()
    print(f"pgn2pack wrote {out_bin}; stderr: {len(err_lines)} lines "
          f"(last: {err_lines[-1] if err_lines else '<none>'})")


def expected_fens_per_game(pgn_path: Path):
    """Iterate games in the PGN in order. For each game yield a list of
    (ply_index, fen_after_that_ply), or None if python-chess couldn't parse.
    Indexing matches pgn2pack's game_id (which counts every game header).
    """
    with pgn_path.open("r", encoding="utf-8", errors="replace") as f:
        while True:
            game = chess.pgn.read_game(f)
            if game is None:
                return
            per_ply = []
            try:
                board = game.board()  # may be non-startpos if [FEN] set
                ply = 0
                for mv in game.mainline_moves():
                    board.push(mv)
                    per_ply.append((ply, board.fen()))
                    ply += 1
            except Exception as e:
                # Treat as broken — still yield so indexing is preserved.
                yield None
                continue
            yield per_ply


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--zst", type=Path, default=DEFAULT_ZST)
    ap.add_argument("--binary", type=Path, default=DEFAULT_BIN)
    ap.add_argument("--budget-mb", type=int, default=100,
                    help="Approximate uncompressed slice to extract (MB)")
    ap.add_argument("--keep", action="store_true",
                    help="Keep the extracted PGN slice and records .bin")
    args = ap.parse_args()

    if not args.zst.exists():
        print(f"missing zst: {args.zst}", file=sys.stderr)
        return 2
    if not args.binary.exists():
        print(f"missing pgn2pack binary: {args.binary}", file=sys.stderr)
        return 2

    tmp = tempfile.mkdtemp(prefix="pgn2pack_xval_")
    pgn_slice = Path(tmp) / "slice.pgn"
    bin_out = Path(tmp) / "slice.bin"

    budget = args.budget_mb * 1024 * 1024
    print(f"extracting ~{args.budget_mb} MB from {args.zst.name} …")
    n = extract_pgn_slice(args.zst, pgn_slice, budget)
    print(f"wrote {n/1e6:.1f} MB of PGN to {pgn_slice}")

    print("running pgn2pack …")
    run_pgn2pack(args.binary, pgn_slice, bin_out)

    # Group records by game_id.
    print("decoding records …")
    records_by_game = {}
    total_records = 0
    for gid, ply, fen, rule50, fullmove, result, flags in iter_records(bin_out):
        records_by_game.setdefault(gid, []).append((ply, fen))
        total_records += 1
    print(f"decoded {total_records} records across "
          f"{len(records_by_game)} games")

    # Stream expected FENs from python-chess.
    print("cross-checking with python-chess …")
    ep_only_mismatches = 0
    other_mismatches = 0
    ep_examples = []
    other_examples = []
    checked_games = 0
    checked_plies = 0
    for gid, expected in enumerate(expected_fens_per_game(pgn_slice)):
        pack = records_by_game.get(gid)
        if pack is None:
            continue  # pgn2pack skipped this game (broken SAN); that's fine
        if expected is None:
            print(f"game {gid}: python-chess failed but pgn2pack emitted "
                  f"{len(pack)} records")
            continue
        if len(pack) != len(expected):
            other_mismatches += 1
            if len(other_examples) < 5:
                other_examples.append(
                    f"game {gid}: ply count mismatch — "
                    f"pack={len(pack)} expected={len(expected)}")
            continue
        for (p_ply, p_fen), (e_ply, e_fen) in zip(pack, expected):
            if p_ply != e_ply:
                other_mismatches += 1
                if len(other_examples) < 5:
                    other_examples.append(
                        f"game {gid}: ply index out of sync "
                        f"({p_ply} vs {e_ply})")
                break
            p_norm = normalize_fen_for_compare(p_fen)
            e_norm = normalize_fen_for_compare(e_fen)
            if p_norm != e_norm:
                p_parts = p_norm.split()
                e_parts = e_norm.split()
                # Category: ep-only iff every other field matches.
                if (len(p_parts) == 6 and len(e_parts) == 6 and
                    p_parts[:3] == e_parts[:3] and
                    p_parts[4:] == e_parts[4:] and
                    p_parts[3] != e_parts[3]):
                    ep_only_mismatches += 1
                    if len(ep_examples) < 3:
                        ep_examples.append(
                            f"game {gid} ply {p_ply}: ep pack={p_parts[3]} "
                            f"expected={e_parts[3]}")
                else:
                    other_mismatches += 1
                    if len(other_examples) < 5:
                        other_examples.append(
                            f"game {gid} ply {p_ply}:\n"
                            f"  pgn2pack: {p_fen}\n"
                            f"  expected: {e_fen}")
            checked_plies += 1
        checked_games += 1

    if args.keep:
        print(f"kept: {pgn_slice} {bin_out}")
    else:
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)

    print("====")
    print(f"games cross-checked: {checked_games}  plies: {checked_plies}")
    print(f"ep-only mismatches: {ep_only_mismatches}")
    for e in ep_examples:
        print(f"  {e}")
    print(f"other mismatches:   {other_mismatches}")
    for e in other_examples:
        print(f"  {e}")
    return 0 if (ep_only_mismatches + other_mismatches) == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
