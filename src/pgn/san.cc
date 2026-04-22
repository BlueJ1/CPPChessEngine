// Derived from lc0 chess/pgn.h (GPLv3) SanToMove. Rewritten to operate purely
// in the board's own perspective (no post-mirror) so the result can be handed
// directly to PositionHistory::Append.
#include "pgn/san.h"

#include <algorithm>
#include <string>

#include "utils/exception.h"

namespace pgn2pack {

using lczero::BitBoard;
using lczero::BoardSquare;
using lczero::ChessBoard;
using lczero::Exception;
using lczero::Move;

namespace {

Move::Promotion PieceToPromotion(int p) {
  switch (p) {
    case -1: return Move::Promotion::None;
    case 2:  return Move::Promotion::Queen;
    case 3:  return Move::Promotion::Bishop;
    case 4:  return Move::Promotion::Knight;
    case 5:  return Move::Promotion::Rook;
    default: throw Exception("Illegal SAN promotion piece.");
  }
}

}  // namespace

Move SanToMove(std::string_view san, const ChessBoard& board) {
  // Strip trailing annotations.
  while (!san.empty()) {
    char c = san.back();
    if (c == '+' || c == '#' || c == '!' || c == '?') san.remove_suffix(1);
    else break;
  }
  if (san.size() < 2) throw Exception("SAN too short: " + std::string(san));

  const bool flipped = board.flipped();

  // Castling: king on rank 0 (our perspective), rook on our_*_rook file.
  if (san[0] == 'O' && san.size() >= 3 && san[1] == '-' && san[2] == 'O') {
    auto king_bb = board.kings() & board.ours();
    BoardSquare king_sq(lczero::GetLowestBit(king_bb.as_int()));
    bool queenside = (san.size() >= 5 && san[3] == '-' && san[4] == 'O');
    uint8_t rook_file = queenside ? board.castlings().our_queenside_rook()
                                  : board.castlings().our_kingside_rook();
    Move candidate(BoardSquare(0, king_sq.col()), BoardSquare(0, rook_file));
    auto legal = board.GenerateLegalMoves();
    if (std::find(legal.begin(), legal.end(), candidate) == legal.end()) {
      throw Exception("Illegal castling: " + std::string(san));
    }
    return candidate;
  }

  int p = 0;  // 0=pawn, 1=K, 2=Q, 3=B, 4=N, 5=R
  size_t idx = 0;
  switch (san[0]) {
    case 'K': p = 1; idx = 1; break;
    case 'Q': p = 2; idx = 1; break;
    case 'B': p = 3; idx = 1; break;
    case 'N': p = 4; idx = 1; break;
    case 'R': p = 5; idx = 1; break;
    default: break;
  }

  int r1 = -1, c1 = -1, r2 = -1, c2 = -1, p2 = -1;
  bool p_pending = false;
  for (; idx < san.size(); ++idx) {
    char ch = san[idx];
    if (ch == 'x' || ch == ':') continue;
    if (ch == '=') { p_pending = true; continue; }
    if (ch >= '1' && ch <= '8') { r1 = r2; r2 = ch - '1'; continue; }
    if (ch >= 'a' && ch <= 'h') { c1 = c2; c2 = ch - 'a'; continue; }
    if (p_pending) {
      switch (ch) {
        case 'Q': p2 = 2; break;
        case 'B': p2 = 3; break;
        case 'N': p2 = 4; break;
        case 'R': p2 = 5; break;
      }
      p_pending = false;
      break;
    }
    break;
  }
  if (r2 == -1 || c2 == -1) {
    throw Exception("SAN missing destination: " + std::string(san));
  }

  // Convert to the board's perspective.
  int br2 = flipped ? 7 - r2 : r2;
  int br1 = (r1 == -1) ? -1 : (flipped ? 7 - r1 : r1);

  BitBoard search_bits(0);
  switch (p) {
    case 0: search_bits = board.pawns() & board.ours(); break;
    case 1: search_bits = board.kings() & board.ours(); break;
    case 2: search_bits = board.queens() & board.ours(); break;
    case 3: search_bits = board.bishops() & board.ours(); break;
    case 4: search_bits = board.knights() & board.ours(); break;
    case 5: search_bits = board.rooks() & board.ours(); break;
  }

  auto legal = board.GenerateLegalMoves();
  int match_r = -1, match_c = -1;
  for (BoardSquare sq : search_bits) {
    if (br1 != -1 && sq.row() != br1) continue;
    if (c1  != -1 && sq.col() != c1)  continue;
    Move candidate(sq, BoardSquare(br2, c2), PieceToPromotion(p2));
    if (std::find(legal.begin(), legal.end(), candidate) == legal.end()) {
      continue;
    }
    if (match_c != -1) {
      throw Exception("Ambiguous SAN: " + std::string(san));
    }
    match_r = sq.row();
    match_c = sq.col();
  }
  if (match_c == -1) {
    throw Exception("Illegal SAN: " + std::string(san));
  }
  return Move(BoardSquare(match_r, match_c), BoardSquare(br2, c2),
              PieceToPromotion(p2));
}

}  // namespace pgn2pack
