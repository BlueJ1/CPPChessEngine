// Derived from lc0 (GPLv3). Stripped of logging dependency.
#pragma once

#include <stdexcept>
#include <string>

namespace lczero {

class Exception : public std::runtime_error {
 public:
  Exception(const std::string& what) : std::runtime_error(what) {}
};

}  // namespace lczero
