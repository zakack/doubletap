#include "frame_handler.h"

#include "socd.h"
#include "uinput_output.h"

#define MAX_FRAME_EVENTS 256

static struct input_event frame_buf[MAX_FRAME_EVENTS];
static size_t frame_count;

static struct libevdev_uinput *current_uidev;
static void frame_emit(const struct input_event *ev)
{
    if (ev->type == EV_SYN && ev->code == SYN_REPORT)
        return;
    if (current_uidev)
        uinput_output_write_event(current_uidev, ev);
}

void frame_handler_reset(void)
{
    frame_count = 0;
}

int frame_handler_process(struct input_event *ie,
                          struct libevdev_uinput *uidev,
                          int v1_code, int v2_code,
                          const int *trigger_keys, size_t trigger_keys_count,
                          void (*audio_cb)(void))
{
    if (!ie || !uidev)
        return -1;

    current_uidev = uidev;

    if (ie->type == EV_SYN && ie->code == SYN_DROPPED) {
        uinput_output_write_event(uidev, ie);
        frame_count = 0;
        return 1;
    }

    if (ie->type == EV_SYN && ie->code == SYN_REPORT) {
        for (size_t i = 0; i < frame_count; i++)
            socd_process_event(&frame_buf[i], v1_code, v2_code,
                               trigger_keys, trigger_keys_count,
                               frame_emit, audio_cb);

        struct input_event syn = {
            .time  = ie->time,
            .type  = EV_SYN,
            .code  = SYN_REPORT,
            .value = 0,
        };
        uinput_output_write_event(uidev, &syn);
        frame_count = 0;
        return 1;
    }

    if (ie->type == EV_SYN) {
        uinput_output_write_event(uidev, ie);
        return 0;
    }

    if (frame_count < MAX_FRAME_EVENTS)
        frame_buf[frame_count++] = *ie;

    if (frame_count >= MAX_FRAME_EVENTS) {
        for (size_t i = 0; i < frame_count; i++)
            socd_process_event(&frame_buf[i], v1_code, v2_code,
                               trigger_keys, trigger_keys_count,
                               frame_emit, audio_cb);

        struct input_event syn = {
            .time  = ie->time,
            .type  = EV_SYN,
            .code  = SYN_REPORT,
            .value = 0,
        };
        uinput_output_write_event(uidev, &syn);
        frame_count = 0;
    }

    return 0;
}
