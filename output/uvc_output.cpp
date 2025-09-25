#include "uvc_output.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <cstring>
#include <stdexcept>
#include <iostream>

UVCOutput::UVCOutput(VideoOptions const *options) 
    : Output(options), v4l2_fd_(-1), device_path_("/dev/video0"), 
      output_width_(1920), output_height_(1080),
      frames_written_(0), bytes_written_(0), dropped_frames_(0)
{
    // Parse device path from options if provided
    const std::string& output = options->Get().output;
    if (output.find("/dev/video") == 0) {
        device_path_ = output;
    }

    // Extract width/height from video options if available
    if (options->Get().width > 0 && options->Get().height > 0) {
        output_width_ = options->Get().width;
        output_height_ = options->Get().height;
    }

    if (!setupV4L2Device()) {
        throw std::runtime_error("Failed to setup V4L2 device: " + device_path_);
    }

    std::cout << "UVCOutput: Initialized with device " << device_path_ 
              << " (" << output_width_ << "x" << output_height_ << ")" << std::endl;
}

UVCOutput::~UVCOutput()
{
    cleanup();
    std::cout << "UVCOutput: Wrote " << frames_written_ << " frames (" 
              << bytes_written_ << " bytes), dropped " << dropped_frames_ << " frames" << std::endl;
}

bool UVCOutput::setupV4L2Device()
{
    // Open the V4L2 loopback device
    v4l2_fd_ = open(device_path_.c_str(), O_WRONLY);
    if (v4l2_fd_ < 0) {
        std::cerr << "UVCOutput: Failed to open " << device_path_ << ": " << strerror(errno) << std::endl;
        return false;
    }

    // Query device capabilities
    struct v4l2_capability cap;
    if (ioctl(v4l2_fd_, VIDIOC_QUERYCAP, &cap) < 0) {
        std::cerr << "UVCOutput: VIDIOC_QUERYCAP failed" << std::endl;
        close(v4l2_fd_);
        v4l2_fd_ = -1;
        return false;
    }

    // Check if device supports video output
    if (!(cap.capabilities & V4L2_CAP_VIDEO_OUTPUT)) {
        std::cerr << "UVCOutput: Device does not support video output" << std::endl;
        close(v4l2_fd_);
        v4l2_fd_ = -1;
        return false;
    }

    // Set MJPEG format
    struct v4l2_format fmt = {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    fmt.fmt.pix.width = output_width_;
    fmt.fmt.pix.height = output_height_;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    fmt.fmt.pix.colorspace = V4L2_COLORSPACE_JPEG;

    if (ioctl(v4l2_fd_, VIDIOC_S_FMT, &fmt) < 0) {
        std::cerr << "UVCOutput: VIDIOC_S_FMT failed: " << strerror(errno) << std::endl;
        close(v4l2_fd_);
        v4l2_fd_ = -1;
        return false;
    }

    std::cout << "UVCOutput: Set MJPEG format " << output_width_ << "x" << output_height_ << std::endl;
    
    return true;
}

void UVCOutput::outputBuffer(void *mem, size_t size, int64_t timestamp_us, uint32_t flags)
{
    if (v4l2_fd_ < 0) {
        dropped_frames_++;
        return;
    }

    // Check if this looks like a valid MJPEG frame
    if (!isMJPEGFrame(mem, size)) {
        std::cerr << "UVCOutput: Invalid MJPEG frame, skipping" << std::endl;
        dropped_frames_++;
        return;
    }

    // Write frame directly to V4L2 device
    ssize_t written = write(v4l2_fd_, mem, size);
    if (written < 0) {
        std::cerr << "UVCOutput: Failed to write frame: " << strerror(errno) << std::endl;
        dropped_frames_++;
    } else if (written != static_cast<ssize_t>(size)) {
        std::cerr << "UVCOutput: Partial write: " << written << "/" << size << std::endl;
        dropped_frames_++;
    } else {
        frames_written_++;
        bytes_written_ += size;
    }
}

bool UVCOutput::isMJPEGFrame(void *mem, size_t size)
{
    if (size < 4) {
        return false;
    }
    
    uint8_t *data = static_cast<uint8_t*>(mem);
    
    // Check JPEG SOI marker (0xFF 0xD8)
    if (data[0] != 0xFF || data[1] != 0xD8) {
        return false;
    }
    
    // Check JPEG EOI marker (0xFF 0xD9) at the end
    if (data[size-2] != 0xFF || data[size-1] != 0xD9) {
        return false;
    }
    
    return true;
}

void UVCOutput::cleanup()
{
    if (v4l2_fd_ >= 0) {
        close(v4l2_fd_);
        v4l2_fd_ = -1;
    }
}