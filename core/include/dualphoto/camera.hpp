#pragma once

#include "frame.hpp"

namespace dualphoto {

class Camera {
  public:
    virtual ~Camera() = default;

    [[nodiscard]] virtual Frame capture() = 0;
};

} // namespace dualphoto
