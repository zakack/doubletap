#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "evdev_input.h"

int evdev_input_open(const char *device_path, struct libevdev **dev, int *fd)
{
	int d = open(device_path, O_RDONLY);
	if (d < 0) {
		perror("evdev_input_open: open");
		return -1;
	}

	if (libevdev_new_from_fd(d, dev) < 0) {
		fprintf(stderr, "evdev_input_open: libevdev_new_from_fd failed\n");
		close(d);
		return -1;
	}

	if (libevdev_grab(*dev, LIBEVDEV_GRAB) < 0) {
		fprintf(stderr, "evdev_input_open: libevdev_grab failed\n");
		libevdev_free(*dev);
		close(d);
		return -1;
	}

	*fd = d;
	return 0;
}

int evdev_input_next_event(struct libevdev *dev, struct input_event *ev)
{
	int rc = libevdev_next_event(dev,
		LIBEVDEV_READ_FLAG_NORMAL | LIBEVDEV_READ_FLAG_BLOCKING, ev);

	while (rc == LIBEVDEV_READ_STATUS_SYNC)
		rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_SYNC, ev);

	if (rc == -EAGAIN)
		return 0;

	if (rc != LIBEVDEV_READ_STATUS_SUCCESS) {
		fprintf(stderr, "evdev read error: %s\n", strerror(-rc));
		return -1;
	}

	return 1;
}

void evdev_input_close(struct libevdev *dev, int fd)
{
	libevdev_grab(dev, LIBEVDEV_UNGRAB);
	libevdev_free(dev);
	close(fd);
}
