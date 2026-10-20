cmake_minimum_required(VERSION 3.20)
project(udm_lcd LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(UDM_BUILD_GUI "Build the native SDL2 Dear ImGui GUI" ON)
find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED IMPORTED_TARGET jsoncpp liblz4 openssl)
add_executable(udm-lcd src/main.cpp src/common.cpp src/media.cpp src/player.cpp src/remote.cpp src/firmware.cpp src/device.cpp)
target_include_directories(udm-lcd PRIVATE src)
if(CMAKE_CROSSCOMPILING)
    target_include_directories(udm-lcd PRIVATE ${DEPS_INCLUDE_DIRS})
    target_link_directories(udm-lcd PRIVATE ${DEPS_STATIC_LIBRARY_DIRS})
    target_link_libraries(udm-lcd PRIVATE ${DEPS_STATIC_LIBRARIES} pthread dl)
else()
    target_link_libraries(udm-lcd PRIVATE PkgConfig::DEPS pthread)
endif()
target_compile_options(udm-lcd PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
install(TARGETS udm-lcd RUNTIME DESTINATION bin)
install(FILES CMakeLists.txt DESTINATION share/udm-lcd)
install(DIRECTORY firmware DESTINATION share/udm-lcd)

if(UDM_BUILD_GUI AND NOT CMAKE_CROSSCOMPILING)
    include(FetchContent)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG f5befd2d29e66809cd1110a152e375a7f1981f06
    )
    FetchContent_MakeAvailable(imgui)
    add_executable(udm-lcd-gui
        src/gui.cpp
        src/gui_process.cpp
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer2.cpp
    )
    target_include_directories(udm-lcd-gui PRIVATE ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
    target_link_libraries(udm-lcd-gui PRIVATE PkgConfig::SDL2 PkgConfig::DEPS pthread)
    add_dependencies(udm-lcd-gui udm-lcd)
    target_compile_options(udm-lcd-gui PRIVATE -Wall -Wextra -Wpedantic)
    install(TARGETS udm-lcd-gui RUNTIME DESTINATION bin)
endif()
