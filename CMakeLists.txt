cmake_minimum_required(VERSION 3.5)

project(osu-intercept)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
pkg_check_modules(LIBEVDEV REQUIRED libevdev)
pkg_check_modules(LIBYAML REQUIRED yaml-0.1)

add_executable(osu-interceptd main.c evdev_input.c uinput_output.c socd.c audio.c config.c lifecycle.c privdrop.c frame_handler.c)
target_compile_options(osu-interceptd PRIVATE -Wall -Wextra ${PIPEWIRE_CFLAGS} ${LIBEVDEV_CFLAGS} ${LIBYAML_CFLAGS})
target_link_libraries(osu-interceptd ${PIPEWIRE_LDFLAGS} ${LIBEVDEV_LDFLAGS} ${LIBYAML_LDFLAGS} -lrt)

install(TARGETS osu-interceptd RUNTIME DESTINATION bin)
install(FILES osu-intercept.service DESTINATION /etc/systemd/system)
install(FILES example-config.yaml DESTINATION /etc/osu-intercept RENAME config.yaml)

