#pragma once

#include "output.hpp"
#include <linux/videodev2.h>
#include <string>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
}

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
    uint32_t output_format_;
    uint32_t output_width_;
    uint32_t output_height_;

    // Format detection and validation
    bool setupV4L2Device();
    bool detectInputFormat(void *mem, size_t size);
    void cleanup();

    // MJPEG handling
    bool isMJPEGFrame(void *mem, size_t size);
    void outputMJPEGFrame(void *mem, size_t size);

    // H.264 to MJPEG transcoding
    bool setupTranscoder();
    void cleanupTranscoder();
    bool transcodeH264ToMJPEG(void *h264_data, size_t h264_size, uint8_t **mjpeg_data, size_t *mjpeg_size);

    // FFmpeg transcoding context
    AVCodecContext *decoder_context_;
    AVCodecContext *encoder_context_;
    AVFrame *decode_frame_;
    AVFrame *encode_frame_;
    AVPacket *decode_packet_;
    AVPacket *encode_packet_;
    SwsContext *sws_ctx_;

    // State management
    bool transcoding_enabled_;
    bool first_frame_;
    enum INPUT_FORMAT
    {
        FORMAT_UNKNOWN,
        FORMAT_MJPEG,
        FORMAT_H264,
        FORMAT_RAW
    } input_format_;

    // Performance optimization
    uint8_t* transcode_buffer_;
    size_t transcode_buffer_size_;

    // Statistics
    uint64_t frames_written_;
    uint64_t bytes_written_;
    uint64_t dropped_frames_;
};
