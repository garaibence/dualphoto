#pragma once

#include <cstdint>
#include <memory>
#include <span>

namespace dualphoto {

class X11Renderer {
  public:
    explicit X11Renderer(const char* display_name = nullptr, const char* output_name = nullptr);
    ~X11Renderer();

    X11Renderer(const X11Renderer&) = delete;
    X11Renderer& operator=(const X11Renderer&) = delete;

    X11Renderer(X11Renderer&&) noexcept;
    X11Renderer& operator=(X11Renderer&&) noexcept;

    [[nodiscard]] std::uint32_t width() const noexcept;
    [[nodiscard]] std::uint32_t height() const noexcept;

    // Presents tightly packed RGB8 pixels, scaling them to the selected output.
    void present(std::uint32_t frame_width, std::uint32_t frame_height,
                 std::span<const std::uint8_t> rgb);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dualphoto
