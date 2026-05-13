#ifndef FRAME_HANDLER_H
#define FRAME_HANDLER_H

#include <linux/input.h>
#include <libevdev/libevdev-uinput.h>

/* Process one evdev event. Handles per-frame accumulation, SYN_DROPPED
 * passthrough, and batched SOCD processing on SYN_REPORT.
 *
 * Parameters:
 *   ie                - pointer to the input event (borrowed, not stored)
 *   uidev             - uinput device for output
 *   v1_code           - virtual key code for SOCD state 0
 *   v2_code           - virtual key code for SOCD state 1
 *   trigger_keys      - array of key codes that trigger SOCD (NULL/0 = all)
 *   trigger_keys_count - number of entries in trigger_keys
 *   audio_cb          - callback invoked on every virtual key press (may be NULL)
 *
 * Returns:
 *    1 = event fully handled (frame emitted or passthrough written)
 *    0 = event buffered, more events needed to complete frame
 *   -1 = invalid parameter
 */
int frame_handler_process(struct input_event *ie,
                          struct libevdev_uinput *uidev,
                          int v1_code, int v2_code,
                          const int *trigger_keys, size_t trigger_keys_count,
                          void (*audio_cb)(void));

/* Reset buffered frame state. */
void frame_handler_reset(void);

#endif
