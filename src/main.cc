#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "chess/board.h"
#include "chess/position.h"
#include "pack/record.h"
#include "pgn/reader.h"

namespace {

struct Args {
  std::string out_path;
  std::string fen_out_path;
  int min_elo = 0;
  uint16_t min_ply = 0;
  uint16_t max_ply = std::numeric_limits<uint16_t>::max();
  uint32_t take_every = 1;
  uint64_t max_positions = 0;
  bool stdout_fen = false;
  std::vector<std::string> inputs;
};

bool ParseUnsigned(const std::string& s, uint64_t* out) {
  if (s.empty()) return false;
  char* end = nullptr;
  unsigned long long value = std::strtoull(s.c_str(), &end, 10);
  if (*end != '\0') return false;
  *out = static_cast<uint64_t>(value);
  return true;
}

bool ParseArgs(int argc, char** argv, Args* args) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--out" && i + 1 < argc) {
      args->out_path = argv[++i];
    } else if (a == "--fen-out" && i + 1 < argc) {
      args->fen_out_path = argv[++i];
    } else if (a == "--min-elo" && i + 1 < argc) {
      args->min_elo = std::atoi(argv[++i]);
    } else if (a == "--min-ply" && i + 1 < argc) {
      uint64_t value = 0;
      if (!ParseUnsigned(argv[++i], &value) ||
          value > std::numeric_limits<uint16_t>::max()) {
        std::cerr << "Invalid --min-ply value\n";
        return false;
      }
      args->min_ply = static_cast<uint16_t>(value);
    } else if (a == "--max-ply" && i + 1 < argc) {
      uint64_t value = 0;
      if (!ParseUnsigned(argv[++i], &value) ||
          value > std::numeric_limits<uint16_t>::max()) {
        std::cerr << "Invalid --max-ply value\n";
        return false;
      }
      args->max_ply = static_cast<uint16_t>(value);
    } else if (a == "--take-every" && i + 1 < argc) {
      uint64_t value = 0;
      if (!ParseUnsigned(argv[++i], &value) ||
          value == 0 || value > std::numeric_limits<uint32_t>::max()) {
        std::cerr << "Invalid --take-every value\n";
        return false;
      }
      args->take_every = static_cast<uint32_t>(value);
    } else if (a == "--max-positions" && i + 1 < argc) {
      if (!ParseUnsigned(argv[++i], &args->max_positions)) {
        std::cerr << "Invalid --max-positions value\n";
        return false;
      }
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
  if (args->min_ply > args->max_ply) {
    std::cerr << "--min-ply must be <= --max-ply\n";
    return false;
  }
  return !args->inputs.empty();
}

void PrintUsage() {
  std::cerr
      << "usage: pgn2pack [--out path.bin] [--fen-out positions.fen|-]\n"
      << "                [--min-elo N] [--min-ply N] [--max-ply N]\n"
      << "                [--take-every N] [--max-positions N]\n"
      << "                [--stdout-fen] input1.pgn[.zst] [...]\n";
}

struct StopProcessing {};

}  // namespace

int main(int argc, char** argv) {
  lczero::InitializeMagicBitboards();

  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    PrintUsage();
    return 1;
  }

  const bool write_packed = !args.out_path.empty() ||
                            (!args.stdout_fen && args.fen_out_path.empty());

  std::FILE* out = nullptr;
  if (write_packed && args.out_path.empty()) {
    out = stdout;
  } else if (!args.out_path.empty()) {
    out = std::fopen(args.out_path.c_str(), "wb");
    if (!out) {
      std::perror(args.out_path.c_str());
      return 1;
    }
  }

  std::FILE* fen_out = nullptr;
  if (!args.fen_out_path.empty()) {
    if (args.fen_out_path == "-") {
      fen_out = stdout;
    } else {
      fen_out = std::fopen(args.fen_out_path.c_str(), "wb");
      if (!fen_out) {
        std::perror(args.fen_out_path.c_str());
        if (out && out != stdout) std::fclose(out);
        return 1;
      }
    }
  }

  std::vector<pgn2pack::PackedRecord> batch;
  batch.reserve(256);

  uint64_t total_games = 0;
  uint64_t total_plies = 0;
  uint64_t total_positions = 0;
  uint32_t next_game_id = 0;
  bool stop = false;

  auto flush_batch = [&]() {
    if (!write_packed || batch.empty()) return;
    size_t wrote = std::fwrite(batch.data(), sizeof(pgn2pack::PackedRecord),
                               batch.size(), out);
    if (wrote != batch.size()) {
      throw std::runtime_error("Failed to write packed records.");
    }
    batch.clear();
  };

  for (const std::string& path : args.inputs) {
    if (stop) break;
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
      if (ply_in_game < args.min_ply || ply_in_game > args.max_ply) return;
      if (((ply_in_game - args.min_ply) % args.take_every) != 0) return;
      if (args.max_positions > 0 && total_positions >= args.max_positions) {
        throw StopProcessing{};
      }

      if (write_packed) {
        batch.emplace_back();
        pgn2pack::EncodePosition(history.Last(), game_id, ply_in_game,
                                 current_result, current_flags, &batch.back());
      }

      if (args.stdout_fen) {
        std::cout << lczero::GetFen(history.Last()) << "\n";
      }
      if (fen_out) {
        const std::string fen = lczero::GetFen(history.Last());
        if (std::fprintf(fen_out, "%s\n", fen.c_str()) < 0) {
          throw std::runtime_error("Failed to write FEN output.");
        }
      }
      ++total_positions;

      if (batch.size() >= 256) {
        flush_batch();
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
    } catch (const StopProcessing&) {
      stop = true;
    } catch (const std::exception& e) {
      std::cerr << "[pgn2pack] fatal on " << path << ": " << e.what() << "\n";
    }
  }

  try {
    flush_batch();
  } catch (const std::exception& e) {
    std::cerr << "[pgn2pack] fatal while flushing output: " << e.what() << "\n";
    if (out && out != stdout) std::fclose(out);
    if (fen_out && fen_out != stdout) std::fclose(fen_out);
    return 1;
  }
  if (out && out != stdout) std::fclose(out);
  if (fen_out && fen_out != stdout) std::fclose(fen_out);

  std::cerr << "[pgn2pack] done: " << total_games << " games, "
            << total_plies << " plies, " << total_positions
            << " positions emitted\n";
  return 0;
}
