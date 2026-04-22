#include "pgn/reader.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <zstd.h>

#include "chess/board.h"
#include "chess/position.h"
#include "pgn/san.h"
#include "utils/exception.h"

namespace pgn2pack {

namespace {

// Reads a compressed or uncompressed file into a line stream without
// buffering the whole file.
class LineReader {
 public:
  LineReader(const std::string& path) {
    file_ = std::fopen(path.c_str(), "rb");
    if (!file_) {
      throw lczero::Exception("Cannot open " + path + ": " + std::strerror(errno));
    }
    is_zstd_ = path.size() >= 4 &&
               path.compare(path.size() - 4, 4, ".zst") == 0;
    if (is_zstd_) {
      dstream_ = ZSTD_createDStream();
      if (!dstream_) throw lczero::Exception("ZSTD_createDStream failed.");
      ZSTD_initDStream(dstream_);
      in_buf_.resize(ZSTD_DStreamInSize());
      out_buf_.resize(ZSTD_DStreamOutSize());
    } else {
      out_buf_.resize(1 << 16);
    }
    line_.reserve(256);
  }

  ~LineReader() {
    if (dstream_) ZSTD_freeDStream(dstream_);
    if (file_) std::fclose(file_);
  }

  // Returns a non-owning view of the next line (newline stripped). Returns
  // nullopt (as empty-flag false) when the stream is exhausted.
  bool NextLine(std::string_view* out) {
    line_.clear();
    while (true) {
      // Scan current output window for newline.
      while (out_pos_ < out_end_) {
        char c = out_buf_[out_pos_++];
        if (c == '\n') {
          if (!line_.empty() && line_.back() == '\r') line_.pop_back();
          *out = line_;
          return true;
        }
        line_.push_back(c);
      }
      // Refill output window.
      if (!RefillOutput()) {
        if (line_.empty()) return false;
        if (!line_.empty() && line_.back() == '\r') line_.pop_back();
        *out = line_;
        return true;
      }
    }
  }

 private:
  bool RefillOutput() {
    out_pos_ = 0;
    out_end_ = 0;
    if (is_zstd_) {
      while (true) {
        if (in_pos_ >= in_end_) {
          in_pos_ = 0;
          in_end_ = std::fread(in_buf_.data(), 1, in_buf_.size(), file_);
          if (in_end_ == 0) return false;
        }
        ZSTD_inBuffer zin{in_buf_.data(), in_end_, in_pos_};
        ZSTD_outBuffer zout{out_buf_.data(), out_buf_.size(), 0};
        size_t ret = ZSTD_decompressStream(dstream_, &zout, &zin);
        if (ZSTD_isError(ret)) {
          throw lczero::Exception(std::string("zstd: ") +
                                  ZSTD_getErrorName(ret));
        }
        in_pos_ = zin.pos;
        out_end_ = zout.pos;
        if (out_end_ > 0) return true;
        // Need more input.
      }
    } else {
      out_end_ = std::fread(out_buf_.data(), 1, out_buf_.size(), file_);
      return out_end_ > 0;
    }
  }

  std::FILE* file_ = nullptr;
  ZSTD_DStream* dstream_ = nullptr;
  bool is_zstd_ = false;
  std::vector<char> in_buf_;
  std::vector<char> out_buf_;
  size_t in_pos_ = 0, in_end_ = 0;
  size_t out_pos_ = 0, out_end_ = 0;
  std::string line_;
};

// Parses `[Tag "Value"]`. Returns false on malformed.
bool ParseTag(std::string_view line, std::string_view* tag,
              std::string_view* value) {
  if (line.empty() || line.front() != '[') return false;
  size_t i = 1;
  while (i < line.size() && line[i] == ' ') ++i;
  size_t tag_start = i;
  while (i < line.size() && line[i] != ' ' && line[i] != '"') ++i;
  if (i >= line.size()) return false;
  *tag = line.substr(tag_start, i - tag_start);
  while (i < line.size() && line[i] != '"') ++i;
  if (i >= line.size()) return false;
  size_t val_start = ++i;
  while (i < line.size() && line[i] != '"') ++i;
  if (i >= line.size()) return false;
  *value = line.substr(val_start, i - val_start);
  return true;
}

bool IsResultToken(std::string_view t) {
  return t == "1-0" || t == "0-1" || t == "1/2-1/2" || t == "*";
}

int ParseElo(std::string_view s) {
  if (s.empty() || s == "?") return 0;
  int v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return 0;
    v = v * 10 + (c - '0');
  }
  return v;
}

// Tokenize a move-text line, after brace/semicolon/variation stripping.
// Calls `on_token(sv)` for each SAN/result token.
template <typename F>
void TokenizeMoveText(std::string_view line, F&& on_token) {
  size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
    if (i >= line.size()) break;
    size_t start = i;
    while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) ++i;
    std::string_view tok(line.data() + start, i - start);

    // Strip leading move number like "1." or "1..." or "23...".
    size_t dot = tok.find('.');
    if (dot != std::string_view::npos) {
      bool all_digits = dot > 0;
      for (size_t k = 0; k < dot; ++k) {
        if (tok[k] < '0' || tok[k] > '9') { all_digits = false; break; }
      }
      if (all_digits) {
        size_t k = dot;
        while (k < tok.size() && tok[k] == '.') ++k;
        tok.remove_prefix(k);
      }
    }
    if (tok.empty()) continue;
    if (tok[0] == '$') continue;  // NAG
    on_token(tok);
  }
}

}  // namespace

uint64_t StreamPgnFile(const std::string& path,
                       uint32_t game_id_base,
                       const PlyCallback& on_ply,
                       const GameCallback& on_game) {
  LineReader reader(path);
  lczero::PositionHistory history;
  GameHeader header;
  std::string scratch;  // accumulates move text across lines
  scratch.reserve(4096);
  bool in_game = false;
  bool in_comment = false;
  int variation_depth = 0;
  uint32_t games_emitted = 0;
  uint32_t game_id = game_id_base;
  uint16_t ply = 0;
  bool game_broken = false;

  auto reset_game = [&]() {
    header = GameHeader{};
    scratch.clear();
    in_game = false;
    in_comment = false;
    variation_depth = 0;
    ply = 0;
    game_broken = false;
  };

  auto init_history = [&]() {
    lczero::ChessBoard board;
    int rule50 = 0, fullmove = 1;
    const std::string& fen = header.start_fen.empty()
        ? std::string(lczero::ChessBoard::kStartposFen)
        : header.start_fen;
    try {
      board.SetFromFen(fen, &rule50, &fullmove);
    } catch (const lczero::Exception& e) {
      std::cerr << "[pgn2pack] bad FEN in game " << game_id << ": " << e.what()
                << "\n";
      game_broken = true;
      return;
    }
    // lc0's SetFromFen writes the raw fullmove counter back into its second
    // out-param, but PositionHistory::Reset wants the half-move count since
    // game start. Convert: ply = (fullmove - 1) * 2 + (black_to_move ? 1 : 0).
    int game_ply = (fullmove - 1) * 2 + (board.flipped() ? 1 : 0);
    history.Reset(board, rule50, game_ply);
  };

  auto flush_scratch_tokens = [&]() {
    if (game_broken) { scratch.clear(); return; }
    TokenizeMoveText(scratch, [&](std::string_view tok) {
      if (game_broken) return;
      if (IsResultToken(tok)) return;  // handled by outer
      try {
        lczero::Move m = SanToMove(tok, history.Last().GetBoard());
        history.Append(m);
        on_ply(header, history, game_id, ply);
        ++ply;
      } catch (const lczero::Exception& e) {
        std::cerr << "[pgn2pack] skipping game " << game_id << " at ply "
                  << ply << ": " << e.what() << " (tok=\"" << tok << "\")\n";
        game_broken = true;
      }
    });
    scratch.clear();
  };

  auto finish_game = [&]() {
    if (!in_game) return;
    flush_scratch_tokens();
    if (!game_broken) {
      on_game(header, game_id, ply);
      ++games_emitted;
    }
    ++game_id;
    reset_game();
  };

  std::string_view raw;
  bool expecting_moves = false;
  while (reader.NextLine(&raw)) {
    std::string_view line = raw;
    if (line.size() >= 3 &&
        static_cast<unsigned char>(line[0]) == 0xEF &&
        static_cast<unsigned char>(line[1]) == 0xBB &&
        static_cast<unsigned char>(line[2]) == 0xBF) {
      line.remove_prefix(3);
    }

    if (!line.empty() && line.front() == '[') {
      // Tag line — begins (or continues) a header block.
      if (expecting_moves) {
        // New header started without a result terminator for the previous game.
        finish_game();
        expecting_moves = false;
      }
      in_game = true;
      std::string_view tag, value;
      if (ParseTag(line, &tag, &value)) {
        if (tag == "FEN") header.start_fen.assign(value);
        else if (tag == "Result") header.result.assign(value);
        else if (tag == "WhiteElo") header.white_elo = ParseElo(value);
        else if (tag == "BlackElo") header.black_elo = ParseElo(value);
        else if (tag == "UTCDate") header.utc_date.assign(value);
        else if (tag == "TimeControl") header.time_control.assign(value);
        else if (tag == "Event") header.event.assign(value);
      }
      continue;
    }

    // Blank line: if we saw tags and haven't started move text, this
    // separates header from moves. If we were in moves, ignore.
    if (line.empty()) {
      if (in_game && !expecting_moves) {
        // Moving from header to move text.
        expecting_moves = true;
        init_history();
      }
      continue;
    }

    if (!in_game) continue;  // stray text before any game header
    if (!expecting_moves) {
      // First non-tag, non-blank line after tags without a blank separator.
      expecting_moves = true;
      init_history();
    }

    // Strip comments and variations into `filtered`.
    std::string filtered;
    filtered.reserve(line.size());
    bool saw_result = false;
    for (size_t i = 0; i < line.size(); ++i) {
      char c = line[i];
      if (in_comment) {
        if (c == '}') in_comment = false;
        continue;
      }
      if (c == '{') { in_comment = true; continue; }
      if (c == ';') break;  // rest-of-line comment
      if (c == '(') { ++variation_depth; continue; }
      if (c == ')') { if (variation_depth > 0) --variation_depth; continue; }
      if (variation_depth > 0) continue;
      filtered.push_back(c);
    }

    // Append to scratch and check for a result terminator within this line.
    scratch.append(filtered);
    scratch.push_back(' ');

    // Quick scan: if a result token appears, finish this game.
    // (Tokenization will ignore the result token itself.)
    auto contains_result = [](const std::string& s) {
      for (const char* t : {"1-0", "0-1", "1/2-1/2", " * "}) {
        if (s.find(t) != std::string::npos) return true;
      }
      return false;
    };
    if (contains_result(scratch)) saw_result = true;

    if (saw_result) {
      finish_game();
      expecting_moves = false;
    }
  }

  // Flush trailing game.
  if (in_game) finish_game();

  return games_emitted;
}

}  // namespace pgn2pack
