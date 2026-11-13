#include "v4l2_camera.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
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

} // namespace dualphoto
