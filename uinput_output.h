#ifndef UINPUT_OUTPUT_H
#define UINPUT_OUTPUT_H

#include <linux/input.h>
#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>

int uinput_output_create(struct libevdev *source_dev, struct libevdev_uinput **uidev);
int uinput_output_write_event(struct libevdev_uinput *uidev, const struct input_event *ev);
void uinput_output_destroy(struct libevdev_uinput *uidev);

#endif
