#pragma once

#include "dualphoto/camera.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace dualphoto {

struct CameraControlStatus {
    std::string name;
    bool configured = false;
    std::string detail;
};

struct CameraControlRange {
    std::int32_t minimum = 0;
    std::int32_t maximum = 0;
    std::int32_t step = 1;
};

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
    [[nodiscard]] std::vector<CameraControlStatus> enable_automatic_controls();
    [[nodiscard]] std::vector<CameraControlStatus> lock_automatic_controls();
    [[nodiscard]] std::optional<CameraControlRange> exposure_absolute_range() const;
    [[nodiscard]] bool set_exposure_absolute(std::int32_t value);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dualphoto
