#include "uinput_output.h"

#include <errno.h>
#include <string.h>
#include <stdio.h>

int uinput_output_create(struct libevdev *source_dev, struct libevdev_uinput **uidev)
{
    int rc = libevdev_uinput_create_from_device(source_dev, LIBEVDEV_UINPUT_OPEN_MANAGED, uidev);
    if (rc != 0) {
        fprintf(stderr, "uinput create failed: %s\n", strerror(-rc));
        return -1;
    }
    fprintf(stderr, "uinput device created: %s\n", libevdev_uinput_get_devnode(*uidev));
    return 0;
}

int uinput_output_write_event(struct libevdev_uinput *uidev, const struct input_event *ev)
{
    int rc = libevdev_uinput_write_event(uidev, ev->type, ev->code, ev->value);
    if (rc != 0) {
        fprintf(stderr, "uinput write failed: %s\n", strerror(-rc));
        return -1;
    }
    return 0;
}

void uinput_output_destroy(struct libevdev_uinput *uidev)
{
    libevdev_uinput_destroy(uidev);
}
