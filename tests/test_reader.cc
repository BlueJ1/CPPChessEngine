#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <unistd.h>
#include <fstream>
#include <string>
#include <vector>

#include <zstd.h>

#include "chess/board.h"
#include "chess/position.h"
#include "pack/record.h"
#include "pgn/reader.h"

using pgn2pack::GameHeader;
using pgn2pack::StreamPgnFile;
using lczero::PositionHistory;

namespace fs = std::filesystem;

namespace {

struct TempFile {
  fs::path path;
  explicit TempFile(const std::string& suffix) {
    auto dir = fs::temp_directory_path();
    char buf[64];
    std::snprintf(buf, sizeof(buf), "pgn2pack_%d_%p",
                  (int)::getpid(), (void*)this);
    path = dir / (std::string(buf) + suffix);
  }
  ~TempFile() {
    std::error_code ec;
    fs::remove(path, ec);
  }
  void WritePlain(const std::string& data) {
    std::ofstream(path, std::ios::binary).write(data.data(), data.size());
  }
  void WriteZstd(const std::string& data) {
    std::vector<char> compressed(ZSTD_compressBound(data.size()));
    size_t n = ZSTD_compress(compressed.data(), compressed.size(),
                             data.data(), data.size(), 3);
    REQUIRE_FALSE(ZSTD_isError(n));
    std::ofstream(path, std::ios::binary).write(compressed.data(), n);
  }
};

struct Capture {
  std::vector<uint16_t> per_ply_ids;     // game_id per ply
  std::vector<uint16_t> per_ply_nums;    // ply_in_game per ply
  std::vector<std::string> per_game_fens_last;  // FEN of last ply per game
  std::vector<GameHeader> game_headers;
  std::vector<uint32_t> game_ids;
  std::vector<uint16_t> plies_per_game;

  auto OnPly() {
    return [this](const GameHeader&, const PositionHistory& h,
                  uint32_t gid, uint16_t ply) {
      per_ply_ids.push_back((uint16_t)gid);
      per_ply_nums.push_back(ply);
      last_fen_ = GetFen(h.Last());
    };
  }
  auto OnGame() {
    return [this](const GameHeader& hdr, uint32_t gid, uint16_t plies) {
      per_game_fens_last.push_back(last_fen_);
      game_headers.push_back(hdr);
      game_ids.push_back(gid);
      plies_per_game.push_back(plies);
      last_fen_.clear();
    };
  }
 private:
  std::string last_fen_;
};

const std::string kTwoTrivialGames =
R"([Event "g1"]
[Result "1-0"]

1. e4 e5 1-0

[Event "g2"]
[Result "0-1"]

1. d4 d5 0-1
)";

}  // namespace

TEST_CASE("Reader parses plain PGN with multiple games", "[reader]") {
  TempFile tf(".pgn");
  tf.WritePlain(kTwoTrivialGames);
  Capture cap;
  uint64_t n = StreamPgnFile(tf.path.string(), 0, cap.OnPly(), cap.OnGame());
  REQUIRE(n == 2);
  REQUIRE(cap.game_ids == std::vector<uint32_t>{0, 1});
  REQUIRE(cap.plies_per_game == std::vector<uint16_t>{2, 2});
  REQUIRE(cap.game_headers[0].event == "g1");
  REQUIRE(cap.game_headers[0].result == "1-0");
  REQUIRE(cap.game_headers[1].event == "g2");
  REQUIRE(cap.game_headers[1].result == "0-1");
}

TEST_CASE("Reader handles zstd-compressed PGN", "[reader]") {
  TempFile tf(".pgn.zst");
  tf.WriteZstd(kTwoTrivialGames);
  Capture cap;
  uint64_t n = StreamPgnFile(tf.path.string(), 0, cap.OnPly(), cap.OnGame());
  REQUIRE(n == 2);
  REQUIRE(cap.plies_per_game == std::vector<uint16_t>{2, 2});
}

TEST_CASE("game_id_base offsets the assigned game_id", "[reader]") {
  TempFile tf(".pgn");
  tf.WritePlain(kTwoTrivialGames);
  Capture cap;
  uint64_t n = StreamPgnFile(tf.path.string(), 100, cap.OnPly(), cap.OnGame());
  REQUIRE(n == 2);
  REQUIRE(cap.game_ids == std::vector<uint32_t>{100, 101});
}

TEST_CASE("ply_in_game is 0-based and monotonic within a game", "[reader]") {
  TempFile tf(".pgn");
  tf.WritePlain(kTwoTrivialGames);
  Capture cap;
  StreamPgnFile(tf.path.string(), 0, cap.OnPly(), cap.OnGame());
  // Two games, 2 plies each. Ply indices restart per game.
  REQUIRE(cap.per_ply_nums == std::vector<uint16_t>{0, 1, 0, 1});
  REQUIRE(cap.per_ply_ids == std::vector<uint16_t>{0, 0, 1, 1});
}

TEST_CASE("Comments, NAGs, and variations are stripped", "[reader]") {
  const std::string pgn =
R"([Event "test"]
[Result "1-0"]

1. e4 {a comment} e5 ; rest-of-line comment
2. Nf3 $14 $6 (2. Nc3 Nc6) Nc6 $1 3. Bb5 a6 1-0
)";
  TempFile tf(".pgn");
  tf.WritePlain(pgn);
  Capture cap;
  uint64_t n = StreamPgnFile(tf.path.string(), 0, cap.OnPly(), cap.OnGame());
  REQUIRE(n == 1);
  // Main-line plies only: e4 e5 Nf3 Nc6 Bb5 a6 = 6.
  REQUIRE(cap.plies_per_game == std::vector<uint16_t>{6});
}

TEST_CASE("[FEN] header yields start_fen on the header", "[reader]") {
  // A crafted position with white to move from a non-standard FEN.
  const std::string pgn =
R"([Event "fen-test"]
[Result "*"]
[SetUp "1"]
[FEN "4k3/8/8/8/8/8/4P3/4K3 w - - 0 1"]

1. e4 *
)";
  TempFile tf(".pgn");
  tf.WritePlain(pgn);
  Capture cap;
  uint64_t n = StreamPgnFile(tf.path.string(), 0, cap.OnPly(), cap.OnGame());
  REQUIRE(n == 1);
  REQUIRE(cap.game_headers[0].start_fen ==
          "4k3/8/8/8/8/8/4P3/4K3 w - - 0 1");
  REQUIRE(cap.plies_per_game == std::vector<uint16_t>{1});
}

TEST_CASE("Broken game is skipped, stream continues", "[reader]") {
  const std::string pgn =
R"([Event "bad"]
[Result "1-0"]

1. e4 Nf6 2. totally_not_san 1-0

[Event "good"]
[Result "0-1"]

1. d4 d5 0-1
)";
  TempFile tf(".pgn");
  tf.WritePlain(pgn);
  Capture cap;
  uint64_t n = StreamPgnFile(tf.path.string(), 0, cap.OnPly(), cap.OnGame());
  REQUIRE(n == 1);
  REQUIRE(cap.game_headers.size() == 1);
  REQUIRE(cap.game_headers[0].event == "good");
}

TEST_CASE("Elo tags are parsed and bad values are zero", "[reader]") {
  const std::string pgn =
R"([Event "elo-test"]
[Result "1/2-1/2"]
[WhiteElo "2400"]
[BlackElo "?"]
[UTCDate "2024.01.02"]
[TimeControl "600+5"]

1. e4 e5 1/2-1/2
)";
  TempFile tf(".pgn");
  tf.WritePlain(pgn);
  Capture cap;
  StreamPgnFile(tf.path.string(), 0, cap.OnPly(), cap.OnGame());
  REQUIRE(cap.game_headers.size() == 1);
  REQUIRE(cap.game_headers[0].white_elo == 2400);
  REQUIRE(cap.game_headers[0].black_elo == 0);
  REQUIRE(cap.game_headers[0].utc_date == "2024.01.02");
  REQUIRE(cap.game_headers[0].time_control == "600+5");
  REQUIRE(cap.game_headers[0].result == "1/2-1/2");
}

TEST_CASE("Trailing game with no result terminator is flushed", "[reader]") {
  const std::string pgn =
R"([Event "unfinished"]
[Result "*"]

1. e4 e5
)";
  TempFile tf(".pgn");
  tf.WritePlain(pgn);
  Capture cap;
  uint64_t n = StreamPgnFile(tf.path.string(), 0, cap.OnPly(), cap.OnGame());
  REQUIRE(n == 1);
  REQUIRE(cap.plies_per_game == std::vector<uint16_t>{2});
}

TEST_CASE("Sample fixture PGN parses all five games", "[reader][fixture]") {
  std::string fixture =
      std::string(PGN2PACK_TEST_DATA_DIR) + "/sample.pgn";
  Capture cap;
  uint64_t n = StreamPgnFile(fixture, 0, cap.OnPly(), cap.OnGame());
  REQUIRE(n == 5);
  // Fifth game ("Promotion test") ends with bxa1=Q — white to move after.
  const std::string& last = cap.per_game_fens_last[4];
  REQUIRE(last.find(" w ") != std::string::npos);
  std::string pp = last.substr(0, last.find(' '));
  std::string rank1 = pp.substr(pp.rfind('/') + 1);
  std::string expanded;
  for (char c : rank1) {
    if (c >= '1' && c <= '8') expanded.append(c - '0', '.');
    else expanded.push_back(c);
  }
  REQUIRE(expanded.size() == 8);
  REQUIRE(expanded[0] == 'q');  // black queen on a1
}
