# Distillation Position Dataset

This project can assemble a first-pass queue of positions for Lc0 teacher
evaluation from high-quality engine PGNs.

## Recommended sources

Use a tiered corpus instead of one monolithic source:

1. LCZero training data, when you want native LC0-style policy/value targets.
2. Stockfish Fishtest LTC PGNs for scale.
3. TCEC full PGNs for elite top-engine games and tournament diversity.
4. CCRL only as an optional diversity source; it is broader and less
   consistently top-engine than TCEC/Fishtest.

For serious distillation, do not train directly on the played move alone. Use
these PGNs to sample positions, then run an Lc0 teacher pass that records at
least policy, Q/WDL, visits/nodes, search settings, and the exact network hash.

## Fishtest input

The Stockfish Fishtest archive ships as `.pgn.gz`. `pgn2pack` reads plain PGN
and `.zst`, so either decompress to plain PGN or transcode to zstd:

```bash
gzip -dc test-id.pgn.gz | zstd -T0 -o test-id.pgn.zst
```

## Position export

If using parquet board shards such as `lczerolens/tcec-boards`, extract the
`fen` column directly:

```bash
python3 scripts/parquet_to_fen_queue.py \
  --out data/tcec_boards_teacher_queue.fen \
  --min-ply 12 \
  --max-ply 160 \
  --take-every 4 \
  train-00000-of-00002.parquet \
  train-00001-of-00002.parquet
```

This also writes `data/tcec_boards_teacher_queue.fen.manifest.json` with input
row counts and sampling statistics.

Export a FEN queue without writing packed binary records:

```bash
./build/pgn2pack \
  --fen-out data/teacher_queue.fen \
  --min-ply 12 \
  --max-ply 160 \
  --take-every 4 \
  --max-positions 10000000 \
  path/to/*.pgn.zst
```

Use `--out data/positions.bin` at the same time if you also want the 40-byte
packed records for downstream training examples:

```bash
./build/pgn2pack \
  --out data/positions.bin \
  --fen-out data/teacher_queue.fen \
  --min-ply 12 \
  --max-ply 160 \
  --take-every 4 \
  path/to/*.pgn.zst
```

## Initial sampling policy

Good defaults for the first teacher pass:

- Skip the opening book region with `--min-ply 12`.
- Avoid very late low-signal tablebase-like tails with `--max-ply 160`.
- Use `--take-every 4` or `--take-every 6` to reduce near-duplicate adjacent
  positions.
- Build separate queues per source, then mix them deliberately during training.
- Deduplicate by normalized FEN downstream if the teacher pass is expensive.

Keep a manifest beside each teacher output with source name, source license,
engine/network version, Lc0 backend, node/visit budget, temperature settings,
and command line.

## Lc0 evaluation

Download a network under `data/lc0/`, build Lc0, then evaluate the queue:

```bash
python3 scripts/evaluate_fens_lc0.py \
  --lc0 lc0/build/codex-release/lc0 \
  --weights data/lc0/t1-256x10-distilled-swa-2432500.pb \
  --input data/tcec_boards_teacher_queue.fen \
  --out data/evals/lc0_t1_256x10_nodes128.jsonl \
  --nodes 128 \
  --backend blas \
  --workers 4 \
  --threads 1
```

The JSONL output stores the FEN, best move, UCI search info, and structured
`move_stats` parsed from Lc0 verbose move stats. To continue a partially
completed output, add `--resume`; the script skips the number of records already
present in the output file.

For Metal GPU evaluation on Apple Silicon, run several Lc0 subprocesses against
the same GPU. The coordinator writes JSONL records in input order even when
workers finish out of order:

```bash
python3 scripts/evaluate_fens_lc0.py \
  --lc0 lc0/build/codex-release/lc0 \
  --weights data/lc0/t1-256x10-distilled-swa-2432500.pb \
  --input data/tcec_boards_teacher_queue.fen \
  --out data/evals/lc0_t1_256x10_nodes128.jsonl \
  --nodes 128 \
  --gpu-workers 6 \
  --gpu-backend metal \
  --gpu-backend-opts gpu=0,batch=8,max_batch=64 \
  --gpu-threads 1 \
  --minibatch-size 32 \
  --max-prefetch 32 \
  --resume
```

In the Codex sandbox, Metal device access requires running the evaluator with
escalated permissions. Without that, Lc0 cannot initialize the Metal device.

On multi-GPU systems, pass one `--gpu-backend-opts` value per device and use
the GPU backend supported by your Lc0 build. Values are cycled across GPU
workers:

```bash
python3 scripts/evaluate_fens_lc0.py \
  --lc0 lc0/build/codex-release/lc0 \
  --weights data/lc0/t1-256x10-distilled-swa-2432500.pb \
  --input data/tcec_boards_teacher_queue.fen \
  --out data/evals/lc0_t1_256x10_nodes128.jsonl \
  --nodes 128 \
  --gpu-workers 2 \
  --gpu-backend cudnn \
  --gpu-backend-opts gpu=0 \
  --gpu-backend-opts gpu=1 \
  --resume
```
