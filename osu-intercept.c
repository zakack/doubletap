#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <linux/input.h>

// Default virtual keys
#define V1 KEY_Z
#define V2 KEY_X

void print_usage(FILE *stream, const char *program) {
    // clang-format off
    fprintf(stream,
            "osu-intercept-passthrough - dynamic negative edge filter with physical passthrough\n"
            "\n"
            "usage: %s [-h | -Z virtual1 | -X virtual2]\n"
            "\n"
            "options:\n"
            "    -h show this message and exit\n"
            "    -Z key code for first virtual key (default: %d)\n"
            "    -X key code for second virtual key (default: %d)\n",
            program, V1, V2);
    // clang-format on
}

static inline int read_ev(struct input_event *ev) {
    return fread(ev, sizeof(struct input_event), 1, stdin) == 1;
}

static inline void write_ev(const struct input_event *ev) {
    if (fwrite(ev, sizeof(struct input_event), 1, stdout) != 1)
        exit(EXIT_FAILURE);
}

int main(int argc, char *argv[]) {
    int v1_code = 0;
    int v2_code = 0;

    for (int opt; (opt = getopt(argc, argv, "hZ:X:")) != -1;) {
        switch (opt) {
            case 'h': return print_usage(stdout, argv[0]), EXIT_SUCCESS;
            case 'Z': v1_code = atoi(optarg); continue;
            case 'X': v2_code = atoi(optarg); continue;
            default:  return print_usage(stderr, argv[0]), EXIT_FAILURE;
        }
    }
    
    if (!v1_code) v1_code = V1;
    if (!v2_code) v2_code = V2;

    setbuf(stdin, NULL);
    setbuf(stdout, NULL);

    struct input_event ie;

    int last_key1 = 0;
    int last_key2 = 0;
    int key1_pressed = 0;
    int key2_pressed = 0;
    int act = 0; // 0 = v1_code active, 1 = v2_code active

    while (read_ev(&ie)) {
        // Pass ALL non-key events (like EV_SYN and MSC_SCAN) through immediately
        if (ie.type != EV_KEY) {
            write_ev(&ie);
            continue;
        }
        
        // Pass the physical key event through immediately
        write_ev(&ie);

        // Ignore key repeats to keep the state machine clean
        if (ie.value == 2) continue; 

        if (ie.value == 1) { // PRESS EVENT
            if (ie.code == last_key1) {
                key1_pressed = 1;
            } else if (ie.code == last_key2) {
                key2_pressed = 1;
            } else {
                // Dynamic Shift: New key takes over, oldest is forgotten
                last_key1 = last_key2;
                last_key2 = ie.code;
                key1_pressed = key2_pressed;
                key2_pressed = 1;
            }

            if (key1_pressed && key2_pressed) {
                // OVERLAP (1 -> 2 keys): Release active, toggle, press new
                struct input_event rel_ev = { .type = EV_KEY, .code = act ? v2_code : v1_code, .value = 0 };
                write_ev(&rel_ev);

                act = !act;
                struct input_event press_ev = { .type = EV_KEY, .code = act ? v2_code : v1_code, .value = 1 };
                write_ev(&press_ev);
            } else {
                // SINGLE PRESS (0 -> 1 key)
                act = (ie.code == last_key2);
                struct input_event press_ev = { .type = EV_KEY, .code = act ? v2_code : v1_code, .value = 1 };
                write_ev(&press_ev);
            }
            
        } else if (ie.value == 0) { // RELEASE EVENT
            int is_tracked = 0;

            if (ie.code == last_key1) {
                key1_pressed = 0;
                is_tracked = 1;
            } else if (ie.code == last_key2) {
                key2_pressed = 0;
                is_tracked = 1;
            }

            if (is_tracked) {
                if (key1_pressed || key2_pressed) {
                    // OVERLAP RELEASE (2 -> 1 keys): Release active, toggle, press remaining
                    struct input_event rel_ev = { .type = EV_KEY, .code = act ? v2_code : v1_code, .value = 0 };
                    write_ev(&rel_ev);

                    act = !act;
                    struct input_event press_ev = { .type = EV_KEY, .code = act ? v2_code : v1_code, .value = 1 };
                    write_ev(&press_ev);
                } else {
                    // ALL RELEASED (1 -> 0 keys): Release active
                    struct input_event rel_ev = { .type = EV_KEY, .code = act ? v2_code : v1_code, .value = 0 };
                    write_ev(&rel_ev);
                    act = 0; // Reset state
                }
            }
            // Untracked releases do nothing here, because the physical release was already passed through at the top.
        }
    }
    return 0;
}
