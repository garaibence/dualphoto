#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#if defined(DUALPHOTO_HAVE_DISPLAY_X11) && defined(DUALPHOTO_HAVE_CAMERA_V4L2)
#include "v4l2_camera.hpp"
#include "x11_renderer.hpp"
#endif

namespace {

constexpr std::uint32_t pattern_width = 640;
constexpr std::uint32_t pattern_height = 480;
constexpr std::uint32_t required_stable_samples = 3;
constexpr std::size_t auto_stability_window = 6;
constexpr std::size_t exposure_sample_count = 6;
constexpr std::size_t exposure_warmup_frames = 6;
constexpr std::size_t exposure_stability_attempts = 3;
constexpr std::size_t exposure_verification_attempts = 3;
constexpr std::size_t exposure_fallback_steps = 16;
constexpr double minimum_usable_contrast = 20.0;
constexpr double stable_luminance_absolute_tolerance = 2.0;
constexpr double stable_luminance_relative_tolerance = 0.03;
constexpr auto baseline_settle = std::chrono::seconds(2);
constexpr auto auto_minimum_settle = std::chrono::seconds(2);
constexpr auto auto_calibration_timeout = std::chrono::seconds(10);
constexpr auto transition_timeout = std::chrono::seconds(10);

struct Options {
    std::string device = "/dev/video0";
    std::string display;
    std::string output;
    std::uint32_t width = 640;
    std::uint32_t height = 360;
    std::uint32_t transitions = 20;
};

struct Measurement {
    std::chrono::steady_clock::duration latency{};
    std::chrono::steady_clock::duration frame_interval{};
};

struct LuminanceStats {
    double mean = 0.0;
    double minimum = 0.0;
    double maximum = 0.0;
    std::chrono::steady_clock::duration maximum_frame_interval{};

    [[nodiscard]] bool stable() const {
        return maximum - minimum <= std::max(stable_luminance_absolute_tolerance,
                                             mean * stable_luminance_relative_tolerance);
    }
};

struct ExposureEvaluation {
    std::optional<std::int32_t> value;
    LuminanceStats white;
    LuminanceStats black;

    [[nodiscard]] double contrast() const { return white.mean - black.mean; }

    [[nodiscard]] bool usable() const {
        return white.stable() && black.stable() && contrast() >= minimum_usable_contrast;
    }
};

enum class ParseResult { success, help, error };

bool parse_positive_integer(std::string_view text, std::uint32_t& result) {
    if (text.empty()) {
        return false;
    }

    std::uint32_t parsed = 0;
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto [parsed_end, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc{} || parsed_end != end || parsed == 0) {
        return false;
    }
    result = parsed;
    return true;
}

void print_usage(const char* executable) {
    std::cout << "Usage: " << executable
              << " [--device PATH] [--width PIXELS] [--height PIXELS] "
                 "[--transitions COUNT] [--display DISPLAY] [--output OUTPUT]\n"
              << "Measure end-to-end projector-to-camera brightness settling time.\n";
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

        const std::string value(argv[++i]);
        if (value.empty()) {
            std::cerr << "Value for " << argument << " must not be empty\n";
            return ParseResult::error;
        }
        if (argument == "--device") {
            options.device = value;
        } else if (argument == "--display") {
            options.display = value;
        } else if (argument == "--output") {
            options.output = value;
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
        } else if (argument == "--transitions") {
            if (!parse_positive_integer(value, options.transitions)) {
                std::cerr << "Transition count must be a positive integer\n";
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

#if defined(DUALPHOTO_HAVE_DISPLAY_X11) && defined(DUALPHOTO_HAVE_CAMERA_V4L2)
using Clock = std::chrono::steady_clock;
using Rgb = std::array<std::uint8_t, 3>;

dualphoto::Frame make_solid_frame(const Rgb& color) {
    dualphoto::Frame frame;
    frame.width = pattern_width;
    frame.height = pattern_height;
    frame.rgb.resize(static_cast<std::size_t>(pattern_width) * pattern_height * 3U);
    for (std::size_t offset = 0; offset < frame.rgb.size(); offset += 3U) {
        frame.rgb[offset] = color[0];
        frame.rgb[offset + 1U] = color[1];
        frame.rgb[offset + 2U] = color[2];
    }
    return frame;
}

double center_luminance(const dualphoto::Frame& frame) {
    if (frame.width == 0 || frame.height == 0 ||
        frame.width > std::numeric_limits<std::size_t>::max() / 3U / frame.height ||
        static_cast<std::size_t>(frame.width) * frame.height * 3U != frame.rgb.size()) {
        throw std::runtime_error("Camera returned an invalid RGB8 frame");
    }

    const auto left = frame.width / 4U;
    const auto right = frame.width - left;
    const auto top = frame.height / 4U;
    const auto bottom = frame.height - top;
    std::uint64_t luminance_sum = 0;
    std::uint64_t pixel_count = 0;
    for (std::uint32_t y = top; y < bottom; ++y) {
        for (std::uint32_t x = left; x < right; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * frame.width + x) * 3U;
            const auto luminance = (77U * frame.rgb[offset] + 150U * frame.rgb[offset + 1U] +
                                    29U * frame.rgb[offset + 2U]) >>
                                   8U;
            luminance_sum += luminance;
            ++pixel_count;
        }
    }
    return static_cast<double>(luminance_sum) / static_cast<double>(pixel_count);
}

double auto_calibrate_camera(dualphoto::V4L2Camera& camera) {
    const auto started_at = Clock::now();
    std::vector<double> luminances;
    luminances.reserve(auto_stability_window);

    while (Clock::now() - started_at < auto_calibration_timeout) {
        luminances.push_back(center_luminance(camera.capture()));
        if (luminances.size() > auto_stability_window) {
            luminances.erase(luminances.begin());
        }

        if (Clock::now() - started_at < auto_minimum_settle ||
            luminances.size() < auto_stability_window) {
            continue;
        }

        const auto [minimum, maximum] = std::minmax_element(luminances.begin(), luminances.end());
        const auto average =
            std::accumulate(luminances.begin(), luminances.end(), 0.0) / luminances.size();
        if (*maximum - *minimum <= std::max(2.0, average * 0.03)) {
            return average;
        }
    }

    throw std::runtime_error(
        "Camera auto-exposure did not stabilize under the white pattern within 10 seconds");
}

LuminanceStats measure_luminance(dualphoto::X11Renderer& renderer, dualphoto::V4L2Camera& camera,
                                 const dualphoto::Frame& pattern) {
    renderer.present(pattern);
    for (std::size_t sample = 0; sample < exposure_warmup_frames; ++sample) {
        (void)camera.capture();
    }

    LuminanceStats latest;
    for (std::size_t attempt = 0; attempt < exposure_stability_attempts; ++attempt) {
        std::vector<double> luminances;
        luminances.reserve(exposure_sample_count);
        auto previous_capture_at = Clock::now();
        auto maximum_frame_interval = Clock::duration::zero();
        for (std::size_t sample = 0; sample < exposure_sample_count; ++sample) {
            const auto frame = camera.capture();
            const auto captured_at = Clock::now();
            if (sample != 0) {
                maximum_frame_interval =
                    std::max(maximum_frame_interval, captured_at - previous_capture_at);
            }
            previous_capture_at = captured_at;
            luminances.push_back(center_luminance(frame));
        }

        const auto [minimum, maximum] = std::minmax_element(luminances.begin(), luminances.end());
        const auto mean =
            std::accumulate(luminances.begin(), luminances.end(), 0.0) / luminances.size();
        latest = {mean, *minimum, *maximum, maximum_frame_interval};
        if (latest.stable()) {
            return latest;
        }
    }
    return latest;
}

ExposureEvaluation evaluate_exposure(dualphoto::X11Renderer& renderer,
                                     dualphoto::V4L2Camera& camera, const dualphoto::Frame& white,
                                     const dualphoto::Frame& black, std::int32_t value) {
    if (!camera.set_exposure_absolute(value)) {
        throw std::runtime_error("Camera rejected the manual exposure setting");
    }
    const auto white_stats = measure_luminance(renderer, camera, white);
    const auto black_stats = measure_luminance(renderer, camera, black);
    const ExposureEvaluation evaluation{value, white_stats, black_stats};
    std::cout << "  Exposure " << value << ": white " << std::fixed << std::setprecision(1)
              << white_stats.mean << " [" << white_stats.minimum << '-' << white_stats.maximum
              << "], black " << black_stats.mean << " [" << black_stats.minimum << '-'
              << black_stats.maximum << "], contrast " << evaluation.contrast() << ", frame "
              << std::chrono::duration<double, std::milli>(
                     std::max(white_stats.maximum_frame_interval,
                              black_stats.maximum_frame_interval))
                     .count()
              << " ms: " << (evaluation.usable() ? "stable/usable" : "not usable") << '\n';
    return evaluation;
}

ExposureEvaluation tune_manual_exposure(dualphoto::X11Renderer& renderer,
                                        dualphoto::V4L2Camera& camera,
                                        const dualphoto::Frame& white,
                                        const dualphoto::Frame& black) {
    const auto range = camera.exposure_absolute_range();
    if (!range) {
        std::cout << "Manual exposure control is unavailable; retaining driver-selected "
                     "exposure and checking its stability.\n";
        const ExposureEvaluation evaluation{std::nullopt,
                                            measure_luminance(renderer, camera, white),
                                            measure_luminance(renderer, camera, black)};
        if (!evaluation.usable()) {
            throw std::runtime_error(
                "Driver-selected exposure does not produce stable, usable black/white contrast");
        }
        return evaluation;
    }
    if (range->step <= 0 || range->maximum < range->minimum) {
        throw std::runtime_error("Camera reported an invalid manual exposure range");
    }

    const auto step_count =
        (static_cast<std::int64_t>(range->maximum) - range->minimum) / range->step;
    std::int64_t low_step = 0;
    std::int64_t high_step = step_count;
    std::optional<std::int64_t> fastest_usable_step;

    while (low_step <= high_step) {
        const auto candidate_step = low_step + (high_step - low_step) / 2;
        const auto candidate_value = static_cast<std::int32_t>(
            static_cast<std::int64_t>(range->minimum) + candidate_step * range->step);
        const auto evaluation = evaluate_exposure(renderer, camera, white, black, candidate_value);
        if (evaluation.usable()) {
            fastest_usable_step = candidate_step;
            high_step = candidate_step - 1;
        } else {
            low_step = candidate_step + 1;
        }
    }

    if (!fastest_usable_step) {
        throw std::runtime_error(
            "No exposure setting produced stable black/white images with at least " +
            std::to_string(minimum_usable_contrast) + " luma contrast");
    }

    const auto fastest_value = static_cast<std::int32_t>(static_cast<std::int64_t>(range->minimum) +
                                                         *fastest_usable_step * range->step);
    std::cout << "Verifying shortest usable exposure " << fastest_value
              << " (V4L2 100-us units).\n";
    for (std::size_t attempt = 0; attempt < exposure_verification_attempts; ++attempt) {
        const auto verified = evaluate_exposure(renderer, camera, white, black, fastest_value);
        if (verified.usable()) {
            return verified;
        }
        std::cout << "  Verification attempt " << (attempt + 1U) << '/'
                  << exposure_verification_attempts << " was unstable.\n";
    }

    std::cout << "Minimum exposure did not verify consistently; checking slightly longer "
                 "settings.\n";
    const auto fallback_end = std::min(
        step_count, *fastest_usable_step + static_cast<std::int64_t>(exposure_fallback_steps));
    for (auto candidate_step = *fastest_usable_step + 1; candidate_step <= fallback_end;
         ++candidate_step) {
        const auto candidate_value = static_cast<std::int32_t>(
            static_cast<std::int64_t>(range->minimum) + candidate_step * range->step);
        for (std::size_t attempt = 0; attempt < exposure_verification_attempts; ++attempt) {
            const auto verified =
                evaluate_exposure(renderer, camera, white, black, candidate_value);
            if (verified.usable()) {
                std::cout << "Using the shortest nearby setting that passed verification.\n";
                return verified;
            }
        }
    }

    throw std::runtime_error(
        "Exposure candidates did not remain stable through verification; check lighting, "
        "camera motion, and webcam control behavior, then retry calibration");
}

Measurement measure_transition(dualphoto::X11Renderer& renderer, dualphoto::V4L2Camera& camera,
                               const dualphoto::Frame& pattern, bool to_white, double threshold) {
    const auto presented_at = Clock::now();
    renderer.present(pattern);
    auto previous_capture_at = presented_at;
    bool has_previous_capture = false;
    auto max_frame_interval = Clock::duration::zero();
    std::uint32_t stable_samples = 0;

    while (Clock::now() - presented_at < transition_timeout) {
        const auto frame = camera.capture();
        const auto captured_at = Clock::now();
        if (has_previous_capture) {
            max_frame_interval = std::max(max_frame_interval, captured_at - previous_capture_at);
        } else {
            has_previous_capture = true;
        }
        previous_capture_at = captured_at;

        const auto luminance = center_luminance(frame);
        if ((to_white && luminance >= threshold) || (!to_white && luminance <= threshold)) {
            ++stable_samples;
            if (stable_samples >= required_stable_samples) {
                return {captured_at - presented_at, max_frame_interval};
            }
        } else {
            stable_samples = 0;
        }
    }

    throw std::runtime_error("Camera ROI did not reach the expected brightness before timeout; "
                             "check projector visibility, exposure, and requested resolution");
}

double percentile_ms(const std::vector<Measurement>& sorted, std::size_t percentile) {
    const auto index = (percentile * sorted.size() + 99U) / 100U - 1U;
    return std::chrono::duration<double, std::milli>(sorted[index].latency).count();
}

int run_calibration(const Options& options) {
    const char* display_name = options.display.empty() ? nullptr : options.display.c_str();
    const char* output_name = options.output.empty() ? nullptr : options.output.c_str();

    dualphoto::X11Renderer renderer(display_name, output_name);
    dualphoto::V4L2Camera camera(options.device, options.width, options.height);
    const auto black = make_solid_frame({0, 0, 0});
    const auto white = make_solid_frame({255, 255, 255});

    renderer.present(white);
    std::cout
        << "Projector output: " << renderer.width() << 'x' << renderer.height()
        << "; camera: " << options.device << " (requested " << options.width << 'x'
        << options.height << ")\n"
        << "Keep the camera aimed at the projected image and leave camera controls untouched.\n"
        << "Enabling automatic controls under white projection:\n";
    const auto auto_controls = camera.enable_automatic_controls();
    for (const auto& status : auto_controls) {
        std::cout << "  " << status.name << ": " << (status.configured ? "enabled" : "unavailable")
                  << " (" << status.detail << ")\n";
    }
    if (!auto_controls[0].configured && !auto_controls[1].configured) {
        throw std::runtime_error(
            "The camera exposes neither automatic exposure nor automatic gain; "
            "cannot calibrate image brightness automatically");
    }

    std::cout << "Waiting for camera brightness to stabilize under white...\n";
    const auto calibrated_luminance = auto_calibrate_camera(camera);
    std::cout << std::fixed << std::setprecision(1)
              << "Auto-calibrated ROI luma: " << calibrated_luminance << " (0-255)\n"
              << "Locking the resulting camera settings:\n";
    const auto control_locks = camera.lock_automatic_controls();
    for (const auto& status : control_locks) {
        std::cout << "  " << status.name << ": " << (status.configured ? "locked" : "not locked")
                  << " (" << status.detail << ")\n";
    }

    std::cout << "Searching for the shortest stable exposure with usable contrast...\n";
    const auto exposure = tune_manual_exposure(renderer, camera, white, black);
    const auto black_level = exposure.black.mean;
    const auto white_level = exposure.white.mean;
    const auto contrast = exposure.contrast();
    const auto exposure_frame_interval =
        std::max(exposure.white.maximum_frame_interval, exposure.black.maximum_frame_interval);
    std::cout << "Selected exposure: ";
    if (exposure.value) {
        std::cout << *exposure.value << " (V4L2 100-us units)";
    } else {
        std::cout << "driver-selected";
    }
    std::cout << ", white " << std::fixed << std::setprecision(1) << white_level << ", black "
              << black_level << ", contrast " << contrast << "; max sample interval "
              << std::chrono::duration<double, std::milli>(exposure_frame_interval).count()
              << " ms.\n";

    const auto white_threshold = black_level + contrast * 0.9;
    const auto black_threshold = white_level - contrast * 0.9;
    const auto sample_frame = camera.capture();
    std::cout << "Camera actual frame size: " << sample_frame.width << 'x' << sample_frame.height
              << '\n'
              << std::fixed << std::setprecision(1) << "Measured ROI levels: black " << black_level
              << ", white " << white_level << " (0-255 luma)\n"
              << "Measuring " << options.transitions << " alternating transitions...\n";

    renderer.present(black);
    std::this_thread::sleep_for(baseline_settle);
    for (std::size_t sample = 0; sample < exposure_warmup_frames; ++sample) {
        cmake --build build --parallel
        ./build/apps/calibrate/dualphoto-calibrate --output HDMI-1        (void)camera.capture();
    }

    std::vector<Measurement> measurements;
    measurements.reserve(options.transitions);
    auto to_white = false;
    for (std::uint32_t index = 0; index < options.transitions; ++index) {
        to_white = !to_white;
        const auto measurement =
            measure_transition(renderer, camera, to_white ? white : black, to_white,
                               to_white ? white_threshold : black_threshold);
        measurements.push_back(measurement);
        std::cout << "Transition " << (index + 1U) << '/' << options.transitions << " to "
                  << (to_white ? "white" : "black") << ": "
                  << std::chrono::duration<double, std::milli>(measurement.latency).count()
                  << " ms\n";
    }

    std::vector<Measurement> sorted = measurements;
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.latency < rhs.latency; });
    const auto max_frame_interval =
        std::max_element(measurements.begin(), measurements.end(),
                         [](const auto& lhs, const auto& rhs) {
                             return lhs.frame_interval < rhs.frame_interval;
                         })
            ->frame_interval;
    const auto recommended_wait = sorted.back().latency + max_frame_interval;

    std::cout << "Observed end-to-end latency: p50 " << percentile_ms(sorted, 50) << " ms, p95 "
              << percentile_ms(sorted, 95) << " ms, max "
              << std::chrono::duration<double, std::milli>(sorted.back().latency).count() << " ms\n"
              << "Largest camera capture interval: "
              << std::chrono::duration<double, std::milli>(max_frame_interval).count() << " ms\n"
              << "Suggested minimum settle wait: "
              << std::chrono::duration<double, std::milli>(recommended_wait).count()
              << " ms (observed max plus one capture interval)\n"
              << "Software timing only: this is an observed host-side estimate, not a "
                 "hardware-sync guarantee.\n";

    renderer.present(black);
    return 0;
}
#endif

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

#if defined(DUALPHOTO_HAVE_DISPLAY_X11) && defined(DUALPHOTO_HAVE_CAMERA_V4L2)
    try {
        return run_calibration(options);
    } catch (const std::exception& error) {
        std::cerr << "Calibration failed: " << error.what() << '\n';
        return 1;
    }
#else
    (void)options;
    std::cerr << "Calibration requires the X11 display and V4L2 camera backends. "
                 "Enable DUALPHOTO_WITH_X11 and DUALPHOTO_WITH_V4L2.\n";
    return 1;
#endif
}
