cmake_minimum_required(VERSION 3.20)
# macOS: a universal app (Apple silicon and Intel) for macOS 11 and later
# (before project(), so every target gets them).
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND NOT CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
endif()
project(SMASPort LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_options(/W3 /O2 /Oi /GL /fp:fast /MP /utf-8 /wd4244 /wd4267 /wd4305 /wd4800 /wd4146)
  add_link_options(/LTCG)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(CORE_SOURCES
  src/core/cpu65816.cpp
  src/core/apu.cpp
  src/core/dsp.cpp
  src/core/ppu.cpp
  src/core/snes.cpp
  src/core/disasm.cpp
  src/core/png.cpp
  src/core/fileio.cpp
  src/core/rewind.cpp
  src/core/msu1.cpp
  src/game/achievements.cpp
  src/game/smas.cpp
  src/game/smb1.cpp
  src/game/smb1coop.cpp
  src/game/palette.cpp
  src/game/smb3.cpp
  src/game/smb3coop.cpp
  src/game/smb2.cpp
)

add_library(smascore STATIC ${CORE_SOURCES})
target_include_directories(smascore PUBLIC src)

if(WIN32)
add_executable(smastool tools/smastool.cpp)
target_link_libraries(smastool smascore)

add_executable(SuperMarioAllStars WIN32
  src/app/app.cpp
  src/app/app_win.cpp
  src/app/autotest_win.cpp
  src/app/sys_win.cpp
  src/app/config.cpp
  src/app/input.cpp
  src/app/audio.cpp
  src/app/video.cpp
  src/app/menu.cpp
  src/app/launcher.cpp
  src/app/canvas_d2d.cpp
  src/app/utf8.cpp
  src/app/achstore.cpp
  src/app/monitors.cpp
  src/app/music.cpp
  src/app/slang.cpp
  src/app/imageload.cpp
  src/app/msupack.cpp
  src/app/achexport.cpp
  src/app/updatecheck.cpp
  src/app/app.rc
)
target_include_directories(SuperMarioAllStars PRIVATE third_party/stb)
target_link_libraries(SuperMarioAllStars smascore
  d3d11 dxgi d3dcompiler d2d1 dwrite xaudio2 xinput dinput8 dxguid winmm comdlg32 shell32 ole32 user32 gdi32 winhttp)
set_target_properties(SuperMarioAllStars PROPERTIES OUTPUT_NAME "SuperMarioAllStars")
target_link_options(SuperMarioAllStars PRIVATE /MANIFEST:NO)
# librashader (slang shader presets), loaded at run time when present
add_custom_command(TARGET SuperMarioAllStars POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/third_party/librashader/librashader.dll $<TARGET_FILE_DIR:SuperMarioAllStars>
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/third_party/fonts/LuckiestGuy-Regular.ttf $<TARGET_FILE_DIR:SuperMarioAllStars>
  # the bundled slang presets (libretro slang-shaders, with their licences) and the player README
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/third_party/slang-shaders $<TARGET_FILE_DIR:SuperMarioAllStars>/shaders
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/release/README.txt $<TARGET_FILE_DIR:SuperMarioAllStars>
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/third_party/librashader/LICENSE.md $<TARGET_FILE_DIR:SuperMarioAllStars>/librashader-LICENSE.md)
elseif(APPLE)
  # macOS: the same SDL sound / controller layer as Android and Linux, with
  # its own entry (src/platform/macos) and Metal video (no OpenGL). Built as
  # an app bundle; SDL2 from third_party/SDL2-*-src when it's there
  # (packaging/macos/fetch_sdl.sh: static, for a self-contained universal
  # app), else an installed one (-DCMAKE_OSX_ARCHITECTURES=arm64 for Homebrew's).
  enable_language(OBJCXX)
  if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
  endif()
  file(GLOB SDL_SRC_DIR LIST_DIRECTORIES true ${CMAKE_SOURCE_DIR}/third_party/SDL2-*-src)
  if(SDL_SRC_DIR)
    list(GET SDL_SRC_DIR 0 SDL_SRC_DIR)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    add_subdirectory(${SDL_SRC_DIR} ${CMAKE_BINARY_DIR}/SDL EXCLUDE_FROM_ALL)
    set(SDL_LIB SDL2::SDL2-static)
  else()
    find_package(SDL2 REQUIRED)
    set(SDL_LIB SDL2::SDL2)
  endif()
  target_compile_options(smascore PRIVATE -O2)
  add_executable(smastool tools/smastool.cpp)
  target_link_libraries(smastool smascore)
  set(MAC_RESOURCES
    ${CMAKE_SOURCE_DIR}/third_party/fonts/LuckiestGuy-Regular.ttf
    ${CMAKE_SOURCE_DIR}/third_party/fonts/LICENSE.txt
    ${CMAKE_SOURCE_DIR}/release/README.txt
    ${CMAKE_SOURCE_DIR}/packaging/macos/AppIcon.icns)
  set_source_files_properties(${MAC_RESOURCES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  add_executable(SuperMarioAllStars MACOSX_BUNDLE
    src/app/app.cpp
    src/app/config.cpp
    src/app/menu.cpp
    src/app/launcher.cpp
    src/app/music.cpp
    src/app/achstore.cpp
    src/app/utf8.cpp
    src/app/sys_posix.cpp
    src/app/imageload.cpp
    src/app/msupack.cpp
    src/app/achexport.cpp
    src/app/updatecheck.cpp
    src/platform/android/audio_sdl.cpp
    src/platform/android/input_sdl.cpp
    src/platform/android/touch.cpp
    src/platform/macos/video_metal.mm
    src/platform/macos/slang_metal.mm
    src/platform/macos/app_macos.mm
    ${MAC_RESOURCES}
  )
  set_target_properties(SuperMarioAllStars PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/src/platform/macos/Info.plist.in
    MACOSX_BUNDLE_BUNDLE_NAME "Super Mario All-Stars"
    MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.darhkfox.supermarioallstars"
    XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES)
  set_source_files_properties(src/platform/macos/video_metal.mm src/platform/macos/slang_metal.mm
    src/platform/macos/app_macos.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  target_compile_options(SuperMarioAllStars PRIVATE -O2 -Wno-unused-parameter -Wno-sign-compare)
  target_include_directories(SuperMarioAllStars PRIVATE third_party/stb)
  # the bundled slang presets, and librashader (loaded at run time when it's
  # there: packaging/macos/build_librashader.sh builds it)
  add_custom_command(TARGET SuperMarioAllStars POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/third_party/slang-shaders
      $<TARGET_BUNDLE_CONTENT_DIR:SuperMarioAllStars>/Resources/shaders
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/third_party/librashader/LICENSE.md
      $<TARGET_BUNDLE_CONTENT_DIR:SuperMarioAllStars>/Resources/librashader-LICENSE.md)
  if(EXISTS ${CMAKE_SOURCE_DIR}/third_party/librashader/librashader.dylib)
    add_custom_command(TARGET SuperMarioAllStars POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_BUNDLE_CONTENT_DIR:SuperMarioAllStars>/Frameworks
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/third_party/librashader/librashader.dylib
        $<TARGET_BUNDLE_CONTENT_DIR:SuperMarioAllStars>/Frameworks/librashader.dylib)
  endif()
  target_link_libraries(SuperMarioAllStars smascore ${SDL_LIB}
    "-framework Cocoa" "-framework Metal" "-framework QuartzCore" "-framework CoreText"
    "-framework UniformTypeIdentifiers")
elseif(UNIX AND NOT ANDROID)
  # Desktop Linux (the Steam Deck): the Android build's SDL / OpenGL ES 3 layer
  # with its own entry (src/platform/linux); FreeType draws the text. Packed
  # as an AppImage by .github/workflows/linux.yml.
  if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
  endif()
  find_package(SDL2 REQUIRED)
  find_package(Freetype REQUIRED)
  find_package(Threads REQUIRED)
  target_compile_options(smascore PRIVATE -O2)
  add_executable(SuperMarioAllStars
    src/app/app.cpp
    src/app/config.cpp
    src/app/menu.cpp
    src/app/launcher.cpp
    src/app/music.cpp
    src/app/achstore.cpp
    src/app/utf8.cpp
    src/app/sys_posix.cpp
    src/app/imageload.cpp
    src/app/msupack.cpp
    src/app/achexport.cpp
    src/app/updatecheck.cpp
    src/platform/android/video_gles.cpp
    src/platform/android/audio_sdl.cpp
    src/platform/android/input_sdl.cpp
    src/platform/android/touch.cpp
    src/platform/linux/app_linux.cpp
  )
  target_compile_options(SuperMarioAllStars PRIVATE -O2 -Wno-unused-parameter -Wno-sign-compare)
  target_include_directories(SuperMarioAllStars PRIVATE third_party/stb)
  target_link_libraries(SuperMarioAllStars smascore SDL2::SDL2 Freetype::Freetype GLESv2 Threads::Threads)
endif()
