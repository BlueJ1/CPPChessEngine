#define CATCH_CONFIG_RUNNER
#include <catch2/catch_session.hpp>

#include "chess/board.h"

int main(int argc, char** argv) {
  lczero::InitializeMagicBitboards();
  return Catch::Session().run(argc, argv);
}
