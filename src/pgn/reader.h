// Streaming PGN reader over zstd-compressed or plain input. One game at a
// time, no allocation of the full file.
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <zstd.h>

#include "chess/board.h"
#include "chess/position.h"

namespace pgn2pack {

struct GameHeader {
  std::string start_fen;       // empty → startpos
  std::string result;          // "1-0"/"0-1"/"1/2-1/2"/"*"
  int white_elo = 0;
  int black_elo = 0;
  std::string utc_date;        // "YYYY.MM.DD"
  std::string time_control;
  std::string event;
};

// Called once per half-move. `history.Last()` holds the resulting position.
// `header` is the current game's header.
using PlyCallback =
    std::function<void(const GameHeader& header,
                       const lczero::PositionHistory& history,
                       uint32_t game_id, uint16_t ply_in_game)>;

// Called after each complete game. Useful for flushing game-scoped state.
using GameCallback = std::function<void(const GameHeader& header,
                                        uint32_t game_id,
                                        uint16_t total_plies)>;

// Parses `path` (auto-detected: *.zst → zstd stream, otherwise plain file).
// Invokes `on_ply` for every half-move and `on_game` when each game ends.
// Games that fail to parse (illegal SAN, truncated, etc.) are logged to stderr
// and skipped; parsing continues with the next game.
//
// Returns the number of games successfully emitted.
uint64_t StreamPgnFile(const std::string& path,
                       uint32_t game_id_base,
                       const PlyCallback& on_ply,
                       const GameCallback& on_game);

}  // namespace pgn2pack
