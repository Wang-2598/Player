#include "OSD.h"
#include "Font8x8.h"
#include <cstdio>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <algorithm>

OSD::OSD(SDL_Renderer* renderer) : renderer_(renderer) {}

// ---------------------------------------------------------------------------
// Low-level font rendering
// ---------------------------------------------------------------------------

void OSD::drawChar(int px, int py, char c,
                   uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    int idx = (uint8_t)c - 32;
    if (idx < 0 || idx >= 96) idx = 0;

    const int scale = Config::OSD_FONT_SCALE;
    SDL_SetRenderDrawColor(renderer_, r, g, b, a);

    for (int row = 0; row < 8; ++row) {
        uint8_t rowData = font8x8[idx][row];
        for (int col = 0; col < 8; ++col) {
            if (rowData & (1u << col)) {
                SDL_FRect rect {
                    (float)(px + col * scale),
                    (float)(py + row * scale),
                    (float)scale,
                    (float)scale
                };
                SDL_RenderFillRect(renderer_, &rect);
            }
        }
    }
}

int OSD::drawText(int px, int py, const char* text,
                  uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    int x = px;
    const int charStride = Config::OSD_CHAR_W * Config::OSD_FONT_SCALE + 1;
    for (; *text; ++text) {
        drawChar(x, py, *text, r, g, b, a);
        x += charStride;
    }
    return x;
}

void OSD::drawLine(int& y, const std::string& line) {
    const int margin = Config::OSD_MARGIN;
    const int scale  = Config::OSD_FONT_SCALE;
    const int charH  = Config::OSD_CHAR_H * scale;

    // Draw a semi-transparent background strip.
    int textW = (int)(line.size()) * (Config::OSD_CHAR_W * scale + 1);
    SDL_FRect bg { (float)margin - 2, (float)y - 1,
                   (float)(textW + 6), (float)(charH + 2) };
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 160);
    SDL_RenderFillRect(renderer_, &bg);

    // Draw the text in yellow.
    drawText(margin, y, line.c_str(), 255, 255, 0, 255);

    y += charH + Config::OSD_LINE_SPACING;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string OSD::formatTime(double seconds) {
    if (seconds < 0.0) seconds = 0.0;
    int total = (int)seconds;
    int h = total / 3600;
    int m = (total % 3600) / 60;
    int s = total % 60;
    char buf[32];
    if (h > 0)
        snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, s);
    else
        snprintf(buf, sizeof(buf), "%02d:%02d", m, s);
    return buf;
}

// ---------------------------------------------------------------------------
// Main render
// ---------------------------------------------------------------------------

void OSD::render() {
    if (!visible_) return;

    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

    int y = Config::OSD_MARGIN;
    char buf[256];

    // Line 1: Playback status + time
    snprintf(buf, sizeof(buf), "[%s]  %s / %s",
             status_.c_str(),
             formatTime(current_time_).c_str(),
             formatTime(duration_).c_str());
    drawLine(y, buf);

    // Line 2: Speed + Volume
    snprintf(buf, sizeof(buf), "Speed: %.2fx   Vol: %d%%",
             speed_, (int)(volume_ * 100.0f + 0.5f));
    drawLine(y, buf);

    // Line 3: Video codec + decode mode + FPS
    snprintf(buf, sizeof(buf), "Video: %s (%s)  %.2f fps",
             vcodec_.empty() ? "N/A" : vcodec_.c_str(),
             decode_mode_.empty() ? "SW" : decode_mode_.c_str(),
             fps_);
    drawLine(y, buf);

    // Line 4: Audio codec
    snprintf(buf, sizeof(buf), "Audio: %s",
             acodec_.empty() ? "N/A" : acodec_.c_str());
    drawLine(y, buf);

    // Line 5: Resolution + bitrate
    if (res_w_ > 0 && res_h_ > 0) {
        if (vbitrate_ > 0)
            snprintf(buf, sizeof(buf), "Res: %dx%d   Bitrate: %.1f kbps",
                     res_w_, res_h_, vbitrate_ / 1000.0);
        else
            snprintf(buf, sizeof(buf), "Res: %dx%d", res_w_, res_h_);
        drawLine(y, buf);
    }
}
