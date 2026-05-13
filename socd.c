#include "socd.h"

static int last_key1;
static int last_key2;
static int key1_pressed;
static int key2_pressed;
static int act;

static int is_trigger_key(int code, const int *trigger_keys, size_t trigger_keys_count) {
    if (trigger_keys_count == 0)
        return 1;
    for (size_t i = 0; i < trigger_keys_count; i++) {
        if (trigger_keys[i] == code)
            return 1;
    }
    return 0;
}

void socd_init(void) {
    last_key1 = 0;
    last_key2 = 0;
    key1_pressed = 0;
    key2_pressed = 0;
    act = 0;
}

void socd_process_event(const struct input_event *ie, int v1_code, int v2_code,
                        const int *trigger_keys, size_t trigger_keys_count,
                        void (*emit)(const struct input_event *),
                        void (*audio_cb)(void)) {
    if (ie->type != EV_KEY) {
        emit(ie);
        return;
    }

    struct input_event *e = (struct input_event *)ie;

    if (ie->value == 2) {
        if (ie->code != v1_code && ie->code != v2_code) {
            emit(e);
        }
        return;
    }

    if (ie->value == 1) {
        if (!is_trigger_key(ie->code, trigger_keys, trigger_keys_count)) {
            emit(e);
            return;
        }

        if (ie->code == last_key1) {
            key1_pressed = 1;
        } else if (ie->code == last_key2) {
            key2_pressed = 1;
        } else {
            last_key1 = last_key2;
            last_key2 = ie->code;
            key1_pressed = key2_pressed;
            key2_pressed = 1;
        }

        if (key1_pressed && key2_pressed) {
            struct input_event rel_ev;
            rel_ev.time = ie->time;
            rel_ev.type = EV_KEY;
            rel_ev.code = act ? v2_code : v1_code;
            rel_ev.value = 0;
            emit(&rel_ev);

            struct input_event syn_ev;
            syn_ev.time = ie->time;
            syn_ev.type = EV_SYN;
            syn_ev.code = SYN_REPORT;
            syn_ev.value = 0;
            emit(&syn_ev);

            act = !act;
        } else {
            act = (ie->code == last_key2);
        }

        struct input_event press_ev;
        press_ev.time = ie->time;
        press_ev.type = EV_KEY;
        press_ev.code = act ? v2_code : v1_code;
        press_ev.value = 1;
        emit(&press_ev);

        if (audio_cb)
            audio_cb();

        if (ie->code != v1_code && ie->code != v2_code) {
            emit(e);
        }

    } else if (ie->value == 0) {
        int is_tracked = 0;

        if (!is_trigger_key(ie->code, trigger_keys, trigger_keys_count)) {
            emit(e);
            return;
        }

        if (ie->code == last_key1) {
            key1_pressed = 0;
            is_tracked = 1;
        } else if (ie->code == last_key2) {
            key2_pressed = 0;
            is_tracked = 1;
        }

        if (is_tracked) {
            if (key1_pressed || key2_pressed) {
                struct input_event rel_ev;
                rel_ev.time = ie->time;
                rel_ev.type = EV_KEY;
                rel_ev.code = act ? v2_code : v1_code;
                rel_ev.value = 0;
                emit(&rel_ev);

                struct input_event syn_ev;
                syn_ev.time = ie->time;
                syn_ev.type = EV_SYN;
                syn_ev.code = SYN_REPORT;
                syn_ev.value = 0;
                emit(&syn_ev);

                act = !act;

                struct input_event press_ev;
                press_ev.time = ie->time;
                press_ev.type = EV_KEY;
                press_ev.code = act ? v2_code : v1_code;
                press_ev.value = 1;
                emit(&press_ev);

                if (audio_cb)
                    audio_cb();
            } else {
                struct input_event rel_ev;
                rel_ev.time = ie->time;
                rel_ev.type = EV_KEY;
                rel_ev.code = act ? v2_code : v1_code;
                rel_ev.value = 0;
                emit(&rel_ev);
                act = 0;
            }
        }

        if (ie->code != v1_code && ie->code != v2_code) {
            emit(e);
        }
    }
}
