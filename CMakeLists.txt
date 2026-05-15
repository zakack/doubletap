cmake_minimum_required(VERSION 3.10)

project(osu-intercept C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

include(GNUInstallDirs)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
pkg_check_modules(LIBEVDEV REQUIRED libevdev)
pkg_check_modules(LIBYAML  REQUIRED yaml-0.1)

add_executable(osu-interceptd osu-interceptd.c)

target_compile_options(osu-interceptd PRIVATE
    -Wall -Wextra
    ${PIPEWIRE_CFLAGS_OTHER}
    ${LIBEVDEV_CFLAGS_OTHER}
    ${LIBYAML_CFLAGS_OTHER}
)

target_include_directories(osu-interceptd PRIVATE
    ${PIPEWIRE_INCLUDE_DIRS}
    ${LIBEVDEV_INCLUDE_DIRS}
    ${LIBYAML_INCLUDE_DIRS}
)

target_link_directories(osu-interceptd PRIVATE
    ${PIPEWIRE_LIBRARY_DIRS}
    ${LIBEVDEV_LIBRARY_DIRS}
    ${LIBYAML_LIBRARY_DIRS}
)

target_link_libraries(osu-interceptd PRIVATE
    ${PIPEWIRE_LIBRARIES}
    ${LIBEVDEV_LIBRARIES}
    ${LIBYAML_LIBRARIES}
)

install(TARGETS osu-interceptd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES   config.yaml             DESTINATION ${CMAKE_INSTALL_DATADIR}/osu-intercept)
install(FILES   osu-intercept.service   DESTINATION ${CMAKE_INSTALL_DATADIR}/osu-intercept)
