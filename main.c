#include "config.h"
#include "evdev_input.h"
#include "uinput_output.h"
#include "socd.h"
#include "audio.h"
#include "lifecycle.h"
#include "privdrop.h"
#include "frame_handler.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sched.h>

int main(int argc, char *argv[]) {
    struct config cfg = {0};
    struct libevdev *dev = NULL;
    struct libevdev_uinput *uidev = NULL;
    int fd = -1;
    int ret = EXIT_FAILURE;

    // 1. Parse config
    if (config_lookup(argc, argv, &cfg) != 0)
        return EXIT_FAILURE;

    // 2. Open evdev device (requires root)
    if (evdev_input_open(cfg.device_path, &dev, &fd) != 0)
        goto cleanup_config;

    // 3. Create uinput device (mirror source, requires root)
    if (uinput_output_create(dev, &uidev) != 0)
        goto cleanup_evdev;

    // 4. Init audio (non-fatal)
    if (audio_init(cfg.wav_path) != 0)
        fprintf(stderr, "Warning: audio init failed, continuing without audio\n");

    // 5. Drop privileges (now that devices are open)
    if (privdrop_drop("nobody") != 0) {
        fprintf(stderr, "Warning: privilege drop failed, continuing as root\n");
    }

    // 6. Set SCHED_FIFO (non-fatal)
    struct sched_param rt_param = { .sched_priority = 50 };
    if (sched_setscheduler(0, SCHED_FIFO, &rt_param) < 0)
        fprintf(stderr, "Warning: could not set SCHED_FIFO: %s\n", strerror(errno));

    // 7. Init signal handlers
    lifecycle_init();

    // 8. Init SOCD state machine
    socd_init();

    fprintf(stderr, "osu-interceptd: started\n");

    // 9. Main event loop
    struct input_event ie;
    while (lifecycle_is_running()) {
        int rc = evdev_input_next_event(dev, &ie);
        if (rc == -1) {
            fprintf(stderr, "osu-interceptd: evdev read error, shutting down\n");
            break;
        }
        if (rc == 0) continue; // no event (should not happen with blocking read)

        frame_handler_process(&ie, uidev,
                              cfg.virtual_keys[0], cfg.virtual_keys[1],
                              cfg.trigger_keys, cfg.trigger_keys_count,
                              audio_trigger);
    }

    // 10. Cleanup (reverse order of init)
    fprintf(stderr, "osu-interceptd: shutting down...\n");

    // Drain audio
    audio_cleanup();

    // Destroy uinput device (removes /dev/input/eventX)
    uinput_output_destroy(uidev);

    // Close evdev device
    evdev_input_close(dev, fd);

    lifecycle_shutdown();

    config_free(&cfg);
    ret = EXIT_SUCCESS;
    goto done;

cleanup_evdev:
    evdev_input_close(dev, fd);
cleanup_config:
    config_free(&cfg);
done:
    return ret;
}
