#include "v4l2_camera.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace dualphoto {
namespace {

constexpr std::size_t requested_buffer_count = 4;
constexpr int capture_timeout_ms = 5000;

[[noreturn]] void throw_system_error(const std::string& operation) {
    throw std::runtime_error(operation + ": " + std::strerror(errno));
}

int xioctl(int fd, unsigned long request, void* argument) {
    int result = 0;
    do {
        result = ioctl(fd, request, argument);
    } while (result == -1 && errno == EINTR);
    return result;
}

bool unsupported_control_error(int error) {
    return error == EINVAL || error == ENOTTY;
}

std::uint8_t clamp_channel(int value) {
    return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

void yuv_to_rgb(std::uint8_t y, std::uint8_t u, std::uint8_t v, std::uint8_t* rgb) {
    const int luminance = std::max(0, static_cast<int>(y) - 16);
    const int blue_difference = static_cast<int>(u) - 128;
    const int red_difference = static_cast<int>(v) - 128;
    rgb[0] = clamp_channel((298 * luminance + 409 * red_difference + 128) >> 8);
    rgb[1] =
        clamp_channel((298 * luminance - 100 * blue_difference - 208 * red_difference + 128) >> 8);
    rgb[2] = clamp_channel((298 * luminance + 516 * blue_difference + 128) >> 8);
}

} // namespace

struct V4L2Camera::Impl {
    struct MappedBuffer {
        void* address = MAP_FAILED;
        std::size_t length = 0;
    };

    int fd = -1;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t pixel_format = 0;
    std::uint32_t bytes_per_line = 0;
    std::vector<MappedBuffer> buffers;
    bool streaming = false;

    Impl(const std::string& device, std::uint32_t requested_width, std::uint32_t requested_height) {
        if (requested_width == 0 || requested_height == 0) {
            throw std::invalid_argument("Camera dimensions must be non-zero");
        }

        fd = open(device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd == -1) {
            throw_system_error("Unable to open V4L2 device " + device);
        }

        try {
            configure(requested_width, requested_height);
            start_streaming();
        } catch (...) {
            cleanup();
            throw;
        }
    }

    ~Impl() { cleanup(); }

    void cleanup() noexcept {
        if (fd == -1) {
            return;
        }
        if (streaming) {
            v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            (void)xioctl(fd, VIDIOC_STREAMOFF, &type);
            streaming = false;
        }
        for (auto& buffer : buffers) {
            if (buffer.address != MAP_FAILED) {
                (void)munmap(buffer.address, buffer.length);
                buffer.address = MAP_FAILED;
            }
        }
        buffers.clear();
        (void)close(fd);
        fd = -1;
    }

    void configure(std::uint32_t requested_width, std::uint32_t requested_height) {
        v4l2_capability capability{};
        if (xioctl(fd, VIDIOC_QUERYCAP, &capability) == -1) {
            throw_system_error("Unable to query V4L2 device capabilities");
        }
        const auto device_caps = (capability.capabilities & V4L2_CAP_DEVICE_CAPS) != 0
                                     ? capability.device_caps
                                     : capability.capabilities;
        if ((device_caps & V4L2_CAP_VIDEO_CAPTURE) == 0 ||
            (device_caps & V4L2_CAP_STREAMING) == 0) {
            throw std::runtime_error("V4L2 device must support video capture and streaming");
        }

        v4l2_format format{};
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        format.fmt.pix.width = requested_width;
        format.fmt.pix.height = requested_height;
        format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        format.fmt.pix.field = V4L2_FIELD_ANY;
        if (xioctl(fd, VIDIOC_S_FMT, &format) == -1) {
            throw_system_error("Unable to set V4L2 capture format");
        }

        width = format.fmt.pix.width;
        height = format.fmt.pix.height;
        pixel_format = format.fmt.pix.pixelformat;
        bytes_per_line = format.fmt.pix.bytesperline;

        if (width == 0 || height == 0) {
            throw std::runtime_error("V4L2 device returned invalid frame dimensions");
        }
        if (pixel_format != V4L2_PIX_FMT_RGB24 && pixel_format != V4L2_PIX_FMT_BGR24 &&
            pixel_format != V4L2_PIX_FMT_YUYV && pixel_format != V4L2_PIX_FMT_UYVY) {
            throw std::runtime_error("V4L2 device selected an unsupported pixel format");
        }
        if ((pixel_format == V4L2_PIX_FMT_YUYV || pixel_format == V4L2_PIX_FMT_UYVY) &&
            (width % 2U) != 0) {
            throw std::runtime_error("V4L2 YUV 4:2:2 capture requires an even frame width");
        }

        const std::uint64_t bytes_per_pixel =
            pixel_format == V4L2_PIX_FMT_RGB24 || pixel_format == V4L2_PIX_FMT_BGR24 ? 3U : 2U;
        const auto minimum_line_size = static_cast<std::uint64_t>(width) * bytes_per_pixel;
        if (bytes_per_line < minimum_line_size) {
            throw std::runtime_error("V4L2 device returned an invalid row stride");
        }

        v4l2_requestbuffers request{};
        request.count = static_cast<std::uint32_t>(requested_buffer_count);
        request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        request.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd, VIDIOC_REQBUFS, &request) == -1) {
            throw_system_error("Unable to request V4L2 memory-mapped buffers");
        }
        if (request.count < 2) {
            throw std::runtime_error("V4L2 device provided fewer than two capture buffers");
        }

        buffers.resize(request.count);
        for (std::uint32_t index = 0; index < request.count; ++index) {
            v4l2_buffer buffer{};
            buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buffer.memory = V4L2_MEMORY_MMAP;
            buffer.index = index;
            if (xioctl(fd, VIDIOC_QUERYBUF, &buffer) == -1) {
                throw_system_error("Unable to query a V4L2 capture buffer");
            }

            void* address = mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                                 static_cast<off_t>(buffer.m.offset));
            if (address == MAP_FAILED) {
                throw_system_error("Unable to map a V4L2 capture buffer");
            }
            buffers[index] = {address, buffer.length};
        }
    }

    void start_streaming() {
        for (std::uint32_t index = 0; index < buffers.size(); ++index) {
            v4l2_buffer buffer{};
            buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buffer.memory = V4L2_MEMORY_MMAP;
            buffer.index = index;
            if (xioctl(fd, VIDIOC_QBUF, &buffer) == -1) {
                throw_system_error("Unable to queue a V4L2 capture buffer");
            }
        }

        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(fd, VIDIOC_STREAMON, &type) == -1) {
            throw_system_error("Unable to start V4L2 capture streaming");
        }
        streaming = true;
    }

    std::optional<v4l2_queryctrl> query_control(std::uint32_t id) const {
        v4l2_queryctrl query{};
        query.id = id;
        if (xioctl(fd, VIDIOC_QUERYCTRL, &query) == 0) {
            if ((query.flags & V4L2_CTRL_FLAG_DISABLED) != 0) {
                return std::nullopt;
            }
            return query;
        }
        if (unsupported_control_error(errno)) {
            return std::nullopt;
        }
        throw_system_error("Unable to query V4L2 control");
    }

    std::optional<std::int32_t> get_control(std::uint32_t id) const {
        if (!query_control(id)) {
            return std::nullopt;
        }
        v4l2_control control{};
        control.id = id;
        if (xioctl(fd, VIDIOC_G_CTRL, &control) == 0) {
            return control.value;
        }
        if (unsupported_control_error(errno)) {
            return std::nullopt;
        }
        throw_system_error("Unable to read V4L2 control");
    }

    bool set_control(std::uint32_t id, std::int32_t value) const {
        if (!query_control(id)) {
            return false;
        }
        v4l2_control control{};
        control.id = id;
        control.value = value;
        if (xioctl(fd, VIDIOC_S_CTRL, &control) == 0) {
            return true;
        }
        if (unsupported_control_error(errno)) {
            return false;
        }
        throw_system_error("Unable to set V4L2 control");
    }

    bool menu_value_supported(std::uint32_t id, std::int32_t value) const {
        v4l2_querymenu menu{};
        menu.id = id;
        menu.index = static_cast<std::uint32_t>(value);
        if (xioctl(fd, VIDIOC_QUERYMENU, &menu) == 0) {
            return true;
        }
        if (unsupported_control_error(errno)) {
            return false;
        }
        throw_system_error("Unable to query V4L2 control menu");
    }

    CameraControlStatus set_automatic_control(std::uint32_t id, std::int32_t automatic_value,
                                              const char* name) const {
        if (!query_control(id)) {
            return {name, false, "automatic control is unsupported"};
        }
        if (!set_control(id, automatic_value)) {
            return {name, false, "could not enable automatic mode"};
        }
        return {name, true, "automatic mode enabled"};
    }

    CameraControlStatus enable_automatic_exposure() const {
        if (!query_control(V4L2_CID_EXPOSURE_AUTO)) {
            return {"auto exposure", false, "automatic control is unsupported"};
        }

        constexpr std::array<std::pair<std::int32_t, const char*>, 3> automatic_modes = {{
            {V4L2_EXPOSURE_AUTO, "automatic"},
            {V4L2_EXPOSURE_APERTURE_PRIORITY, "aperture-priority automatic"},
            {V4L2_EXPOSURE_SHUTTER_PRIORITY, "shutter-priority automatic"},
        }};
        for (const auto& [mode, description] : automatic_modes) {
            if (menu_value_supported(V4L2_CID_EXPOSURE_AUTO, mode) &&
                set_control(V4L2_CID_EXPOSURE_AUTO, mode)) {
                return {"auto exposure", true, std::string(description) + " mode enabled"};
            }
        }
        return {"auto exposure", false, "driver rejected all advertised automatic modes"};
    }

    CameraControlStatus lock_auto_control(std::uint32_t automatic_id, std::int32_t manual_value,
                                          const char* name, std::uint32_t manual_id) const {
        if (!query_control(automatic_id)) {
            return {name, false, "automatic control is unsupported"};
        }

        const auto automatic_setting = get_control(manual_id);
        if (!set_control(automatic_id, manual_value)) {
            return {name, false, "could not disable automatic mode"};
        }
        if (!automatic_setting) {
            return {name, true, "automatic mode disabled; manual value could not be verified"};
        }
        if (!set_control(manual_id, *automatic_setting)) {
            return {name, true, "automatic mode disabled; calibrated value could not be restored"};
        }
        return {name, true, "automatic mode disabled; calibrated value restored"};
    }

    CameraControlStatus lock_auto_exposure() const {
        if (!query_control(V4L2_CID_EXPOSURE_AUTO)) {
            return {"auto exposure", false, "automatic control is unsupported"};
        }
        if (!set_control(V4L2_CID_EXPOSURE_AUTO, V4L2_EXPOSURE_MANUAL)) {
            return {"auto exposure", false, "could not select manual exposure mode"};
        }
        return {"auto exposure", true,
                "manual mode selected; calibrator will tune exposure from captured brightness"};
    }

    CameraControlStatus lock_auto_white_balance() const {
        if (!query_control(V4L2_CID_AUTO_WHITE_BALANCE)) {
            return {"auto white balance", false, "automatic control is unsupported"};
        }
        if (!set_control(V4L2_CID_AUTO_WHITE_BALANCE, 0)) {
            return {"auto white balance", false, "could not disable automatic mode"};
        }
        const auto red = get_control(V4L2_CID_RED_BALANCE);
        const auto blue = get_control(V4L2_CID_BLUE_BALANCE);
        if (red && !set_control(V4L2_CID_RED_BALANCE, *red)) {
            return {"auto white balance", true, "disabled; red balance could not be reaffirmed"};
        }
        if (blue && !set_control(V4L2_CID_BLUE_BALANCE, *blue)) {
            return {"auto white balance", true, "disabled; blue balance could not be reaffirmed"};
        }
        if (!red || !blue) {
            return {"auto white balance", true,
                    "automatic mode disabled; current color balance could not be verified"};
        }
        return {"auto white balance", true, "disabled; current color balance preserved"};
    }

    std::vector<CameraControlStatus> enable_automatic_controls() const {
        std::vector<CameraControlStatus> results;
        results.push_back(enable_automatic_exposure());
        results.push_back(set_automatic_control(V4L2_CID_AUTOGAIN, 1, "auto gain"));
        results.push_back(
            set_automatic_control(V4L2_CID_AUTO_WHITE_BALANCE, 1, "auto white balance"));
        results.push_back(set_automatic_control(V4L2_CID_FOCUS_AUTO, 1, "autofocus"));
        return results;
    }

    std::vector<CameraControlStatus> lock_automatic_controls() const {
        std::vector<CameraControlStatus> results;
        results.push_back(lock_auto_exposure());
        results.push_back(lock_auto_control(V4L2_CID_AUTOGAIN, 0, "auto gain", V4L2_CID_GAIN));
        results.push_back(lock_auto_white_balance());
        results.push_back(
            lock_auto_control(V4L2_CID_FOCUS_AUTO, 0, "autofocus", V4L2_CID_FOCUS_ABSOLUTE));
        return results;
    }

    std::optional<CameraControlRange> exposure_absolute_range() const {
        const auto control = query_control(V4L2_CID_EXPOSURE_ABSOLUTE);
        if (!control) {
            return std::nullopt;
        }
        return CameraControlRange{control->minimum, control->maximum, std::max(control->step, 1)};
    }

    bool set_exposure_absolute(std::int32_t value) const {
        const auto range = exposure_absolute_range();
        if (!range) {
            return false;
        }
        const auto offset = static_cast<std::int64_t>(value) - range->minimum;
        if (value < range->minimum || value > range->maximum || offset % range->step != 0) {
            throw std::invalid_argument("Exposure value is outside the V4L2 control range or step");
        }
        return set_control(V4L2_CID_EXPOSURE_ABSOLUTE, value);
    }

    Frame capture() {
        pollfd descriptor{fd, POLLIN, 0};
        int poll_result = 0;
        do {
            poll_result = poll(&descriptor, 1, capture_timeout_ms);
        } while (poll_result == -1 && errno == EINTR);
        if (poll_result == -1) {
            throw_system_error("Waiting for a V4L2 frame failed");
        }
        for (;;) {
            if (poll_result == 0) {
                throw std::runtime_error("Timed out waiting for a V4L2 frame");
            }
            if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                throw std::runtime_error("V4L2 device reported a capture error");
            }

            v4l2_buffer buffer{};
            buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buffer.memory = V4L2_MEMORY_MMAP;
            if (xioctl(fd, VIDIOC_DQBUF, &buffer) == 0) {
                return convert_and_requeue(buffer);
            }
            if (errno != EAGAIN) {
                throw_system_error("Unable to dequeue a V4L2 capture buffer");
            }

            descriptor.revents = 0;
            do {
                poll_result = poll(&descriptor, 1, capture_timeout_ms);
            } while (poll_result == -1 && errno == EINTR);
            if (poll_result == -1) {
                throw_system_error("Waiting for a V4L2 frame failed");
            }
        }
    }

    Frame convert_and_requeue(v4l2_buffer& buffer) {
        if (buffer.index >= buffers.size()) {
            throw std::runtime_error("V4L2 device returned an invalid buffer index");
        }

        struct RequeueBuffer {
            int fd;
            v4l2_buffer& buffer;
            bool active = true;

            void finish() {
                active = false;
                if (xioctl(fd, VIDIOC_QBUF, &buffer) == -1) {
                    throw_system_error("Unable to requeue a V4L2 capture buffer");
                }
            }

            ~RequeueBuffer() {
                if (active) {
                    (void)xioctl(fd, VIDIOC_QBUF, &buffer);
                }
            }
        } requeue{fd, buffer};

        const auto row_bytes =
            static_cast<std::uint64_t>(width) *
            (pixel_format == V4L2_PIX_FMT_RGB24 || pixel_format == V4L2_PIX_FMT_BGR24 ? 3U : 2U);
        const auto required_bytes =
            static_cast<std::uint64_t>(height - 1U) * bytes_per_line + row_bytes;
        if (buffer.bytesused > buffers[buffer.index].length || buffer.bytesused < required_bytes) {
            throw std::runtime_error("V4L2 returned an incomplete or invalid frame");
        }
        if (static_cast<std::uint64_t>(width) * height >
            static_cast<std::uint64_t>(SIZE_MAX) / 3U) {
            throw std::length_error("V4L2 frame dimensions overflow the RGB8 buffer size");
        }

        Frame frame;
        frame.width = width;
        frame.height = height;
        frame.rgb.resize(static_cast<std::size_t>(width) * height * 3U);
        const auto* source = static_cast<const std::uint8_t*>(buffers[buffer.index].address);

        if (pixel_format == V4L2_PIX_FMT_RGB24 || pixel_format == V4L2_PIX_FMT_BGR24) {
            for (std::uint32_t y = 0; y < height; ++y) {
                const auto* source_row = source + static_cast<std::size_t>(y) * bytes_per_line;
                auto* destination_row = frame.rgb.data() + static_cast<std::size_t>(y) * width * 3U;
                if (pixel_format == V4L2_PIX_FMT_RGB24) {
                    std::memcpy(destination_row, source_row, static_cast<std::size_t>(row_bytes));
                } else {
                    for (std::uint32_t x = 0; x < width; ++x) {
                        destination_row[x * 3U] = source_row[x * 3U + 2U];
                        destination_row[x * 3U + 1U] = source_row[x * 3U + 1U];
                        destination_row[x * 3U + 2U] = source_row[x * 3U];
                    }
                }
            }
        } else {
            for (std::uint32_t y = 0; y < height; ++y) {
                const auto* source_row = source + static_cast<std::size_t>(y) * bytes_per_line;
                auto* destination_row = frame.rgb.data() + static_cast<std::size_t>(y) * width * 3U;
                for (std::uint32_t x = 0; x < width; x += 2U) {
                    const auto* pair = source_row + static_cast<std::size_t>(x) * 2U;
                    const auto y0 = pixel_format == V4L2_PIX_FMT_YUYV ? pair[0] : pair[1];
                    const auto u = pixel_format == V4L2_PIX_FMT_YUYV ? pair[1] : pair[0];
                    const auto y1 = pixel_format == V4L2_PIX_FMT_YUYV ? pair[2] : pair[3];
                    const auto v = pixel_format == V4L2_PIX_FMT_YUYV ? pair[3] : pair[2];
                    yuv_to_rgb(y0, u, v, destination_row + static_cast<std::size_t>(x) * 3U);
                    yuv_to_rgb(y1, u, v, destination_row + (static_cast<std::size_t>(x) + 1U) * 3U);
                }
            }
        }
        requeue.finish();
        return frame;
    }
};

V4L2Camera::V4L2Camera(std::string device, std::uint32_t width, std::uint32_t height)
    : impl_(std::make_unique<Impl>(device, width, height)) {}

V4L2Camera::~V4L2Camera() = default;

V4L2Camera::V4L2Camera(V4L2Camera&&) noexcept = default;
V4L2Camera& V4L2Camera::operator=(V4L2Camera&&) noexcept = default;

Frame V4L2Camera::capture() {
    if (impl_ == nullptr) {
        throw std::logic_error("Cannot capture with a moved-from V4L2 camera");
    }
    return impl_->capture();
}

std::vector<CameraControlStatus> V4L2Camera::enable_automatic_controls() {
    if (impl_ == nullptr) {
        throw std::logic_error("Cannot enable controls on a moved-from V4L2 camera");
    }
    return impl_->enable_automatic_controls();
}

std::vector<CameraControlStatus> V4L2Camera::lock_automatic_controls() {
    if (impl_ == nullptr) {
        throw std::logic_error("Cannot lock controls on a moved-from V4L2 camera");
    }
    return impl_->lock_automatic_controls();
}

std::optional<CameraControlRange> V4L2Camera::exposure_absolute_range() const {
    if (impl_ == nullptr) {
        throw std::logic_error("Cannot query controls on a moved-from V4L2 camera");
    }
    return impl_->exposure_absolute_range();
}

bool V4L2Camera::set_exposure_absolute(std::int32_t value) {
    if (impl_ == nullptr) {
        throw std::logic_error("Cannot set controls on a moved-from V4L2 camera");
    }
    return impl_->set_exposure_absolute(value);
}

} // namespace dualphoto
