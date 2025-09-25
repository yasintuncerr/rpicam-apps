#include "uvc_output.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <cstring>
#include <stdexcept>

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

    LOG(1, "UVCOutput: Initialized with device " << device_path_ 
        << " (" << output_width_ << "x" << output_height_ << ")");
}

UVCOutput::~UVCOutput()
{
    cleanup();
    LOG(1, "UVCOutput: Wrote " << frames_written_ << " frames (" 
        << bytes_written_ << " bytes), dropped " << dropped_frames_ << " frames");
}

bool UVCOutput::setupV4L2Device()
{
    // Open the V4L2 loopback device
    v4l2_fd_ = open(device_path_.c_str(), O_WRONLY);
    if (v4l2_fd_ < 0) {
        LOG(0, "UVCOutput: Failed to open " << device_path_ << ": " << strerror(errno));
        return false;
    }

    // Query device capabilities
    struct v4l2_capability cap;
    if (ioctl(v4l2_fd_, VIDIOC_QUERYCAP, &cap) < 0) {
        LOG(0, "UVCOutput: VIDIOC_QUERYCAP failed");
        close(v4l2_fd_);
        v4l2_fd_ = -1;
        return false;
    }

    // Check if device supports video output
    if (!(cap.capabilities & V4L2_CAP_VIDEO_OUTPUT)) {
        LOG(0, "UVCOutput: Device does not support video output");
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
        LOG(0, "UVCOutput: VIDIOC_S_FMT failed: " << strerror(errno));
        close(v4l2_fd_);
        v4l2_fd_ = -1;
        return false;
    }

    LOG(1, "UVCOutput: Set MJPEG format " << output_width_ << "x" << output_height_);
    
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
        LOG(1, "UVCOutput: Invalid MJPEG frame, skipping");
        dropped_frames_++;
        return;
    }

    LOG(2, "UVCOutput: Writing MJPEG frame " << size << " bytes");

    // Write frame directly to V4L2 device
    ssize_t written = write(v4l2_fd_, mem, size);
    if (written < 0) {
        LOG(0, "UVCOutput: Failed to write frame: " << strerror(errno));
        dropped_frames_++;
    } else if (written != static_cast<ssize_t>(size)) {
        LOG(1, "UVCOutput: Partial write: " << written << "/" << size);
        dropped_frames_++;
    } else {
        frames_written_++;
        bytes_written_ += size;
        LOG(2, "UVCOutput: Successfully wrote " << size << " bytes");
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