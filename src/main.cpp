#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <algorithm>

#include <SDL3/SDL.h>

#include "Player.h"

static void printUsage(const char* prog) {
    fprintf(stdout,
        "Usage: %s [options] <video_file>\n"
        "\n"
        "Options:\n"
        "  --hw        Use NVIDIA NVDEC hardware decoder (RTX 4060)\n"
        "  --sw        Use CPU software decoder (default)\n"
        "  --vol <n>   Initial volume 0-100 (default: 80)\n"
        "\n"
        "Keyboard shortcuts:\n"
        "  Space       Toggle pause / play\n"
        "  Esc         Quit\n"
        "  Right       Fast forward 10 seconds\n"
        "  Left        Rewind 10 seconds\n"
        "  Up          Volume +5%%\n"
        "  Down        Volume -5%%\n"
        "  Tab         Cycle playback speed (0.25x - 3.0x)\n"
        "  L-Shift     Toggle OSD info overlay\n"
        "\n"
        "Mouse:\n"
        "  Wheel up    Volume +3%%\n"
        "  Wheel down  Volume -3%%\n"
        "\n"
        "Window resize is supported; the video scales to fill the window.\n",
        prog);
}

int main(int argc, char* argv[]) {
    // ----- Argument parsing -----
    Player::Options opts;
    opts.decodeMode = DecodeMode::Software;
    opts.initVolume = 0.8f;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--hw") == 0) {
            opts.decodeMode = DecodeMode::Hardware;
        } else if (strcmp(argv[i], "--sw") == 0) {
            opts.decodeMode = DecodeMode::Software;
        } else if (strcmp(argv[i], "--vol") == 0 && i + 1 < argc) {
            int vol = atoi(argv[++i]);
            opts.initVolume = std::clamp(vol, 0, 100) / 100.0f;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printUsage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            opts.filename = argv[i];
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
        }
    }

    if (opts.filename.empty()) {
        printUsage(argv[0]);
        return 1;
    }

    // ----- Initialise SDL3 -----
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    // ----- Create and run player -----
    // Use a nested scope so that ~Player() (which calls SDL_DestroyAudioStream,
    // SDL_DestroyTexture, etc.) runs BEFORE SDL_Quit().  If SDL is shut down
    // first, any SDL resource destructor causes a segfault.
    {
        Player player;
        if (!player.open(opts)) {
            fprintf(stderr, "Failed to open '%s'\n", opts.filename.c_str());
            SDL_Quit();
            return 1;
        }

        fprintf(stdout, "Decode mode: %s\n",
                opts.decodeMode == DecodeMode::Hardware ? "Hardware (NVDEC)" : "Software (CPU)");

        player.run();
    } // <-- ~Player() called here: all SDL resources released while SDL is still alive

    SDL_Quit();
    return 0;
}
