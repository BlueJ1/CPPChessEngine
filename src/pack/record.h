// 40-byte packed position record. See plan for format.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

#include "chess/position.h"

namespace pgn2pack {

#pragma pack(push, 1)
struct PackedRecord {
  uint64_t occupancy;        // bits: white|black occupied squares, a1=bit0
  uint8_t  piece_nibbles[16]; // LSB-first, 4 bits per occupied square
  uint8_t  stm_castle_ep;    // bit0: stm (0=w,1=b); bits1-4: castling KQkq;
                             // bits5-7: ep file 0-7 (a-h), only valid when
                             // flags bit 1 is set
  // bit 0 of `flags` marks a non-standard starting position.
  // bit 1 of `flags` marks "en passant square is set" (file in stm_castle_ep).
  uint8_t  rule50;           // halfmove clock
  uint16_t fullmove;
  uint32_t game_id;
  uint16_t ply_in_game;
  uint8_t  result;           // 0=*, 1=1-0, 2=0-1, 3=½-½
  uint8_t  flags;            // bit0: non-standard startpos
  uint32_t reserved;
};
#pragma pack(pop)
static_assert(sizeof(PackedRecord) == 40, "PackedRecord must be 40 bytes");

uint8_t EncodeResult(const std::string& result_tag);

// Encodes the current position into `out`. `flags` / `result` / `game_id` /
// `ply_in_game` are caller-provided.
void EncodePosition(const lczero::Position& pos, uint32_t game_id,
                    uint16_t ply_in_game, uint8_t result, uint8_t flags,
                    PackedRecord* out);

// Decodes a packed record back to a textual FEN, for verification. The
// reconstruction is faithful for standard chess positions (it does not
// recover Chess960 rook files).
std::string DecodeFen(const PackedRecord& rec);

}  // namespace pgn2pack
