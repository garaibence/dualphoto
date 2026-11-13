#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#if defined(DUALPHOTO_HAVE_CAMERA_V4L2)
#include "v4l2_camera.hpp"
#endif

namespace {

struct Options {
    std::string_view device = "/dev/video0";
    std::uint32_t width = 640;
    std::uint32_t height = 480;
    std::uint32_t frames = 10;
};

enum class ParseResult { success, help, error };

bool parse_positive_integer(std::string_view text, std::uint32_t& result) {
    if (text.empty()) {
        return false;
    }

    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto [parsed_end, error] = std::from_chars(begin, end, result);
    return error == std::errc{} && parsed_end == end && result != 0;
}

void print_usage(const char* executable) {
    std::cout << "Usage: " << executable
              << " [--device PATH] [--width PIXELS] [--height PIXELS] [--frames COUNT]\n"
              << "Capture frames from a V4L2 camera and report RGB statistics.\n";
}

ParseResult parse_options(int argc, char* argv[], Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument == "--help" || argument == "-h") {
            print_usage(argv[0]);
            return ParseResult::help;
        }

        if (i + 1 >= argc) {
            std::cerr << "Missing value for " << argument << '\n';
            print_usage(argv[0]);
            return ParseResult::error;
        }
        const std::string_view value(argv[++i]);

        if (argument == "--device") {
            if (value.empty()) {
                std::cerr << "Device path must not be empty\n";
                return ParseResult::error;
            }
            options.device = value;
        } else if (argument == "--width") {
            if (!parse_positive_integer(value, options.width)) {
                std::cerr << "Width must be a positive integer\n";
                return ParseResult::error;
            }
        } else if (argument == "--height") {
            if (!parse_positive_integer(value, options.height)) {
                std::cerr << "Height must be a positive integer\n";
                return ParseResult::error;
            }
        } else if (argument == "--frames") {
            if (!parse_positive_integer(value, options.frames)) {
                std::cerr << "Frame count must be a positive integer\n";
                return ParseResult::error;
            }
        } else {
            std::cerr << "Unknown argument: " << argument << '\n';
            print_usage(argv[0]);
            return ParseResult::error;
        }
    }
    return ParseResult::success;
}

} // namespace

int main(int argc, char* argv[]) {
    Options options;
    const auto parse_result = parse_options(argc, argv, options);
    if (parse_result == ParseResult::help) {
        return 0;
    }
    if (parse_result == ParseResult::error) {
        return 2;
    }

#if defined(DUALPHOTO_HAVE_CAMERA_V4L2)
    try {
        dualphoto::V4L2Camera camera(std::string(options.device), options.width, options.height);
        std::cout << "Capturing " << options.frames << " frame(s) from " << options.device
                  << " (requested " << options.width << 'x' << options.height << ")\n";
        const auto start = std::chrono::steady_clock::now();

        for (std::uint32_t index = 0; index < options.frames; ++index) {
            const auto frame = camera.capture();
            const auto expected_size = static_cast<std::uint64_t>(frame.width) * frame.height * 3U;
            if (frame.width == 0 || frame.height == 0 || expected_size != frame.rgb.size()) {
                throw std::runtime_error("Camera returned an invalid RGB8 frame");
            }

            std::uint64_t red_total = 0;
            std::uint64_t green_total = 0;
            std::uint64_t blue_total = 0;
            for (std::size_t offset = 0; offset < frame.rgb.size(); offset += 3U) {
                red_total += frame.rgb[offset];
                green_total += frame.rgb[offset + 1U];
                blue_total += frame.rgb[offset + 2U];
            }

            const auto pixel_count = static_cast<double>(frame.width) * frame.height;
            std::cout << "Frame " << (index + 1U) << ": " << frame.width << 'x' << frame.height
                      << " RGB8, mean RGB=(" << std::fixed << std::setprecision(1)
                      << red_total / pixel_count << ", " << green_total / pixel_count << ", "
                      << blue_total / pixel_count << ")\n";
        }

        const auto elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start);
        std::cout << "Captured " << options.frames << " frame(s) in " << std::fixed
                  << std::setprecision(2) << elapsed.count() << " s";
        if (elapsed.count() > 0.0) {
            std::cout << " (" << options.frames / elapsed.count() << " fps)";
        }
        std::cout << '\n';
    } catch (const std::exception& error) {
        std::cerr << "Camera test failed: " << error.what() << '\n';
        return 1;
    }
#else
    std::cerr << "No implemented camera backend is available in this build. "
                 "Enable DUALPHOTO_WITH_V4L2 to test a V4L2 camera.\n";
    return 1;
#endif

    return 0;
}
