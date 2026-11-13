#pragma once

#include "dualphoto/display.hpp"

#include <cstdint>
#include <memory>
#include <span>

namespace dualphoto {

class X11Renderer final : public Display {
  public:
    explicit X11Renderer(const char* display_name = nullptr, const char* output_name = nullptr);
    ~X11Renderer() override;

    X11Renderer(const X11Renderer&) = delete;
    X11Renderer& operator=(const X11Renderer&) = delete;

    X11Renderer(X11Renderer&&) noexcept;
    X11Renderer& operator=(X11Renderer&&) noexcept;

    [[nodiscard]] std::uint32_t width() const noexcept override;
    [[nodiscard]] std::uint32_t height() const noexcept override;

    void present(const Frame& frame) override;

    // Presents tightly packed RGB8 pixels, scaling them to the selected output.
    void present(std::uint32_t frame_width, std::uint32_t frame_height,
                 std::span<const std::uint8_t> rgb);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dualphoto
