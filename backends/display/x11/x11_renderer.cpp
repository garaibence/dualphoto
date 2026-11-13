#include "x11_renderer.hpp"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>

namespace dualphoto {
namespace {

unsigned long channel_to_mask(std::uint8_t channel, unsigned long mask) {
    unsigned int bit_count = 0;
    for (unsigned int bit = 0; bit < sizeof(mask) * CHAR_BIT; ++bit) {
        if ((mask & (1UL << bit)) != 0) {
            ++bit_count;
        }
    }

    if (bit_count == 0) {
        return 0;
    }

    const auto maximum = bit_count == sizeof(unsigned long) * CHAR_BIT
                             ? std::numeric_limits<unsigned long>::max()
                             : (1UL << bit_count) - 1UL;
    auto value = static_cast<unsigned long>(
        (static_cast<unsigned long long>(channel) * maximum + 127ULL) / 255ULL);

    unsigned long result = 0;
    for (unsigned int bit = 0; bit < sizeof(mask) * CHAR_BIT; ++bit) {
        const auto bit_mask = 1UL << bit;
        if ((mask & bit_mask) != 0) {
            if ((value & 1UL) != 0) {
                result |= bit_mask;
            }
            value >>= 1U;
        }
    }
    return result;
}

} // namespace

struct X11Renderer::Impl {
    ::Display* display = nullptr;
    int screen = 0;
    Window window = 0;
    GC gc = nullptr;
    XImage* image = nullptr;
    int x = 0;
    int y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    explicit Impl(const char* display_name, const char* output_name) {
        display = XOpenDisplay(display_name);
        if (display == nullptr) {
            throw std::runtime_error("Unable to open the X11 display");
        }

        screen = DefaultScreen(display);
        const auto screen_width = DisplayWidth(display, screen);
        const auto screen_height = DisplayHeight(display, screen);
        if (screen_width <= 0 || screen_height <= 0) {
            XCloseDisplay(display);
            display = nullptr;
            throw std::runtime_error("The X11 display has invalid dimensions");
        }
        width = static_cast<std::uint32_t>(screen_width);
        height = static_cast<std::uint32_t>(screen_height);

        if (output_name != nullptr) {
            auto* resources = XRRGetScreenResourcesCurrent(display, RootWindow(display, screen));
            if (resources == nullptr) {
                XCloseDisplay(display);
                display = nullptr;
                throw std::runtime_error("Unable to query XRandR outputs");
            }

            bool output_found = false;
            bool output_active = false;
            for (int i = 0; i < resources->noutput; ++i) {
                auto* output = XRRGetOutputInfo(display, resources, resources->outputs[i]);
                if (output == nullptr) {
                    continue;
                }

                const bool name_matches =
                    output->nameLen ==
                        static_cast<int>(std::char_traits<char>::length(output_name)) &&
                    std::equal(output_name, output_name + output->nameLen, output->name);
                if (name_matches) {
                    output_found = true;
                    if (output->connection == RR_Connected && output->crtc != None) {
                        auto* crtc = XRRGetCrtcInfo(display, resources, output->crtc);
                        if (crtc != nullptr) {
                            x = crtc->x;
                            y = crtc->y;
                            width = crtc->width;
                            height = crtc->height;
                            output_active = width > 0 && height > 0;
                            XRRFreeCrtcInfo(crtc);
                        }
                    }
                    XRRFreeOutputInfo(output);
                    break;
                }
                XRRFreeOutputInfo(output);
            }
            XRRFreeScreenResources(resources);

            if (!output_found) {
                XCloseDisplay(display);
                display = nullptr;
                throw std::runtime_error(std::string("X11 output not found: ") + output_name);
            }
            if (!output_active) {
                XCloseDisplay(display);
                display = nullptr;
                throw std::runtime_error(std::string("X11 output is not active: ") + output_name);
            }
        }

        auto* visual = DefaultVisual(display, screen);
        const auto depth = DefaultDepth(display, screen);
        const auto root = RootWindow(display, screen);

        XSetWindowAttributes attributes{};
        attributes.override_redirect = True;
        attributes.background_pixel = BlackPixel(display, screen);
        attributes.event_mask = ExposureMask;

        window = XCreateWindow(display, root, x, y, width, height, 0, depth, InputOutput, visual,
                               CWOverrideRedirect | CWBackPixel | CWEventMask, &attributes);
        if (window == 0) {
            XCloseDisplay(display);
            display = nullptr;
            throw std::runtime_error("Unable to create the X11 fullscreen window");
        }

        gc = XCreateGC(display, window, 0, nullptr);
        if (gc == nullptr) {
            XDestroyWindow(display, window);
            XCloseDisplay(display);
            window = 0;
            display = nullptr;
            throw std::runtime_error("Unable to create the X11 graphics context");
        }

        image = XCreateImage(display, visual, static_cast<unsigned int>(depth), ZPixmap, 0, nullptr,
                             width, height, BitmapPad(display), 0);
        if (image == nullptr) {
            XFreeGC(display, gc);
            XDestroyWindow(display, window);
            XCloseDisplay(display);
            gc = nullptr;
            window = 0;
            display = nullptr;
            throw std::runtime_error("Unable to create the X11 frame buffer");
        }

        const auto buffer_size = static_cast<std::size_t>(image->bytes_per_line) * height;
        image->data = static_cast<char*>(std::calloc(buffer_size, 1));
        if (image->data == nullptr) {
            XDestroyImage(image);
            XFreeGC(display, gc);
            XDestroyWindow(display, window);
            XCloseDisplay(display);
            image = nullptr;
            gc = nullptr;
            window = 0;
            display = nullptr;
            throw std::bad_alloc();
        }

        XMapRaised(display, window);
        XSync(display, False);
    }

    ~Impl() {
        if (display != nullptr) {
            if (image != nullptr) {
                XDestroyImage(image);
            }
            if (gc != nullptr) {
                XFreeGC(display, gc);
            }
            if (window != 0) {
                XDestroyWindow(display, window);
            }
            XCloseDisplay(display);
        }
    }
};

X11Renderer::X11Renderer(const char* display_name, const char* output_name)
    : impl_(std::make_unique<Impl>(display_name, output_name)) {}

X11Renderer::~X11Renderer() = default;

X11Renderer::X11Renderer(X11Renderer&&) noexcept = default;
X11Renderer& X11Renderer::operator=(X11Renderer&&) noexcept = default;

std::uint32_t X11Renderer::width() const noexcept {
    return impl_ == nullptr ? 0 : impl_->width;
}

std::uint32_t X11Renderer::height() const noexcept {
    return impl_ == nullptr ? 0 : impl_->height;
}

void X11Renderer::present(const Frame& frame) {
    present(frame.width, frame.height, frame.rgb);
}

void X11Renderer::present(std::uint32_t frame_width, std::uint32_t frame_height,
                          std::span<const std::uint8_t> rgb) {
    if (impl_ == nullptr) {
        throw std::logic_error("Cannot present a frame with a moved-from X11 renderer");
    }
    if (frame_width == 0 || frame_height == 0) {
        throw std::invalid_argument("Frame dimensions must be non-zero");
    }
    if (frame_width > std::numeric_limits<std::size_t>::max() / 3U / frame_height) {
        throw std::length_error("Frame dimensions overflow the RGB8 buffer size");
    }
    const auto expected_size = static_cast<std::size_t>(frame_width) * frame_height * 3U;
    if (rgb.size() != expected_size) {
        throw std::invalid_argument("RGB8 buffer size does not match the frame dimensions");
    }

    auto* visual = DefaultVisual(impl_->display, impl_->screen);
    std::array<unsigned long, 256> red_values{};
    std::array<unsigned long, 256> green_values{};
    std::array<unsigned long, 256> blue_values{};
    for (std::size_t value = 0; value < red_values.size(); ++value) {
        red_values[value] = channel_to_mask(static_cast<std::uint8_t>(value), visual->red_mask);
        green_values[value] = channel_to_mask(static_cast<std::uint8_t>(value), visual->green_mask);
        blue_values[value] = channel_to_mask(static_cast<std::uint8_t>(value), visual->blue_mask);
    }

    for (std::uint32_t y = 0; y < impl_->height; ++y) {
        const auto source_y = static_cast<std::uint32_t>(static_cast<std::uint64_t>(y) *
                                                         frame_height / impl_->height);
        for (std::uint32_t x = 0; x < impl_->width; ++x) {
            const auto source_x = static_cast<std::uint32_t>(static_cast<std::uint64_t>(x) *
                                                             frame_width / impl_->width);
            const auto offset = (static_cast<std::size_t>(source_y) * frame_width + source_x) * 3U;
            const auto pixel = red_values[rgb[offset]] | green_values[rgb[offset + 1U]] |
                               blue_values[rgb[offset + 2U]];
            XPutPixel(impl_->image, static_cast<int>(x), static_cast<int>(y), pixel);
        }
    }

    XPutImage(impl_->display, impl_->window, impl_->gc, impl_->image, 0, 0, 0, 0, impl_->width,
              impl_->height);
    XFlush(impl_->display);
}

} // namespace dualphoto
