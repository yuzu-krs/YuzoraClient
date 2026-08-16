#include "version/GameVersion.hpp"

#include <format>

namespace yuzora::version {

std::string GameVersion::toString() const {
    return std::format("{}.{}.{}.{}", major_, minor_, patch_, build_);
}

}  // namespace yuzora::version
