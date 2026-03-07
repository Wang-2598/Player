#pragma once

#include <array>
#include <cstdint>

namespace Config {

    // Default window size
    constexpr int DEFAULT_WIDTH  = 1280;
    constexpr int DEFAULT_HEIGHT = 720;

    // Packet queue capacity (in number of packets)
    constexpr int VIDEO_PACKET_QUEUE_SIZE = 128;
    constexpr int AUDIO_PACKET_QUEUE_SIZE = 256;

    // Video frame queue capacity
    constexpr int VIDEO_FRAME_QUEUE_SIZE = 8;

    // Seek step in seconds (arrow keys)
    constexpr double SEEK_STEP = 10.0;

    // Volume adjustment per key press (5%) and mouse wheel (3%)
    constexpr double VOLUME_STEP_KEY   = 0.05;
    constexpr double VOLUME_STEP_WHEEL = 0.03;

    // Volume range
    constexpr double VOLUME_MIN = 0.0;
    constexpr double VOLUME_MAX = 1.0;

    // Audio output format
    constexpr int AUDIO_SAMPLE_RATE = 44100;
    constexpr int AUDIO_CHANNELS    = 2;

    // AV sync thresholds (seconds)
    constexpr double AV_SYNC_THRESHOLD_MIN = 0.04;
    constexpr double AV_SYNC_THRESHOLD_MAX = 0.10;
    constexpr double AV_NOSYNC_THRESHOLD   = 10.0;

    // If video is more than this far behind audio, drop the frame
    constexpr double FRAME_DROP_THRESHOLD = 0.5;

    // Speed levels cycled by Tab key
    constexpr std::array<double, 8> SPEED_LEVELS = {0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0};
    // Index of 1.0x in SPEED_LEVELS
    constexpr int DEFAULT_SPEED_INDEX = 3;

    // OSD rendering
    constexpr int OSD_FONT_SCALE  = 2;   // Multiplier applied to each 8x8 glyph pixel
    constexpr int OSD_CHAR_W      = 8;   // Glyph width  in pixels (before scale)
    constexpr int OSD_CHAR_H      = 8;   // Glyph height in pixels (before scale)
    constexpr int OSD_LINE_SPACING = 4;  // Extra vertical spacing between OSD lines (pixels)
    constexpr int OSD_MARGIN       = 8;  // Distance from window edge (pixels)

} // namespace Config
