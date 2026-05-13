#ifndef SOCD_H
#define SOCD_H

#include <linux/input.h>
#include <stddef.h>

/* Reset SOCD state machine (call once before processing events). */
void socd_init(void);

/* Process a single input event through the SOCD state machine.
 *
 * Parameters:
 *   ie                - The input event to process
 *   v1_code           - Virtual key code for state 0 (default KEY_F13 = 183)
 *   v2_code           - Virtual key code for state 1 (default KEY_F14 = 184)
 *   trigger_keys       - Array of key codes that participate in SOCD (NULL or count=0 means all)
 *   trigger_keys_count - Number of entries in trigger_keys
 *   emit               - Callback to send each output event (virtual press/release, pass-through, SYN)
 *   audio_cb           - Callback invoked on every virtual key press (may be NULL)
 */
void socd_process_event(const struct input_event *ie, int v1_code, int v2_code,
                        const int *trigger_keys, size_t trigger_keys_count,
                        void (*emit)(const struct input_event *),
                        void (*audio_cb)(void));

#endif
