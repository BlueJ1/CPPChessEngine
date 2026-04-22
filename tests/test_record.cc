#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>
#include <vector>

#include "chess/board.h"
#include "chess/position.h"
#include "pack/record.h"
#include "pgn/san.h"

using lczero::ChessBoard;
using lczero::Position;
using lczero::PositionHistory;
using pgn2pack::EncodePosition;
using pgn2pack::DecodeFen;
using pgn2pack::EncodeResult;
using pgn2pack::PackedRecord;

namespace {

Position PositionFromFen(const std::string& fen) {
  ChessBoard b;
  int rule50 = 0, fullmove = 1;
  b.SetFromFen(fen, &rule50, &fullmove);
  // SetFromFen writes the raw fullmove counter; Position expects half-moves
  // since game start. Mirror the conversion the reader does.
  int game_ply = (fullmove - 1) * 2 + (b.flipped() ? 1 : 0);
  return Position(b, rule50, game_ply);
}

PackedRecord Encode(const std::string& fen, uint32_t game_id = 0,
                    uint16_t ply = 0, uint8_t result = 0, uint8_t flags = 0) {
  PackedRecord rec{};
  EncodePosition(PositionFromFen(fen), game_id, ply, result, flags, &rec);
  return rec;
}

}  // namespace

TEST_CASE("PackedRecord is exactly 40 bytes", "[record]") {
  STATIC_REQUIRE(sizeof(PackedRecord) == 40);
}

TEST_CASE("EncodeResult maps tags correctly", "[record]") {
  REQUIRE(EncodeResult("1-0") == 1);
  REQUIRE(EncodeResult("0-1") == 2);
  REQUIRE(EncodeResult("1/2-1/2") == 3);
  REQUIRE(EncodeResult("*") == 0);
  REQUIRE(EncodeResult("") == 0);
  REQUIRE(EncodeResult("unknown") == 0);
}

TEST_CASE("DecodeFen round-trips startpos", "[record]") {
  const std::string fen = ChessBoard::kStartposFen;
  PackedRecord rec = Encode(fen);
  REQUIRE(DecodeFen(rec) == fen);
}

TEST_CASE("DecodeFen round-trips common positions", "[record]") {
  // Each FEN here must have fullmove=1 and no fabricated ep square — those
  // paths hit known bugs / lc0 quirks documented in their own tests.
  const std::vector<std::string> fens = {
      // Startpos.
      "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
      // Kiwipete.
      "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
      // Black-to-move variant of Kiwipete.
      "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R b KQkq - 0 1",
      // Position 3 (endgame, no castling).
      "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
      // Only white kingside.
      "r3k2r/8/8/8/8/8/8/R3K2R w K - 5 1",
      // Only black queenside.
      "r3k2r/8/8/8/8/8/8/R3K2R b q - 17 1",
      // Lots of promotions-possible pawn racing.
      "8/PPPPPPPP/8/4k3/4K3/8/pppppppp/8 w - - 0 1",
      // Many pieces of every type (stress piece-nibble packing).
      "1nbqkbnr/pppppppp/r7/8/8/R7/PPPPPPPP/1NBQKBNR w Kk - 3 1",
  };

  for (const auto& fen : fens) {
    CAPTURE(fen);
    PackedRecord rec = Encode(fen);
    REQUIRE(DecodeFen(rec) == fen);
  }
}

TEST_CASE("DecodeFen round-trips every castling-rights combo", "[record]") {
  const char* tpl =
      "r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w %s - 0 1";
  const std::vector<std::string> rights = {
      "KQkq", "KQk", "KQq", "KQ", "Kkq", "Kk", "Kq", "K",
      "Qkq", "Qk", "Qq", "Q", "kq", "k", "q", "-",
  };
  for (const auto& r : rights) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), tpl, r.c_str());
    std::string fen = buf;
    CAPTURE(fen);
    PackedRecord rec = Encode(fen);
    REQUIRE(DecodeFen(rec) == fen);
  }
}

TEST_CASE("ep square round-trips through encode/decode (real game lines)",
          "[record]") {
  // Build positions by play (avoids the SetFromFen fullmove/ep quirks).
  auto play = [](const std::vector<std::string>& sans) {
    ChessBoard b;
    int rule50 = 0, game_ply = 0;
    b.SetFromFen(ChessBoard::kStartposFen, &rule50, &game_ply);
    PositionHistory h;
    h.Reset(b, rule50, game_ply);
    for (const auto& s : sans) {
      h.Append(pgn2pack::SanToMove(s, h.Last().GetBoard()));
    }
    return h.Last();
  };

  struct Case { std::vector<std::string> moves; std::string ep_sq; };
  const std::vector<Case> cases = {
      // After 1.Nc3 e5 2.Nf3 e4 3.d4 → black-to-move, ep d3.
      {{"Nc3", "e5", "Nf3", "e4", "d4"}, "d3"},
      // After 1.e4 Nf6 2.e5 d5 → white-to-move, ep d6.
      {{"e4", "Nf6", "e5", "d5"}, "d6"},
  };
  for (const auto& c : cases) {
    CAPTURE(c.ep_sq);
    PackedRecord rec;
    EncodePosition(play(c.moves), 0, 0, 0, 0, &rec);
    std::string fen = DecodeFen(rec);
    REQUIRE(fen.find(std::string(" ") + c.ep_sq + " ") != std::string::npos);
  }
}

TEST_CASE("EncodePosition stores caller-provided metadata verbatim",
          "[record]") {
  PackedRecord rec =
      Encode(ChessBoard::kStartposFen,
             /*game_id=*/0xDEADBEEFu, /*ply=*/1234,
             /*result=*/2, /*flags=*/0x01);
  REQUIRE(rec.game_id == 0xDEADBEEFu);
  REQUIRE(rec.ply_in_game == 1234);
  REQUIRE(rec.result == 2);
  REQUIRE(rec.flags == 0x01);
  REQUIRE(rec.reserved == 0u);
}

TEST_CASE("Startpos produces exactly 32 occupied squares", "[record]") {
  PackedRecord rec = Encode(ChessBoard::kStartposFen);
  // First and last two ranks occupied => 32 bits.
  REQUIRE(__builtin_popcountll(rec.occupancy) == 32);
  // Ranks 1, 2, 7, 8 fully set.
  const uint64_t expected =
      0x000000000000FFFFull |  // ranks 1-2
      0xFFFF000000000000ull;   // ranks 7-8
  REQUIRE(rec.occupancy == expected);
}

TEST_CASE("rule50 edge values survive round-trip", "[record]") {
  // lc0's SetFromFen accepts rule50 as an int; PackedRecord stores uint8_t.
  // Anything >255 would be truncated — so check values that fit.
  // Use fullmove=1 to avoid the known fullmove bug.
  for (int r : {0, 1, 49, 50, 99, 100, 200, 254, 255}) {
    std::string fen =
        "8/8/4k3/8/8/4K3/8/8 w - - " + std::to_string(r) + " 1";
    CAPTURE(fen);
    PackedRecord rec = Encode(fen);
    REQUIRE(static_cast<int>(rec.rule50) == r);
  }
}

TEST_CASE("rule50 high value survives round-trip to DecodeFen", "[record]") {
  const std::string fen = "8/8/4k3/8/8/4K3/8/8 w - - 99 1";
  REQUIRE(DecodeFen(Encode(fen)) == fen);
}

TEST_CASE("Fullmove counter tracks correctly when replaying from startpos",
          "[record]") {
  // Build positions by Append()-ing from startpos and confirm fullmove.
  ChessBoard b;
  int rule50 = 0, game_ply = 0;
  b.SetFromFen(ChessBoard::kStartposFen, &rule50, &game_ply);
  PositionHistory h;
  h.Reset(b, rule50, game_ply);
  // Startpos: fullmove 1.
  {
    PackedRecord rec;
    EncodePosition(h.Last(), 0, 0, 0, 0, &rec);
    REQUIRE(rec.fullmove == 1);
  }
  // 1.e4 → black to move, fullmove still 1.
  {
    auto mv = pgn2pack::SanToMove("e4", h.Last().GetBoard());
    h.Append(mv);
    PackedRecord rec;
    EncodePosition(h.Last(), 0, 1, 0, 0, &rec);
    REQUIRE(rec.fullmove == 1);
  }
  // 1... e5 → white to move, fullmove 2.
  {
    auto mv = pgn2pack::SanToMove("e5", h.Last().GetBoard());
    h.Append(mv);
    PackedRecord rec;
    EncodePosition(h.Last(), 0, 2, 0, 0, &rec);
    REQUIRE(rec.fullmove == 2);
  }
}

TEST_CASE("Fullmove survives when starting from a FEN with fullmove != 1",
          "[record]") {
  // Regression: lczero::ChessBoard::SetFromFen stores the raw fullmove number
  // into its second out-param. The reader converts that into a half-move count
  // before handing it to PositionHistory::Reset, so round-tripping via
  // PositionFromFen preserves the fullmove both for white- and black-to-move.
  const std::string fen_w = "8/8/4k3/8/8/4K3/8/8 w - - 0 50";
  const std::string fen_b = "8/8/4k3/8/8/4K3/8/8 b - - 0 50";
  PackedRecord rec_w = Encode(fen_w);
  PackedRecord rec_b = Encode(fen_b);
  REQUIRE(static_cast<int>(rec_w.fullmove) == 50);
  REQUIRE(static_cast<int>(rec_b.fullmove) == 50);
}

TEST_CASE("Side-to-move bit encodes correctly", "[record]") {
  PackedRecord w = Encode("8/8/4k3/8/8/4K3/8/8 w - - 0 1");
  PackedRecord b = Encode("8/8/4k3/8/8/4K3/8/8 b - - 0 1");
  REQUIRE((w.stm_castle_ep & 0x1) == 0);
  REQUIRE((b.stm_castle_ep & 0x1) == 1);
}

TEST_CASE("All 12 piece types appear with correct nibble color/type",
          "[record]") {
  // Build a position containing all 12 piece kinds at known squares, then
  // verify DecodeFen's piece placement matches.
  const std::string fen =
      "r1bq1rk1/pppp1ppp/2n2n2/2b1p3/2B1P3/2N2N2/PPPP1PPP/R1BQ1RK1 w - - 0 1";
  PackedRecord rec = Encode(fen);
  REQUIRE(DecodeFen(rec) == fen);
}
