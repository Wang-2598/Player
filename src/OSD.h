#pragma once

#include "Config.h"
#include <SDL3/SDL.h>
#include <string>

// On-Screen Display: renders a semi-transparent info overlay in the top-left
// corner of the window using the embedded 8x8 bitmap font.
//
// Visibility is toggled by the Left-Shift key.
// All info strings are set by the Player on each render cycle.
class OSD {
public:
    explicit OSD(SDL_Renderer* renderer);
    ~OSD() = default;

    // Toggle OSD visibility (called on Left Shift key press).
    void toggleVisible() { visible_ = !visible_; }
    bool isVisible() const { return visible_; }

    // Per-frame info setters (called by Player before render()).
    void setStatus(const std::string& status)      { status_   = status;   }
    void setCurrentTime(double seconds)             { current_time_ = seconds; }
    void setDuration(double seconds)                { duration_     = seconds; }
    void setFPS(double fps)                         { fps_      = fps;      }
    void setVideoCodec(const std::string& name)     { vcodec_   = name;    }
    void setAudioCodec(const std::string& name)     { acodec_   = name;    }
    void setResolution(int w, int h)               { res_w_ = w; res_h_ = h; }
    void setVideoBitrate(int64_t bps)               { vbitrate_ = bps;     }
    void setVolume(float vol)                       { volume_   = vol;     }
    void setSpeed(double speed)                     { speed_    = speed;   }
    void setDecodeMode(const std::string& mode)     { decode_mode_ = mode; }

    // Render the OSD overlay. Call after the video frame has been rendered.
    void render();

private:
    // Draw a single character at window pixel position (px, py).
    void drawChar(int px, int py, char c,
                  uint8_t r, uint8_t g, uint8_t b, uint8_t a);

    // Draw a null-terminated string. Returns x position after last character.
    int drawText(int px, int py, const char* text,
                 uint8_t r, uint8_t g, uint8_t b, uint8_t a);

    // Draw one OSD line and advance the y cursor.
    void drawLine(int& y, const std::string& line);

    // Format seconds into "HH:MM:SS" or "MM:SS".
    static std::string formatTime(double seconds);

    SDL_Renderer* renderer_ = nullptr;

    bool        visible_     = true;
    std::string status_;
    double      current_time_ = 0.0;
    double      duration_     = 0.0;
    double      fps_          = 0.0;
    std::string vcodec_;
    std::string acodec_;
    int         res_w_ = 0;
    int         res_h_ = 0;
    int64_t     vbitrate_    = 0;
    float       volume_      = 1.0f;
    double      speed_       = 1.0;
    std::string decode_mode_;
};
