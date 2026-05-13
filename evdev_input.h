#ifndef EVDEV_INPUT_H
#define EVDEV_INPUT_H

#include <linux/input.h>
#include <libevdev/libevdev.h>

int evdev_input_open(const char *device_path, struct libevdev **dev, int *fd);
int evdev_input_next_event(struct libevdev *dev, struct input_event *ev);
void evdev_input_close(struct libevdev *dev, int fd);

#endif
