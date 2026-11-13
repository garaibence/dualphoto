#pragma once

#include "frame.hpp"

#include <cstdint>

namespace dualphoto {

class Display {
  public:
    virtual ~Display() = default;

    [[nodiscard]] virtual std::uint32_t width() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t height() const noexcept = 0;

    virtual void present(const Frame& frame) = 0;
};

} // namespace dualphoto
