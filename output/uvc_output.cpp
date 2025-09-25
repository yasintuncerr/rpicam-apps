/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2020, Raspberry Pi (Trading) Ltd.
 *
 * uvc_output.cpp - UVC output for video streaming to V4L2 loopback devices
 */

#include "uvc_output.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <cstring>
#include <stdexcept>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

UVCOutput::UVCOutput(VideoOptions const *options) 
    : Output(options), v4l2_fd_(-1), device_path_("/dev/video0"), 
      output_format_(V4L2_PIX_FMT_MJPEG), output_width_(1920), output_height_(1080),
      decoder_context_(nullptr), encoder_context_(nullptr), decode_frame_(nullptr), 
      encode_frame_(nullptr), decode_packet_(nullptr), encode_packet_(nullptr), 
      sws_ctx_(nullptr), transcoding_enabled_(false), first_frame_(true),
      input_format_(FORMAT_UNKNOWN), transcode_buffer_(nullptr), transcode_buffer_size_(0),
      frames_written_(0), bytes_written_(0), droppped_frames_(0)
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
        << bytes_written_ << " bytes), dropped " << droppped_frames_ << " frames");
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

    // Set format
    struct v4l2_format fmt = {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    fmt.fmt.pix.width = output_width_;
    fmt.fmt.pix.height = output_height_;
    fmt.fmt.pix.pixelformat = output_format_;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    fmt.fmt.pix.colorspace = V4L2_COLORSPACE_JPEG;

    if (ioctl(v4l2_fd_, VIDIOC_S_FMT, &fmt) < 0) {
        LOG(0, "UVCOutput: VIDIOC_S_FMT failed");
        close(v4l2_fd_);
        v4l2_fd_ = -1;
        return false;
    }

    LOG(2, "UVCOutput: Set format " << output_width_ << "x" << output_height_ 
        << " format=" << std::hex << output_format_);
    
    return true;
}

void UVCOutput::outputBuffer(void *mem, size_t size, int64_t timestamp_us, uint32_t flags)
{
    if (v4l2_fd_ < 0) {
        droppped_frames_++;
        return;
    }

    LOG(2, "UVCOutput: output buffer " << mem << " size " << size);

    // Detect input format on first frame
    if (first_frame_) {
        if (!detectInputFormat(mem, size)) {
            LOG(0, "UVCOutput: Unable to detect input format");
            droppped_frames_++;
            return;
        }
        first_frame_ = false;
    }

    // Handle different input formats
    switch (input_format_) {
        case FORMAT_MJPEG:
            outputMJPEGFrame(mem, size);
            break;
            
        case FORMAT_H264:
            if (transcoding_enabled_) {
                uint8_t *mjpeg_data = nullptr;
                size_t mjpeg_size = 0;
                if (transcodeH264ToMJPEG(mem, size, &mjpeg_data, &mjpeg_size)) {
                    outputMJPEGFrame(mjpeg_data, mjpeg_size);
                    av_free(mjpeg_data);
                } else {
                    droppped_frames_++;
                }
            } else {
                LOG(1, "UVCOutput: H.264 input detected but transcoding not enabled");
                droppped_frames_++;
            }
            break;
            
        default:
            LOG(1, "UVCOutput: Unsupported input format");
            droppped_frames_++;
            return;
    }
}

bool UVCOutput::detectInputFormat(void *mem, size_t size)
{
    if (size < 4) {
        return false;
    }

    uint8_t *data = static_cast<uint8_t*>(mem);
    
    // Check for MJPEG (starts with FF D8, ends with FF D9)
    if (isMJPEGFrame(mem, size)) {
        input_format_ = FORMAT_MJPEG;
        LOG(1, "UVCOutput: Detected MJPEG input format");
        return true;
    }
    
    // Check for H.264 NAL unit (starts with 0x00 0x00 0x00 0x01)
    if (size > 4 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00 && data[3] == 0x01) {
        input_format_ = FORMAT_H264;
        LOG(1, "UVCOutput: Detected H.264 input format");
        
        // Setup transcoding if needed
        if (output_format_ == V4L2_PIX_FMT_MJPEG) {
            transcoding_enabled_ = setupTranscoder();
            if (!transcoding_enabled_) {
                LOG(0, "UVCOutput: Failed to setup H.264 to MJPEG transcoder");
                return false;
            }
        }
        return true;
    }
    
    // Check for H.264 Annex-B format (starts with 0x00 0x00 0x01)
    if (size > 3 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01) {
        input_format_ = FORMAT_H264;
        LOG(1, "UVCOutput: Detected H.264 Annex-B input format");
        
        if (output_format_ == V4L2_PIX_FMT_MJPEG) {
            transcoding_enabled_ = setupTranscoder();
            if (!transcoding_enabled_) {
                LOG(0, "UVCOutput: Failed to setup H.264 to MJPEG transcoder");
                return false;
            }
        }
        return true;
    }

    LOG(1, "UVCOutput: Unknown input format");
    return false;
}

bool UVCOutput::isMJPEGFrame(void *mem, size_t size)
{
    if (size < 4) return false;
    
    uint8_t *data = static_cast<uint8_t*>(mem);
    
    // Check JPEG SOI marker (FF D8)
    if (data[0] != 0xFF || data[1] != 0xD8) {
        return false;
    }
    
    // Check JPEG EOI marker (FF D9) at the end
    if (data[size-2] != 0xFF || data[size-1] != 0xD9) {
        return false;
    }
    
    return true;
}

void UVCOutput::outputMJPEGFrame(void *mem, size_t size)
{
    ssize_t written = write(v4l2_fd_, mem, size);
    if (written < 0) {
        LOG(0, "UVCOutput: Failed to write frame: " << strerror(errno));
        droppped_frames_++;
    } else if (written != static_cast<ssize_t>(size)) {
        LOG(1, "UVCOutput: Partial write: " << written << "/" << size);
        droppped_frames_++;
    } else {
        frames_written_++;
        bytes_written_ += size;
        LOG(2, "UVCOutput: Successfully wrote " << size << " bytes");
    }
}

bool UVCOutput::setupTranscoder()
{
    // Initialize FFmpeg if not already done
    static bool ffmpeg_initialized = false;
    if (!ffmpeg_initialized) {
        av_log_set_level(AV_LOG_WARNING);
        ffmpeg_initialized = true;
    }

    // Find H.264 decoder
    const AVCodec *h264_decoder = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!h264_decoder) {
        LOG(0, "UVCOutput: H.264 decoder not found");
        return false;
    }

    // Create decoder context
    decoder_context_ = avcodec_alloc_context3(h264_decoder);
    if (!decoder_context_) {
        LOG(0, "UVCOutput: Failed to allocate H.264 decoder context");
        return false;
    }

    // Open decoder
    if (avcodec_open2(decoder_context_, h264_decoder, nullptr) < 0) {
        LOG(0, "UVCOutput: Failed to open H.264 decoder");
        cleanupTranscoder();
        return false;
    }

    // Find MJPEG encoder
    const AVCodec *mjpeg_encoder = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!mjpeg_encoder) {
        LOG(0, "UVCOutput: MJPEG encoder not found");
        cleanupTranscoder();
        return false;
    }

    // Create encoder context
    encoder_context_ = avcodec_alloc_context3(mjpeg_encoder);
    if (!encoder_context_) {
        LOG(0, "UVCOutput: Failed to allocate MJPEG encoder context");
        cleanupTranscoder();
        return false;
    }

    // Configure encoder
    encoder_context_->width = output_width_;
    encoder_context_->height = output_height_;
    encoder_context_->pix_fmt = AV_PIX_FMT_YUVJ420P;
    encoder_context_->time_base = {1, 30}; // 30 FPS
    encoder_context_->quality = FF_QP2LAMBDA * 2; // High quality

    // Open encoder
    if (avcodec_open2(encoder_context_, mjpeg_encoder, nullptr) < 0) {
        LOG(0, "UVCOutput: Failed to open MJPEG encoder");
        cleanupTranscoder();
        return false;
    }

    // Allocate frames and packets
    decode_frame_ = av_frame_alloc();
    encode_frame_ = av_frame_alloc();
    decode_packet_ = av_packet_alloc();
    encode_packet_ = av_packet_alloc();

    if (!decode_frame_ || !encode_frame_ || !decode_packet_ || !encode_packet_) {
        LOG(0, "UVCOutput: Failed to allocate frames/packets");
        cleanupTranscoder();
        return false;
    }

    // Configure encode frame
    encode_frame_->format = AV_PIX_FMT_YUVJ420P;
    encode_frame_->width = output_width_;
    encode_frame_->height = output_height_;

    if (av_frame_get_buffer(encode_frame_, 32) < 0) {
        LOG(0, "UVCOutput: Failed to allocate encode frame buffer");
        cleanupTranscoder();
        return false;
    }

    // Allocate transcode buffer
    transcode_buffer_size_ = output_width_ * output_height_ * 3; // Conservative estimate
    transcode_buffer_ = static_cast<uint8_t*>(av_malloc(transcode_buffer_size_));
    if (!transcode_buffer_) {
        LOG(0, "UVCOutput: Failed to allocate transcode buffer");
        cleanupTranscoder();
        return false;
    }

    LOG(1, "UVCOutput: H.264 to MJPEG transcoder setup successfully");
    return true;
}

bool UVCOutput::transcodeH264ToMJPEG(void *h264_data, size_t h264_size, uint8_t **mjpeg_data, size_t *mjpeg_size)
{
    if (!transcoding_enabled_) {
        return false;
    }

    // Prepare input packet
    decode_packet_->data = static_cast<uint8_t*>(h264_data);
    decode_packet_->size = h264_size;

    // Send packet to decoder
    int ret = avcodec_send_packet(decoder_context_, decode_packet_);
    if (ret < 0) {
        LOG(2, "UVCOutput: avcodec_send_packet failed: " << ret);
        return false;
    }

    // Receive frame from decoder
    ret = avcodec_receive_frame(decoder_context_, decode_frame_);
    if (ret < 0) {
        if (ret != AVERROR(EAGAIN)) {
            LOG(2, "UVCOutput: avcodec_receive_frame failed: " << ret);
        }
        return false;
    }

    // Setup scaling context if needed
    if (!sws_ctx_ || decode_frame_->width != decoder_context_->width || 
        decode_frame_->height != decoder_context_->height) {
        
        if (sws_ctx_) {
            sws_freeContext(sws_ctx_);
        }
        
        sws_ctx_ = sws_getContext(
            decode_frame_->width, decode_frame_->height, 
            static_cast<AVPixelFormat>(decode_frame_->format),
            output_width_, output_height_, AV_PIX_FMT_YUVJ420P,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );
        
        if (!sws_ctx_) {
            LOG(0, "UVCOutput: Failed to create scaling context");
            return false;
        }
    }

    // Scale frame
    sws_scale(sws_ctx_, decode_frame_->data, decode_frame_->linesize,
              0, decode_frame_->height, encode_frame_->data, encode_frame_->linesize);

    // Encode to MJPEG
    ret = avcodec_send_frame(encoder_context_, encode_frame_);
    if (ret < 0) {
        LOG(2, "UVCOutput: avcodec_send_frame failed: " << ret);
        return false;
    }

    ret = avcodec_receive_packet(encoder_context_, encode_packet_);
    if (ret < 0) {
        LOG(2, "UVCOutput: avcodec_receive_packet failed: " << ret);
        return false;
    }

    // Allocate output buffer and copy data
    *mjpeg_size = encode_packet_->size;
    *mjpeg_data = static_cast<uint8_t*>(av_malloc(*mjpeg_size));
    if (!*mjpeg_data) {
        av_packet_unref(encode_packet_);
        return false;
    }

    memcpy(*mjpeg_data, encode_packet_->data, *mjpeg_size);
    av_packet_unref(encode_packet_);

    return true;
}

void UVCOutput::cleanupTranscoder()
{
    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }

    if (decode_frame_) {
        av_frame_free(&decode_frame_);
    }

    if (encode_frame_) {
        av_frame_free(&encode_frame_);
    }

    if (decode_packet_) {
        av_packet_free(&decode_packet_);
    }

    if (encode_packet_) {
        av_packet_free(&encode_packet_);
    }

    if (decoder_context_) {
        avcodec_close(decoder_context_);
        avcodec_free_context(&decoder_context_);
    }

    if (encoder_context_) {
        avcodec_close(encoder_context_);
        avcodec_free_context(&encoder_context_);
    }

    if (transcode_buffer_) {
        av_free(transcode_buffer_);
        transcode_buffer_ = nullptr;
        transcode_buffer_size_ = 0;
    }

    transcoding_enabled_ = false;
}

void UVCOutput::cleanup()
{
    cleanupTranscoder();
    
    if (v4l2_fd_ >= 0) {
        close(v4l2_fd_);
        v4l2_fd_ = -1;
    }
}