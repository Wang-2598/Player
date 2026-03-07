#include "VideoRenderer.h"
#include <cstdio>
#include <cstring>
#include <algorithm>

extern "C" {
#include <libavutil/imgutils.h>
}

VideoRenderer::VideoRenderer() = default;

VideoRenderer::~VideoRenderer() {
    if (sws_frame_) av_frame_free(&sws_frame_);
    if (sws_ctx_)   sws_freeContext(sws_ctx_);
    if (texture_)   SDL_DestroyTexture(texture_);
    if (renderer_)  SDL_DestroyRenderer(renderer_);
    if (window_)    SDL_DestroyWindow(window_);
}

bool VideoRenderer::init(const std::string& title, int width, int height) {
    window_ = SDL_CreateWindow(title.c_str(), width, height,
                               SDL_WINDOW_RESIZABLE);
    if (!window_) {
        fprintf(stderr, "[VideoRenderer] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }

    renderer_ = SDL_CreateRenderer(window_, nullptr);
    if (!renderer_) {
        fprintf(stderr, "[VideoRenderer] SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return false;
    }

    // Enable linear filtering for smooth scaling.
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

    win_w_ = width;
    win_h_ = height;
    return true;
}

void VideoRenderer::onWindowResized(int newW, int newH) {
    win_w_ = newW;
    win_h_ = newH;
}

void VideoRenderer::updateTextureSize(int w, int h, AVPixelFormat fmt) {
    if (w == tex_w_ && h == tex_h_ && fmt == tex_fmt_) return;

    if (texture_) { SDL_DestroyTexture(texture_); texture_ = nullptr; }

    SDL_PixelFormat sdl_fmt;
    switch (fmt) {
        case AV_PIX_FMT_NV12:   sdl_fmt = SDL_PIXELFORMAT_NV12; break;
        case AV_PIX_FMT_YUV420P: sdl_fmt = SDL_PIXELFORMAT_IYUV; break;
        default:                 sdl_fmt = SDL_PIXELFORMAT_IYUV; break; // Will use swscale
    }

    texture_ = SDL_CreateTexture(renderer_, sdl_fmt,
                                 SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!texture_) {
        fprintf(stderr, "[VideoRenderer] SDL_CreateTexture failed: %s\n", SDL_GetError());
        return;
    }

    // Reset swscale context if the format or size changed.
    if (sws_ctx_) { sws_freeContext(sws_ctx_); sws_ctx_ = nullptr; }
    if (sws_frame_) { av_frame_free(&sws_frame_); }

    tex_w_   = w;
    tex_h_   = h;
    tex_fmt_ = fmt;
}

SDL_FRect VideoRenderer::calcDstRect(int videoW, int videoH) const {
    if (videoW <= 0 || videoH <= 0)
        return {0, 0, (float)win_w_, (float)win_h_};

    float scale = std::min((float)win_w_ / videoW, (float)win_h_ / videoH);
    float dw = videoW * scale;
    float dh = videoH * scale;
    return {(win_w_ - dw) * 0.5f, (win_h_ - dh) * 0.5f, dw, dh};
}

AVFrame* VideoRenderer::convertToYUV420P(AVFrame* src) {
    if (!sws_frame_) {
        sws_frame_ = av_frame_alloc();
        sws_frame_->format = AV_PIX_FMT_YUV420P;
        sws_frame_->width  = src->width;
        sws_frame_->height = src->height;
        av_frame_get_buffer(sws_frame_, 0);
    }

    if (!sws_ctx_) {
        sws_ctx_ = sws_getContext(
            src->width, src->height, (AVPixelFormat)src->format,
            src->width, src->height, AV_PIX_FMT_YUV420P,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
    }

    if (sws_ctx_) {
        sws_scale(sws_ctx_, src->data, src->linesize, 0, src->height,
                  sws_frame_->data, sws_frame_->linesize);
        sws_frame_->pts = src->pts;
        sws_frame_->best_effort_timestamp = src->best_effort_timestamp;
    }
    return sws_frame_;
}

void VideoRenderer::clear() {
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
}

void VideoRenderer::blitLastFrame() {
    if (!texture_) return;
    SDL_FRect dst = calcDstRect(tex_w_, tex_h_);
    SDL_RenderTexture(renderer_, texture_, nullptr, &dst);
}

void VideoRenderer::renderFrame(AVFrame* frame) {
    if (!frame) return;

    AVPixelFormat fmt = (AVPixelFormat)frame->format;

    // Ensure texture matches the current frame dimensions and format.
    AVPixelFormat tex_input_fmt = fmt;
    if (fmt != AV_PIX_FMT_YUV420P && fmt != AV_PIX_FMT_NV12)
        tex_input_fmt = AV_PIX_FMT_YUV420P;

    updateTextureSize(frame->width, frame->height, tex_input_fmt);
    if (!texture_) return;

    AVFrame* upload_frame = frame;
    if (fmt != AV_PIX_FMT_YUV420P && fmt != AV_PIX_FMT_NV12)
        upload_frame = convertToYUV420P(frame);

    AVPixelFormat upload_fmt = (AVPixelFormat)upload_frame->format;

    if (upload_fmt == AV_PIX_FMT_NV12) {
        SDL_UpdateNVTexture(texture_, nullptr,
            upload_frame->data[0], upload_frame->linesize[0],
            upload_frame->data[1], upload_frame->linesize[1]);
    } else {
        // YUV420P / IYUV
        SDL_UpdateYUVTexture(texture_, nullptr,
            upload_frame->data[0], upload_frame->linesize[0],
            upload_frame->data[1], upload_frame->linesize[1],
            upload_frame->data[2], upload_frame->linesize[2]);
    }

    SDL_FRect dst = calcDstRect(upload_frame->width, upload_frame->height);
    SDL_RenderTexture(renderer_, texture_, nullptr, &dst);
}

void VideoRenderer::present() {
    SDL_RenderPresent(renderer_);
}
