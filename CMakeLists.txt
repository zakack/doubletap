cmake_minimum_required(VERSION 3.5)

project(osu-intercept)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)

add_executable(osu-intercept osu-intercept.c)
target_compile_options(osu-intercept PRIVATE -Wall -Wextra ${PIPEWIRE_CFLAGS})
target_link_libraries(osu-intercept ${PIPEWIRE_LDFLAGS})

install(TARGETS osu-intercept RUNTIME DESTINATION bin)

