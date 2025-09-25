#pragma once

#include "output.hpp"
#include <linux/videodev2.h>
#include <string>

class UVCOutput : public Output
{
public:
    UVCOutput(VideoOptions const *options);
    ~UVCOutput() override;

protected:
    void outputBuffer(void *mem, size_t size, int64_t timestamp_us, uint32_t flags) override;

private:
    // V4L2 device management
    int v4l2_fd_;
    std::string device_path_;
    uint32_t output_width_;
    uint32_t output_height_;

    // Setup and validation
    bool setupV4L2Device();
    bool isMJPEGFrame(void *mem, size_t size);
    void cleanup();

    // Statistics
    uint64_t frames_written_;
    uint64_t bytes_written_;
    uint64_t dropped_frames_;
};