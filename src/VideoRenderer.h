#pragma once

#include "Config.h"
#include <SDL3/SDL.h>
#include <string>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

// VideoRenderer manages the SDL3 window and renderer.
// It accepts decoded AVFrames and blits them to the window, handling:
//   - Automatic scaling to the current window size (letterboxing).
//   - YUV420P frames via SDL_UpdateYUVTexture.
//   - NV12 frames via SDL_UpdateNVTexture.
//   - Any other format via libswscale conversion to YUV420P first.
class VideoRenderer {
public:
    VideoRenderer();
    ~VideoRenderer();

    // Create window and renderer. Returns false on failure.
    bool init(const std::string& title, int width, int height);

    // Render one decoded video frame. Frame is NOT freed by this call.
    void renderFrame(AVFrame* frame);

    // Re-blit the last uploaded texture without re-uploading pixel data.
    // Call when no new frame is available (paused or waiting for sync).
    void blitLastFrame();

    // Call after rendering the frame and OSD to present the backbuffer.
    void present();

    // Clear the back buffer (call at the start of each render cycle).
    void clear();

    // Update texture dimensions when the frame size changes.
    void updateTextureSize(int w, int h, AVPixelFormat fmt);

    // Notify of a window resize event.
    void onWindowResized(int newW, int newH);

    SDL_Renderer* renderer() const { return renderer_; }
    SDL_Window*   window()   const { return window_; }

    int windowWidth()  const { return win_w_; }
    int windowHeight() const { return win_h_; }

private:
    // Compute the destination rectangle that fits the video while preserving
    // its aspect ratio inside the current window (letterbox / pillarbox).
    SDL_FRect calcDstRect(int videoW, int videoH) const;

    // Convert frame to YUV420P using swscale if needed.
    AVFrame* convertToYUV420P(AVFrame* src);

    SDL_Window*   window_   = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture*  texture_  = nullptr;

    int  tex_w_   = 0;
    int  tex_h_   = 0;
    int  win_w_   = 0;
    int  win_h_   = 0;
    AVPixelFormat tex_fmt_ = AV_PIX_FMT_NONE;

    SwsContext*   sws_ctx_ = nullptr;
    AVFrame*      sws_frame_ = nullptr;
};
