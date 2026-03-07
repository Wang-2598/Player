cmake_minimum_required(VERSION 3.20)
project(Player CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Third-party library roots
set(FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/3rdparty/ffmpeg")
set(SDL3_ROOT   "${CMAKE_SOURCE_DIR}/3rdparty/sdl3")

# Include directories
include_directories(
    "${FFMPEG_ROOT}/include"
    "${SDL3_ROOT}/include"
)

# Collect all source files under src/
file(GLOB_RECURSE SOURCES "src/*.cpp")

add_executable(Player ${SOURCES})

# Link search paths
target_link_directories(Player PRIVATE
    "${FFMPEG_ROOT}/lib"
    "${SDL3_ROOT}/lib"
)

# Link libraries
target_link_libraries(Player PRIVATE
    avcodec
    avformat
    avutil
    avfilter
    swresample
    swscale
    SDL3
)

# Windows-specific settings
if(WIN32)
    target_compile_definitions(Player PRIVATE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
    # Keep console for debug output (comment next line to hide console in release)
    # set_target_properties(Player PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

# Copy FFmpeg and SDL3 DLLs to the output directory after build
set(FFMPEG_DLLS
    "${FFMPEG_ROOT}/bin/avcodec-61.dll"
    "${FFMPEG_ROOT}/bin/avdevice-61.dll"
    "${FFMPEG_ROOT}/bin/avfilter-10.dll"
    "${FFMPEG_ROOT}/bin/avformat-61.dll"
    "${FFMPEG_ROOT}/bin/avutil-59.dll"
    "${FFMPEG_ROOT}/bin/swresample-5.dll"
    "${FFMPEG_ROOT}/bin/swscale-8.dll"
)

foreach(DLL ${FFMPEG_DLLS})
    add_custom_command(TARGET Player POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DLL}" "$<TARGET_FILE_DIR:Player>"
        COMMENT "Copying ${DLL}"
    )
endforeach()

add_custom_command(TARGET Player POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${SDL3_ROOT}/bin/SDL3.dll" "$<TARGET_FILE_DIR:Player>"
    COMMENT "Copying SDL3.dll"
)
