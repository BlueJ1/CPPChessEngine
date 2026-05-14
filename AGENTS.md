# AGENTS.md

This file provides guidance to Codex (Codex.ai/code) when working with code in this repository.

## What this project is

`pgn2pack`: a C++20 command-line tool that streams PGN files (plain or `.zst`-compressed), replays every half-move, and writes each resulting position as a fixed **40-byte `PackedRecord`** to a binary file (or stdout). Intended as a training-data pipeline stage for a chess engine.

The repo name ("CPPChessEngine") is aspirational; the product binary is `pgn2pack`. The build also creates a `pgn2pack_lib` static library for shared non-main sources and, by default, a Catch2 test binary. The `lc0/` directory is a vendored checkout of Leela Chess Zero — it is **not** compiled directly. Only the files explicitly listed in `CMakeLists.txt` (notably `src/chess/{bitboard,board,position}.cc`) are built, and those are lc0-derived sources that live under `src/`.

## Build / run

```bash
cmake -S . -B build              # Release by default; tests ON by default
cmake --build build -j
ctest --test-dir build --output-on-failure

./build/pgn2pack --out out.bin --min-elo 2000 tests/sample.pgn
./build/pgn2pack --out /tmp/pgn2pack-smoke.bin --stdout-fen tests/sample.pgn
```

Key build notes:
- C++20, `-O3 -march=native -flto` in Release.
- Depends on **libzstd** (found via `find_package(zstd)` or pkg-config fallback).
- Tests are enabled by default via `PGN2PACK_BUILD_TESTS=ON` and fetch Catch2 v3.5.3 with CMake `FetchContent`. For CLI-only/offline builds, configure with `-DPGN2PACK_BUILD_TESTS=OFF`.
- On non-x86 hosts, `NO_PEXT` / `NO_POPCNT=0` are defined so lc0's magic-bitboard code skips the BMI2 PEXT path. Don't remove this guard without testing on Apple Silicon / ARM.
- `lczero::InitializeMagicBitboards()` **must** be called before any board/position use — `main.cc` and `tests/test_main.cc` do this first thing.

Tests live under `tests/` and cover packing round-trips, SAN parsing, and the streaming PGN reader. `tests/sample.pgn` is both a fixture and a useful manual smoke input. If using `--stdout-fen` for eyeballing replayed positions, pass `--out` too; otherwise packed binary records also go to stdout.

## Architecture

Data flows one direction: **PGN bytes → parsed game → replayed positions → 40-byte records**.

1. `pgn/reader.{h,cc}` — `StreamPgnFile(path, base_id, on_ply, on_game)` auto-detects `.zst` vs plain, parses one game at a time (no full-file buffering), and invokes:
   - `on_ply` after every half-move, with the full `lczero::PositionHistory` so callers see the position *after* the move.
   - `on_game` when a game completes. Games with illegal/ambiguous SAN are logged to stderr and skipped — parsing continues.
2. `pgn/san.cc` — `SanToMove` resolves a SAN token against an lc0 `ChessBoard` (side-to-move perspective). Derived from lc0's `chess/pgn.h`.
3. `chess/{bitboard,board,position}.{h,cc}` — verbatim-ish lc0 sources providing bitboards, board, and `PositionHistory`. GPLv3. **lc0 uses the side-to-move perspective**: the board is mentally flipped when it's black's turn. Any new code touching `ChessBoard` must respect this.
4. `pack/record.{h,cc}` — defines the 40-byte `PackedRecord` (see `record.h` for the exact bit layout: occupancy bitboard + 4-bit piece nibbles + stm/castling/ep packed byte + clocks + game_id/ply/result/flags). `EncodePosition` writes it; `DecodeFen` reconstructs a FEN for round-trip verification (not Chess960-faithful — rook files are lost). The en-passant bit is emitted only when a legal en-passant capture exists.
5. `main.cc` — wires the above: parses CLI flags, batches 256 records at a time to `std::fwrite`, applies `--min-elo` filtering from game headers, and advances `next_game_id` across input files.

### Invariants worth knowing before editing

- `sizeof(PackedRecord) == 40` is `static_assert`ed. The struct is `#pragma pack(push, 1)`. If you add a field, bump the layout intentionally — downstream consumers will read raw bytes.
- `game_id` is intended to be monotonic across inputs: `StreamPgnFile` returns how many games it successfully emitted, and `main.cc` adds that to `next_game_id` before the next file. If you touch broken-game skip semantics, check that emitted IDs stay unique across files.
- `flags` bit 0 means "non-standard startpos" (i.e., the game had a `[FEN ...]` tag that isn't the classical startpos). Bit 1 means "en-passant square is set"; the file lives in `stm_castle_ep` bits 5-7.
- When initializing from a FEN, lc0's `ChessBoard::SetFromFen` writes the raw fullmove number into its second out-param. Convert it to game ply before `PositionHistory::Reset`: `(fullmove - 1) * 2 + (black_to_move ? 1 : 0)`.

## Licensing note

`src/chess/` and parts of `src/pgn/san.cc` are **GPLv3** (lc0 lineage). Anything linked into `pgn2pack` inherits that license.
