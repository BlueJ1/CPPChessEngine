#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "chess/board.h"
#include "chess/position.h"
#include "pack/record.h"
#include "pgn/reader.h"

namespace {

struct Args {
  std::string out_path;
  int min_elo = 0;
  bool stdout_fen = false;
  std::vector<std::string> inputs;
};

bool ParseArgs(int argc, char** argv, Args* args) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--out" && i + 1 < argc) {
      args->out_path = argv[++i];
    } else if (a == "--min-elo" && i + 1 < argc) {
      args->min_elo = std::atoi(argv[++i]);
    } else if (a == "--stdout-fen") {
      args->stdout_fen = true;
    } else if (a == "--help" || a == "-h") {
      return false;
    } else if (!a.empty() && a[0] == '-') {
      std::cerr << "Unknown flag: " << a << "\n";
      return false;
    } else {
      args->inputs.push_back(a);
    }
  }
  return !args->inputs.empty();
}

void PrintUsage() {
  std::cerr << "usage: pgn2pack [--out path.bin] [--min-elo N] [--stdout-fen] "
               "input1.pgn.zst [...]\n";
}

}  // namespace

int main(int argc, char** argv) {
  lczero::InitializeMagicBitboards();

  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    PrintUsage();
    return 1;
  }

  std::FILE* out = stdout;
  if (!args.out_path.empty()) {
    out = std::fopen(args.out_path.c_str(), "wb");
    if (!out) {
      std::perror(args.out_path.c_str());
      return 1;
    }
  }

  std::vector<pgn2pack::PackedRecord> batch;
  batch.reserve(256);

  uint64_t total_games = 0;
  uint64_t total_plies = 0;
  uint32_t next_game_id = 0;

  for (const std::string& path : args.inputs) {
    uint8_t current_result = 0;
    uint8_t current_flags = 0;
    bool skip_current = false;

    auto on_ply = [&](const pgn2pack::GameHeader& h,
                      const lczero::PositionHistory& history,
                      uint32_t game_id, uint16_t ply_in_game) {
      if (ply_in_game == 0) {
        current_result = pgn2pack::EncodeResult(h.result);
        current_flags = (!h.start_fen.empty() &&
                         h.start_fen != lczero::ChessBoard::kStartposFen)
                            ? 0x01 : 0x00;
        skip_current = (args.min_elo > 0) &&
                       (h.white_elo < args.min_elo ||
                        h.black_elo < args.min_elo);
      }
      if (skip_current) return;

      batch.emplace_back();
      pgn2pack::EncodePosition(history.Last(), game_id, ply_in_game,
                               current_result, current_flags, &batch.back());

      if (args.stdout_fen) {
        std::cout << lczero::GetFen(history.Last()) << "\n";
      }

      if (batch.size() >= 256) {
        std::fwrite(batch.data(), sizeof(pgn2pack::PackedRecord),
                    batch.size(), out);
        batch.clear();
      }
    };

    auto on_game = [&](const pgn2pack::GameHeader&, uint32_t,
                       uint16_t plies) {
      if (!skip_current) {
        ++total_games;
        total_plies += plies;
      }
      if (total_games && (total_games % 10000 == 0)) {
        std::cerr << "[pgn2pack] " << total_games << " games, "
                  << total_plies << " plies\n";
      }
    };

    try {
      next_game_id += (uint32_t)pgn2pack::StreamPgnFile(
          path, next_game_id, on_ply, on_game);
    } catch (const std::exception& e) {
      std::cerr << "[pgn2pack] fatal on " << path << ": " << e.what() << "\n";
    }
  }

  if (!batch.empty()) {
    std::fwrite(batch.data(), sizeof(pgn2pack::PackedRecord), batch.size(),
                out);
    batch.clear();
  }
  if (out != stdout) std::fclose(out);

  std::cerr << "[pgn2pack] done: " << total_games << " games, "
            << total_plies << " plies\n";
  return 0;
}
