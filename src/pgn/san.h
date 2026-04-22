// SAN → Move resolution. Derived from lc0's chess/pgn.h (GPLv3).
#pragma once

#include <string_view>

#include "chess/bitboard.h"
#include "chess/board.h"

namespace pgn2pack {

// Parses a SAN token (e.g. "Nf3", "exd5", "O-O", "e8=Q+") against `board`,
// which is in the side-to-move's perspective (lc0 convention). Returns the
// matching move in the same perspective, ready to pass to
// PositionHistory::Append.
//
// Throws lczero::Exception on illegal/ambiguous SAN.
lczero::Move SanToMove(std::string_view san, const lczero::ChessBoard& board);

}  // namespace pgn2pack
