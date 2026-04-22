#include "pack/record.h"

#include <cstring>
#include <string>

#include "chess/bitboard.h"
#include "chess/board.h"
#include "utils/bititer.h"

namespace pgn2pack {

using lczero::BitBoard;
using lczero::BoardSquare;
using lczero::ChessBoard;
using lczero::Position;

namespace {

// Piece codes (low 3 bits of each nibble).
enum : uint8_t { PIECE_P = 0, PIECE_N = 1, PIECE_B = 2,
                 PIECE_R = 3, PIECE_Q = 4, PIECE_K = 5 };

uint8_t PieceCodeAt(const ChessBoard& b, BoardSquare sq) {
  if (b.pawns().get(sq))   return PIECE_P;
  if (b.knights().get(sq)) return PIECE_N;
  if (b.bishops().get(sq)) return PIECE_B;
  if (b.rooks().get(sq))   return PIECE_R;
  if (b.queens().get(sq))  return PIECE_Q;
  return PIECE_K;
}

char PieceChar(uint8_t nibble) {
  static const char white[] = "PNBRQK";
  static const char black[] = "pnbrqk";
  uint8_t code = nibble & 0x07;
  return (nibble & 0x08) ? black[code] : white[code];
}

}  // namespace

uint8_t EncodeResult(const std::string& result_tag) {
  if (result_tag == "1-0")     return 1;
  if (result_tag == "0-1")     return 2;
  if (result_tag == "1/2-1/2") return 3;
  return 0;
}

void EncodePosition(const Position& pos, uint32_t game_id,
                    uint16_t ply_in_game, uint8_t result, uint8_t flags,
                    PackedRecord* out) {
  std::memset(out, 0, sizeof(*out));
  const ChessBoard& wb = pos.GetWhiteBoard();
  // Occupancy excludes the "fake" en-passant pawns (kPawnMask handled by
  // board.pawns()). `ours|theirs` already excludes them per lc0 invariants.
  BitBoard occupancy = wb.ours() | wb.theirs();
  out->occupancy = occupancy.as_int();

  // Pack nibbles LSB-first in the order of set bits.
  int nib_idx = 0;
  uint64_t bits = occupancy.as_int();
  while (bits) {
    unsigned long lsb = lczero::GetLowestBit(bits);
    bits &= bits - 1;
    BoardSquare sq((uint8_t)lsb);
    uint8_t code = PieceCodeAt(wb, sq);
    uint8_t color = wb.theirs().get(sq) ? 0x08 : 0x00;
    uint8_t nibble = code | color;
    if (nib_idx & 1) {
      out->piece_nibbles[nib_idx >> 1] |= (nibble << 4);
    } else {
      out->piece_nibbles[nib_idx >> 1] = nibble;
    }
    ++nib_idx;
  }

  const auto& c = wb.castlings();
  uint8_t castle = 0;
  if (c.we_can_00())    castle |= 0x1;  // K
  if (c.we_can_000())   castle |= 0x2;  // Q
  if (c.they_can_00())  castle |= 0x4;  // k
  if (c.they_can_000()) castle |= 0x8;  // q

  // FIDE-2001 legal-ep rule: only emit the ep file when a pawn can actually
  // perform the capture without leaving its own king in check. lc0's
  // `en_passant()` bitboard reflects a pseudo-legal flag (set on any two-push
  // that has an adjacent enemy pawn), so it can over-report. Filter through
  // GenerateLegalMoves in the side-to-move perspective instead.
  uint8_t ep_file = 0;
  bool ep_present = false;
  const ChessBoard& stm_board = pos.GetBoard();
  BitBoard ep_ghost = stm_board.en_passant();
  if (!ep_ghost.empty()) {
    const int ghost_col = (*ep_ghost.begin()).col();
    // In stm perspective: own pawn on row 4 (rank 5) captures diagonally to
    // row 5 (rank 6) on the ghost's column.
    for (const lczero::Move& mv : stm_board.GenerateLegalMoves()) {
      if (mv.to().row() == 5 && mv.to().col() == ghost_col &&
          mv.from().row() == 4 &&
          (stm_board.pawns() & stm_board.ours()).get(mv.from())) {
        ep_file = static_cast<uint8_t>(ghost_col);
        ep_present = true;
        break;
      }
    }
  }
  uint8_t stm = pos.IsBlackToMove() ? 1 : 0;
  out->stm_castle_ep = stm | (castle << 1) | ((ep_file & 0x7) << 5);

  out->rule50 = (uint8_t)pos.GetRule50Ply();
  int fullmove = (pos.GetGamePly() + (pos.IsBlackToMove() ? 1 : 2)) / 2;
  out->fullmove = (uint16_t)fullmove;
  out->game_id = game_id;
  out->ply_in_game = ply_in_game;
  out->result = result;
  // flags bit 0: caller-provided (non-standard startpos). Bit 1: ep present.
  out->flags = static_cast<uint8_t>((flags & 0x1) | (ep_present ? 0x2 : 0));
  out->reserved = 0;
}

std::string DecodeFen(const PackedRecord& rec) {
  uint8_t squares[64] = {0};
  bool present[64] = {false};
  uint64_t bits = rec.occupancy;
  int nib_idx = 0;
  while (bits) {
    unsigned long lsb = lczero::GetLowestBit(bits);
    bits &= bits - 1;
    uint8_t nibble = (nib_idx & 1)
        ? (rec.piece_nibbles[nib_idx >> 1] >> 4)
        : (rec.piece_nibbles[nib_idx >> 1] & 0x0F);
    squares[lsb] = nibble;
    present[lsb] = true;
    ++nib_idx;
  }

  std::string fen;
  for (int row = 7; row >= 0; --row) {
    int empty = 0;
    for (int col = 0; col < 8; ++col) {
      int sq = row * 8 + col;
      if (present[sq]) {
        if (empty) { fen += std::to_string(empty); empty = 0; }
        fen += PieceChar(squares[sq]);
      } else {
        ++empty;
      }
    }
    if (empty) fen += std::to_string(empty);
    if (row > 0) fen += '/';
  }

  uint8_t stm = rec.stm_castle_ep & 0x1;
  uint8_t castle = (rec.stm_castle_ep >> 1) & 0xF;
  uint8_t ep_file = (rec.stm_castle_ep >> 5) & 0x7;
  bool ep_present = (rec.flags & 0x2) != 0;

  fen += stm ? " b " : " w ";
  if (castle == 0) fen += '-';
  else {
    if (castle & 0x1) fen += 'K';
    if (castle & 0x2) fen += 'Q';
    if (castle & 0x4) fen += 'k';
    if (castle & 0x8) fen += 'q';
  }
  fen += ' ';
  if (!ep_present) fen += '-';
  else {
    // stm==white means black just moved: ep square on rank 6.
    // stm==black means white just moved: ep square on rank 3.
    int ep_row = stm ? 2 : 5;
    fen += char('a' + ep_file);
    fen += char('1' + ep_row);
  }
  fen += ' ';
  fen += std::to_string(rec.rule50);
  fen += ' ';
  fen += std::to_string(rec.fullmove);
  return fen;
}

}  // namespace pgn2pack
