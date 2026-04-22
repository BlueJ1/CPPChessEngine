#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "chess/board.h"
#include "chess/position.h"
#include "pgn/san.h"
#include "utils/exception.h"

using lczero::ChessBoard;
using lczero::Move;
using lczero::Position;
using lczero::PositionHistory;
using pgn2pack::SanToMove;

namespace {

PositionHistory HistoryFromFen(const std::string& fen) {
  ChessBoard b;
  int rule50 = 0, game_ply = 0;
  b.SetFromFen(fen, &rule50, &game_ply);
  PositionHistory h;
  h.Reset(b, rule50, game_ply);
  return h;
}

// Apply a single SAN to a starting FEN, return the resulting position FEN.
std::string ApplyOne(const std::string& fen, const std::string& san) {
  PositionHistory h = HistoryFromFen(fen);
  Move m = SanToMove(san, h.Last().GetBoard());
  h.Append(m);
  return GetFen(h.Last());
}

std::string ApplyGame(const std::string& start_fen,
                      const std::vector<std::string>& sans) {
  PositionHistory h = HistoryFromFen(start_fen);
  for (const auto& san : sans) {
    Move m = SanToMove(san, h.Last().GetBoard());
    h.Append(m);
  }
  return GetFen(h.Last());
}

}  // namespace

TEST_CASE("Pawn single and double pushes", "[san]") {
  const std::string sp = ChessBoard::kStartposFen;
  // lc0 only records ep in the FEN when the capture is actually legal (a
  // black pawn must stand adjacent). After 1.e4 from startpos there is no
  // adjacent black pawn, so ep is "-".
  REQUIRE(ApplyOne(sp, "e4") ==
          "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1");
  REQUIRE(ApplyOne(sp, "e3") ==
          "rnbqkbnr/pppppppp/8/8/8/4P3/PPPP1PPP/RNBQKBNR b KQkq - 0 1");
  REQUIRE(ApplyOne(sp, "Nf3") ==
          "rnbqkbnr/pppppppp/8/8/8/5N2/PPPPPPPP/RNBQKB1R b KQkq - 1 1");
}

TEST_CASE("Double push with adjacent pawn on the capture-rank sets ep square",
          "[san]") {
  // ep only legal when the capturing pawn is already on rank 4 (for black)
  // or rank 5 (for white). After 1.Nc3 e5 2.Nf3 e4 3.d4 the black e4 pawn
  // can capture d3 ep.
  std::string fen = ApplyGame(
      ChessBoard::kStartposFen, {"Nc3", "e5", "Nf3", "e4", "d4"});
  REQUIRE(fen.find(" d3 ") != std::string::npos);
  // And black can actually execute exd3.
  std::string after_ep = ApplyOne(fen, "exd3");
  REQUIRE(after_ep.find('P') != std::string::npos);  // black captured pawn, white P on d3? no
  // Verify the d-pawn is gone (white's d-file is empty except on rank 2).
  std::string pp = after_ep.substr(0, after_ep.find(' '));
  // Expand and check column d (index 3) for non-empty squares.
  auto rank_at = [&](int r) {
    size_t pos = 0;
    for (int i = 0; i < r; ++i) pos = pp.find('/', pos) + 1;
    size_t end = pp.find('/', pos);
    std::string row = pp.substr(pos, end == std::string::npos ? end : end - pos);
    std::string e;
    for (char c : row) {
      if (c >= '1' && c <= '8') e.append(c - '0', '.');
      else e.push_back(c);
    }
    return e;
  };
  REQUIRE(rank_at(4)[3] == '.');      // d4 empty (white pawn captured ep)
  REQUIRE(rank_at(5)[3] == 'p');      // black pawn now on d3 (rank 3, index 5)
}

TEST_CASE("Pawn captures", "[san]") {
  const std::string after_e4_d5 =
      "rnbqkbnr/ppp1pppp/8/3p4/4P3/8/PPPP1PPP/RNBQKBNR w KQkq d6 0 2";
  // exd5
  REQUIRE(ApplyOne(after_e4_d5, "exd5") ==
          "rnbqkbnr/ppp1pppp/8/3P4/8/8/PPPP1PPP/RNBQKBNR b KQkq - 0 2");
}

TEST_CASE("En passant capture", "[san]") {
  // 1.e4 Nf6 2.e5 d5 → 3.exd6 captures en passant.
  const std::string fen =
      ApplyGame(ChessBoard::kStartposFen, {"e4", "Nf6", "e5", "d5"});
  // Sanity: ep square is present after 2...d5.
  REQUIRE(fen.find(" d6 ") != std::string::npos);
  const std::string after = ApplyOne(fen, "exd6");
  // d-pawn captured, white pawn now on d6, d5 empty.
  std::string pp = after.substr(0, after.find(' '));
  // Expand for easy inspection.
  std::string expanded;
  for (char c : pp) {
    if (c >= '1' && c <= '8') expanded.append(c - '0', '.');
    else expanded.push_back(c);
  }
  // Rank 6 (index 2 after split by '/'), col 3 (d) should be 'P'.
  auto rank_at = [&](int r) {
    size_t pos = 0;
    for (int i = 0; i < r; ++i) pos = pp.find('/', pos) + 1;
    size_t end = pp.find('/', pos);
    std::string row = pp.substr(pos, end == std::string::npos ? end : end - pos);
    std::string e;
    for (char c : row) {
      if (c >= '1' && c <= '8') e.append(c - '0', '.');
      else e.push_back(c);
    }
    return e;
  };
  REQUIRE(rank_at(2)[3] == 'P');        // white pawn on d6
  REQUIRE(rank_at(3)[3] == '.');        // d5 empty (captured pawn removed)
}

TEST_CASE("All four promotion pieces (no capture)", "[san]") {
  // White pawn on a7 ready to promote, minimal material.
  const std::string fen = "4k3/P7/8/8/8/8/8/4K3 w - - 0 1";
  struct Case { std::string san; char piece; };
  for (const auto& c : std::vector<Case>{{"a8=Q", 'Q'}, {"a8=R", 'R'},
                                         {"a8=B", 'B'}, {"a8=N", 'N'}}) {
    CAPTURE(c.san);
    std::string got = ApplyOne(fen, c.san);
    // Must begin with the promoted piece on a8.
    REQUIRE(got.substr(0, 1) == std::string(1, c.piece));
  }
}

TEST_CASE("Promotion with capture", "[san]") {
  // bxa8=Q: white b7 pawn captures on a8 (must be a black piece there).
  const std::string fen = "r3k3/1P6/8/8/8/8/8/4K3 w - - 0 1";
  std::string got = ApplyOne(fen, "bxa8=Q");
  std::string pp = got.substr(0, got.find(' '));
  REQUIRE(pp.substr(0, 4) == "Q3k3");   // rank 8: Qa8 + empty b-d + Ke8
  REQUIRE(pp.find('P') == std::string::npos);  // b7 pawn is gone
}

TEST_CASE("Kingside and queenside castling, both colors", "[san]") {
  // A position where both sides have full castling rights and all pieces
  // between king and rook are cleared.
  const std::string fen =
      "r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w KQkq - 0 1";
  REQUIRE(ApplyOne(fen, "O-O") ==
          "r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R4RK1 b kq - 1 1");
  REQUIRE(ApplyOne(fen, "O-O-O") ==
          "r3k2r/pppppppp/8/8/8/8/PPPPPPPP/2KR3R b kq - 1 1");

  const std::string fen_b =
      "r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R b KQkq - 0 1";
  REQUIRE(ApplyOne(fen_b, "O-O") ==
          "r4rk1/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w KQ - 1 2");
  REQUIRE(ApplyOne(fen_b, "O-O-O") ==
          "2kr3r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w KQ - 1 2");
}

TEST_CASE("Castling rights lost by king move", "[san]") {
  // After Kf1 from startpos-shaped castling-rights position, white loses
  // both K and Q.
  const std::string fen =
      "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1";
  std::string got = ApplyOne(fen, "Kf1");
  REQUIRE(got.find(" kq ") != std::string::npos);
  REQUIRE(got.find(" K") == std::string::npos);
}

TEST_CASE("Castling rights lost by rook capture", "[san]") {
  // White rook takes black rook on a8 — black loses queenside castling.
  const std::string fen =
      "r3k2r/R7/8/8/8/8/8/4K2R w Kkq - 0 1";
  std::string got = ApplyOne(fen, "Rxa8");
  REQUIRE(got.find(" Kk ") != std::string::npos);   // black 'q' lost
}

TEST_CASE("File disambiguation (two knights to same square)", "[san]") {
  // Two white knights on b1 and f3 can both go to d2 — no, b1 can't go to d2
  // (b1 -> d2 yes, f3 -> d2 yes). Construct explicitly.
  // Knights on b1 and f1 can both reach d2.
  // Use: knights on b1, d1 both can reach c3? b1->c3 yes, d1->c3 no (d1->c3 is
  // a knight move from d1? d1+{-2,+2} or {-2,-2}... d1 to c3 is (-1,+2) nope,
  // knights move ±1/±2. d1 to c3 = (-1,+2) — not a knight move. Let's use
  // knights on b1 and d1, both go to c3? only b1 can. Use b1 and f3 both to
  // d2: b1->d2 (+2,+1) yes, f3->d2 (-2,-1) yes. Good.
  const std::string fen =
      "4k3/8/8/8/8/5N2/8/1N2K3 w - - 0 1";
  // Nbd2 -> knight from b-file.
  std::string got_b = ApplyOne(fen, "Nbd2");
  // Nfd2 -> knight from f-file.
  std::string got_f = ApplyOne(fen, "Nfd2");
  REQUIRE(got_b != got_f);
  // Bare "Nd2" should be ambiguous → throw.
  PositionHistory h = HistoryFromFen(fen);
  REQUIRE_THROWS(SanToMove("Nd2", h.Last().GetBoard()));
}

TEST_CASE("Rank disambiguation (two rooks on same file)", "[san]") {
  // Rooks on e1 and e5; both can move to e3. Disambiguate by rank.
  const std::string fen =
      "4k3/8/8/4R3/8/8/8/4R1K1 w - - 0 1";
  std::string g1 = ApplyOne(fen, "R1e3");
  std::string g5 = ApplyOne(fen, "R5e3");
  REQUIRE(g1 != g5);
  PositionHistory h = HistoryFromFen(fen);
  REQUIRE_THROWS(SanToMove("Re3", h.Last().GetBoard()));
}

TEST_CASE("File+rank disambiguation (three queens)", "[san]") {
  // Three white queens: a1, a4, d1. All can reach a3:
  //   a1 -> a3 (same file)
  //   a4 -> a3 (same file)
  //   d1 -> a4 (no), d1 -> a1 (diagonal? no a1 is occupied) — need different.
  // Pick target where all three reach via ambiguous row+file combos.
  // Use queens on a1, h1, a8. Target: a1. That's not fresh.
  // Standard example (from FIDE): queens on h4, e1, h1 all reach e4.
  //   h4 -> e4 (rank), e1 -> e4 (file), h1 -> e4 (diagonal). YES.
  // Qh4e4: uses file 'h' and rank '4' — still ambiguous with nothing, but
  // unique here. Qh1e4 similarly, Qe1e4 too.
  const std::string fen =
      "4k3/8/8/8/7Q/8/8/4Q2Q w - - 0 1";
  // h4->e4 unambiguous with file 'h': 'Qhe4'? h1 also on h file -> still
  // ambiguous between h4 and h1. Need file+rank: Qh4e4.
  std::string gh4 = ApplyOne(fen, "Qh4e4");
  std::string gh1 = ApplyOne(fen, "Qh1e4");
  std::string ge1 = ApplyOne(fen, "Qe1e4");
  REQUIRE(gh4 != gh1);
  REQUIRE(gh1 != ge1);
  REQUIRE(gh4 != ge1);
  PositionHistory h = HistoryFromFen(fen);
  REQUIRE_THROWS(SanToMove("Qe4", h.Last().GetBoard()));
  REQUIRE_THROWS(SanToMove("Qhe4", h.Last().GetBoard()));  // still ambiguous
  REQUIRE_THROWS(SanToMove("Q1e4", h.Last().GetBoard()));  // still ambiguous
}

TEST_CASE("Check/mate and annotation suffixes are tolerated", "[san]") {
  // Fool's mate: 1.f3 e5 2.g4 Qh4# — parse final move with '#'.
  std::string fen = ApplyGame(ChessBoard::kStartposFen,
                              {"f3", "e5", "g4", "Qh4#"});
  // Black just moved, so white is to move (and mated).
  REQUIRE(fen.find(" w ") != std::string::npos);
  // Sanity: black queen on h4.
  REQUIRE(fen.find("q") != std::string::npos);

  // '+' tolerated on a non-mating check.
  const std::string before =
      "rnbqkbnr/ppp2ppp/8/3pp3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 3";
  REQUIRE_NOTHROW(
      SanToMove("Bb5+", HistoryFromFen(before).Last().GetBoard()));

  // '!' / '?' annotations.
  REQUIRE_NOTHROW(
      SanToMove("Bb5!", HistoryFromFen(before).Last().GetBoard()));
  REQUIRE_NOTHROW(
      SanToMove("Bb5?!", HistoryFromFen(before).Last().GetBoard()));
}

TEST_CASE("Illegal SAN throws", "[san]") {
  PositionHistory h = HistoryFromFen(ChessBoard::kStartposFen);
  REQUIRE_THROWS(SanToMove("e5", h.Last().GetBoard()));     // pawn too far
  REQUIRE_THROWS(SanToMove("Ke2", h.Last().GetBoard()));    // blocked
  REQUIRE_THROWS(SanToMove("Qh5", h.Last().GetBoard()));    // blocked
  REQUIRE_THROWS(SanToMove("x", h.Last().GetBoard()));      // nonsense
  REQUIRE_THROWS(SanToMove("", h.Last().GetBoard()));       // empty
}

TEST_CASE("SanToMove rejects illegal castling", "[san]") {
  // From the startpos, O-O is plainly illegal — the knight, bishop and other
  // pieces occupy the path. SanToMove must reject it via GenerateLegalMoves.
  PositionHistory h = HistoryFromFen(ChessBoard::kStartposFen);
  REQUIRE_THROWS(SanToMove("O-O", h.Last().GetBoard()));
  REQUIRE_THROWS(SanToMove("O-O-O", h.Last().GetBoard()));
  // Castling through check is illegal too.
  PositionHistory h2 = HistoryFromFen(
      "r3k2r/pppppppp/8/8/8/8/PPPPrPPP/R3K2R w KQkq - 0 1");
  REQUIRE_THROWS(SanToMove("O-O", h2.Last().GetBoard()));
}

TEST_CASE("Full Immortal-game SAN parse produces expected mating FEN",
          "[san]") {
  // Anderssen–Kieseritzky 1851, full 23-move sequence.
  const std::vector<std::string> moves = {
    "e4", "e5", "f4", "exf4", "Bc4", "Qh4+", "Kf1", "b5", "Bxb5", "Nf6",
    "Nf3", "Qh6", "d3", "Nh5", "Nh4", "Qg5", "Nf5", "c6", "g4", "Nf6",
    "Rg1", "cxb5", "h4", "Qg6", "h5", "Qg5", "Qf3", "Ng8", "Bxf4", "Qf6",
    "Nc3", "Bc5", "Nd5", "Qxb2", "Bd6", "Bxg1", "e5", "Qxa1+", "Ke2", "Na6",
    "Nxg7+", "Kd8", "Qf6+", "Nxf6", "Be7#",
  };
  std::string fen = ApplyGame(ChessBoard::kStartposFen, moves);
  // Final position: black is checkmated. Side to move is black.
  REQUIRE(fen.find(" b ") != std::string::npos);
  // The mating bishop should be on e7.
  // Piece-placement field is everything before the first space.
  std::string pp = fen.substr(0, fen.find(' '));
  // rank 7 is the second '/'-separated field.
  size_t s1 = pp.find('/');
  size_t s2 = pp.find('/', s1 + 1);
  std::string rank7 = pp.substr(s1 + 1, s2 - s1 - 1);
  // e7 is column 4 (0-indexed). Expand runs of digits.
  std::string expanded;
  for (char c : rank7) {
    if (c >= '1' && c <= '8') expanded.append(c - '0', '.');
    else expanded.push_back(c);
  }
  REQUIRE(expanded.size() == 8);
  REQUIRE(expanded[4] == 'B');
}
