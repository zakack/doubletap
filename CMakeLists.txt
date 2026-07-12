cmake_minimum_required(VERSION 3.10)

project(osu-intercept VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

include(GNUInstallDirs)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
pkg_check_modules(LIBEVDEV REQUIRED libevdev)
pkg_check_modules(LIBYAML  REQUIRED yaml-0.1)

# Installation directories for the systemd user unit and udev rule. Default
# to what the host's systemd/udev report; override with -D for packaging.
if(NOT SYSTEMD_USER_UNIT_DIR)
    pkg_get_variable(SYSTEMD_USER_UNIT_DIR systemd systemduserunitdir)
    if(NOT SYSTEMD_USER_UNIT_DIR)
        set(SYSTEMD_USER_UNIT_DIR "${CMAKE_INSTALL_LIBDIR}/systemd/user")
    endif()
endif()
if(NOT UDEV_RULES_DIR)
    pkg_get_variable(UDEV_DIR udev udevdir)
    if(UDEV_DIR)
        set(UDEV_RULES_DIR "${UDEV_DIR}/rules.d")
    else()
        set(UDEV_RULES_DIR "${CMAKE_INSTALL_LIBDIR}/udev/rules.d")
    endif()
endif()

add_executable(osu-interceptd osu-interceptd.c)

target_compile_definitions(osu-interceptd PRIVATE
    DEF_WAV="${CMAKE_INSTALL_FULL_DATADIR}/osu-intercept/click.wav"
    DEF_CONFIG="${CMAKE_INSTALL_FULL_DATADIR}/osu-intercept/config.yaml"
)

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

configure_file(osu-intercept.service.in osu-intercept.service @ONLY)

install(TARGETS osu-interceptd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES   config.yaml             DESTINATION ${CMAKE_INSTALL_DATADIR}/osu-intercept)
install(FILES   click.wav               DESTINATION ${CMAKE_INSTALL_DATADIR}/osu-intercept)
install(FILES   ${CMAKE_CURRENT_BINARY_DIR}/osu-intercept.service
                                        DESTINATION ${SYSTEMD_USER_UNIT_DIR})
install(FILES   70-osu-intercept-uinput.rules
                                        DESTINATION ${UDEV_RULES_DIR})
