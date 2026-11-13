#pragma once

#include "dualphoto/camera.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace dualphoto {

class V4L2Camera final : public Camera {
  public:
    explicit V4L2Camera(std::string device = "/dev/video0", std::uint32_t width = 640,
                        std::uint32_t height = 480);
    ~V4L2Camera() override;

    V4L2Camera(const V4L2Camera&) = delete;
    V4L2Camera& operator=(const V4L2Camera&) = delete;

    V4L2Camera(V4L2Camera&&) noexcept;
    V4L2Camera& operator=(V4L2Camera&&) noexcept;

    [[nodiscard]] Frame capture() override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dualphoto
