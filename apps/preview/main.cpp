#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>

#if defined(DUALPHOTO_HAVE_DISPLAY_X11)
#include "dualphoto/frame.hpp"
#include "x11_renderer.hpp"
#endif

namespace {

constexpr std::uint32_t frame_width = 640;
constexpr std::uint32_t frame_height = 480;
constexpr std::size_t channel_count = 3;
constexpr std::size_t pixel_count = static_cast<std::size_t>(frame_width) * frame_height;
volatile std::sig_atomic_t keep_running = 1;

void handle_interrupt(int) {
    keep_running = 0;
}

using Frame = std::vector<std::uint8_t>;

Frame make_color_bars() {
    constexpr std::uint8_t bars[][channel_count] = {
        {255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0},
        {255, 0, 255},   {255, 0, 0},   {0, 0, 255},   {0, 0, 0},
    };
    constexpr std::size_t bar_count = sizeof(bars) / sizeof(bars[0]);

    Frame frame(pixel_count * channel_count);
    for (std::uint32_t y = 0; y < frame_height; ++y) {
        for (std::uint32_t x = 0; x < frame_width; ++x) {
            const auto bar = static_cast<std::size_t>(x) * bar_count / frame_width;
            const auto offset = (static_cast<std::size_t>(y) * frame_width + x) * channel_count;
            for (std::size_t channel = 0; channel < channel_count; ++channel) {
                frame[offset + channel] = bars[bar][channel];
            }
        }
    }
    return frame;
}

Frame make_grayscale_ramp() {
    Frame frame(pixel_count * channel_count);
    for (std::uint32_t y = 0; y < frame_height; ++y) {
        for (std::uint32_t x = 0; x < frame_width; ++x) {
            const auto value = static_cast<std::uint8_t>(static_cast<std::uint64_t>(x) * 255U /
                                                         (frame_width - 1U));
            const auto offset = (static_cast<std::size_t>(y) * frame_width + x) * channel_count;
            frame[offset] = value;
            frame[offset + 1U] = value;
            frame[offset + 2U] = value;
        }
    }
    return frame;
}

Frame make_checkerboard() {
    constexpr std::uint32_t square_size = 32;
    Frame frame(pixel_count * channel_count);
    for (std::uint32_t y = 0; y < frame_height; ++y) {
        for (std::uint32_t x = 0; x < frame_width; ++x) {
            const auto value = ((x / square_size + y / square_size) % 2U) == 0 ? 255U : 0U;
            const auto offset = (static_cast<std::size_t>(y) * frame_width + x) * channel_count;
            frame[offset] = static_cast<std::uint8_t>(value);
            frame[offset + 1U] = static_cast<std::uint8_t>(value);
            frame[offset + 2U] = static_cast<std::uint8_t>(value);
        }
    }
    return frame;
}

Frame make_rgb_gradients() {
    Frame frame(pixel_count * channel_count);
    for (std::uint32_t y = 0; y < frame_height; ++y) {
        const auto green =
            static_cast<std::uint8_t>(static_cast<std::uint64_t>(y) * 255U / (frame_height - 1U));
        for (std::uint32_t x = 0; x < frame_width; ++x) {
            const auto red = static_cast<std::uint8_t>(static_cast<std::uint64_t>(x) * 255U /
                                                       (frame_width - 1U));
            const auto offset = (static_cast<std::size_t>(y) * frame_width + x) * channel_count;
            frame[offset] = red;
            frame[offset + 1U] = green;
            frame[offset + 2U] = static_cast<std::uint8_t>(255U - red);
        }
    }
    return frame;
}

struct TestPattern {
    std::string_view name;
    Frame (*create)();
};

constexpr TestPattern patterns[] = {
    {"color bars", make_color_bars},
    {"grayscale ramp", make_grayscale_ramp},
    {"checkerboard", make_checkerboard},
    {"RGB gradients", make_rgb_gradients},
};

void print_usage(const char* executable) {
    std::cout << "Usage: " << executable << " [--display DISPLAY] [--output OUTPUT]\n"
              << "Cycle renderer test patterns until interrupted (Ctrl+C).\n";
}

} // namespace

int main(int argc, char* argv[]) {
    const char* display_name = nullptr;
    const char* output_name = nullptr;

    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument == "--help" || argument == "-h") {
            print_usage(argv[0]);
            return 0;
        }
        if (argument == "--display" || argument == "--output") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << argument << '\n';
                print_usage(argv[0]);
                return 2;
            }
            const char* value = argv[++i];
            if (argument == "--display") {
                display_name = value;
            } else {
                output_name = value;
            }
            continue;
        }

        std::cerr << "Unknown argument: " << argument << '\n';
        print_usage(argv[0]);
        return 2;
    }

#if defined(DUALPHOTO_HAVE_DISPLAY_X11)
    try {
        dualphoto::X11Renderer renderer(display_name, output_name);
        std::cout << "Testing X11 renderer at " << renderer.width() << 'x' << renderer.height()
                  << ". Press Ctrl+C to exit.\n";
        std::signal(SIGINT, handle_interrupt);
        std::signal(SIGTERM, handle_interrupt);

        std::size_t pattern_index = 0;
        while (keep_running != 0) {
            const auto& pattern = patterns[pattern_index];
            const dualphoto::Frame frame{frame_width, frame_height, pattern.create()};
            renderer.present(frame);
            std::cout << "Showing " << pattern.name << '\n';
            pattern_index = (pattern_index + 1U) % (sizeof(patterns) / sizeof(patterns[0]));

            for (int tick = 0; tick < 30 && keep_running != 0; ++tick) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "Renderer test failed: " << error.what() << '\n';
        return 1;
    }
#else
    (void)display_name;
    (void)output_name;
    std::cerr << "No implemented display renderer is available in this build.\n";
    return 1;
#endif

    return 0;
}
