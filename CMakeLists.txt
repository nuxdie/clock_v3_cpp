cmake_minimum_required(VERSION 3.25)

# Ensure all dependencies (SDL_image, CPR, etc.) build as static libraries
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

include(FetchContent)

project(digital_clock_v3)

FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.2.26)
# Force Static build for SDL3
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
# Turn off unnecessary SDL3 subsystems
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
set(SDL_JOYSTICK OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_HIDAPI OFF CACHE BOOL "" FORCE)
set(SDL_POWER OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
set(SDL_DIALOG OFF CACHE BOOL "" FORCE)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
    message(STATUS "Configuring SDL3 for Raspberry Pi (ARM)")
    set(SDL_SSE OFF CACHE BOOL "" FORCE)
    set(SDL_SSE2 OFF CACHE BOOL "" FORCE)
    set(SDL_SSE3 OFF CACHE BOOL "" FORCE)
    set(SDL_MMX OFF CACHE BOOL "" FORCE)
    set(SDL_AVX OFF CACHE BOOL "" FORCE)
    set(SDL_ARMNEON ON CACHE BOOL "" FORCE)
    set(SDL_X11 OFF CACHE BOOL "" FORCE)
    set(SDL_WAYLAND OFF CACHE BOOL "" FORCE)
    set(SDL_OPENGL ON CACHE BOOL "" FORCE)
    set(SDL_OPENGLES ON CACHE BOOL "" FORCE)
    set(SDL_KMSDRM ON CACHE BOOL "" FORCE)
    set(SDL_RPI OFF CACHE BOOL "" FORCE)
    set(SDL_UNIX_CONSOLE_BUILD ON CACHE BOOL "" FORCE)
else()
    message(STATUS "Configuring SDL3 for Host (Linux x64)")
    set(SDL_WAYLAND ON CACHE BOOL "" FORCE)
    set(SDL_X11 ON CACHE BOOL "" FORCE)
    set(SDL_SSE ON CACHE BOOL "" FORCE)
    set(SDL_AVX ON CACHE BOOL "" FORCE)
endif()
FetchContent_MakeAvailable(SDL3)
# Point subsequent find_package(SDL3) calls to the internal build directory
# FetchContent defines <lowercase_name>_BINARY_DIR
set(SDL3_DIR "${sdl3_BINARY_DIR}")

FetchContent_Declare(SDL3_Image
    GIT_REPOSITORY https://github.com/libsdl-org/SDL_image.git
    GIT_TAG release-3.2.4
    GIT_SHALLOW TRUE
    GIT_SUBMODULES "")  # Vendored codecs are not needed: JPEG/PNG are decoded by the built-in STB backend
# Options to make cross-compiling Image easier (use built-in codecs)
set(SDL3IMAGE_VENDORED OFF CACHE BOOL "" FORCE)
set(SDL3IMAGE_BACKEND_STB ON CACHE BOOL "" FORCE)
foreach(fmt AVIF JXL TIF WEBP)
    set(SDL3IMAGE_${fmt} OFF CACHE BOOL "" FORCE)
endforeach()
set(SDL3IMAGE_BACKEND_IMAGEIO OFF CACHE BOOL "" FORCE)
set(SDL3IMAGE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL3_Image)

FetchContent_Declare(SDL3_TTF
    GIT_REPOSITORY https://github.com/libsdl-org/SDL_ttf.git
    GIT_TAG release-3.2.2)
set(SDL3TTF_SAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL3_TTF)

FetchContent_Declare(cpr
    GIT_REPOSITORY https://github.com/libcpr/cpr.git
    GIT_TAG 1.14.1)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CPR_USE_SYSTEM_CURL ON CACHE BOOL "" FORCE)
set(CPR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CPR_CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(cpr)

FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz)
FetchContent_MakeAvailable(json)

add_executable(digital_clock_v3 main.cpp)
target_compile_options(digital_clock_v3 PRIVATE -Wno-psabi)
target_compile_features(digital_clock_v3 PRIVATE cxx_std_20)
target_link_libraries(digital_clock_v3
    PRIVATE
        cpr::cpr
        nlohmann_json::nlohmann_json
        SDL3::SDL3-static
        SDL3_image::SDL3_image
        SDL3_ttf::SDL3_ttf
)
# Since we are cross-compiling static, we sometimes need to explicitly link
# atomic if the dep libraries don't propagate it correctly on ARM.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
    target_link_libraries(digital_clock_v3 PRIVATE atomic)
endif()
target_link_options(digital_clock_v3 PRIVATE
    -static-libgcc
    -static-libstdc++
    $<$<CONFIG:Release>:-s>  # Only adds -strip flag for Release builds
)

if(NOT DEFINED GROQ_API_KEY)
    set(GROQ_API_KEY $ENV{GROQ_API_KEY})
endif()
if(NOT GROQ_API_KEY)
    message(WARNING "GROQ_API_KEY not found in environment. LLM features will not work.")
    set(GROQ_API_KEY "MISSING_KEY")
endif()
# Calendar feeds for the event horizon (optional, space-separated iCal URLs); CALENDAR_URL at run time overrides it.
if(NOT DEFINED CALENDAR_URL)
    set(CALENDAR_URL $ENV{CALENDAR_URL})
endif()
target_compile_definitions(digital_clock_v3 PRIVATE
    $<$<CONFIG:Debug>:APP_DEBUG>
    GROQ_API_KEY="${GROQ_API_KEY}"
    CALENDAR_URL="${CALENDAR_URL}"
)
