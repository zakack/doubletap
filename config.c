#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <yaml.h>

#include "config.h"

int config_parse(const char *path, struct config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->wav_path = strdup(CONFIG_DEFAULT_WAV_PATH);
    if (!cfg->wav_path) {
        fprintf(stderr, "config error: out of memory\n");
        return -1;
    }
    cfg->virtual_keys[0] = CONFIG_DEFAULT_V1;
    cfg->virtual_keys[1] = CONFIG_DEFAULT_V2;

    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "config error: cannot open %s\n", path);
        return -1;
    }

    yaml_parser_t parser;
    yaml_event_t event;

    if (!yaml_parser_initialize(&parser)) {
        fprintf(stderr, "config error: cannot initialize parser\n");
        fclose(f);
        return -1;
    }

    yaml_parser_set_input_file(&parser, f);

    enum { CTX_ROOT, CTX_AUDIO, CTX_MAPPING } ctx = CTX_ROOT;
    const char *key = NULL;
    int in_seq = 0;

    int *trigger_buf = NULL;
    size_t trigger_cap = 0;
    size_t trigger_n = 0;
    int vk_n = 0;

    if (!yaml_parser_parse(&parser, &event) ||
        event.type != YAML_STREAM_START_EVENT) {
        fprintf(stderr, "config error: expected stream start\n");
        goto err;
    }

    while (1) {
        yaml_event_delete(&event);

        if (!yaml_parser_parse(&parser, &event)) {
            fprintf(stderr, "config error: parse error\n");
            goto err;
        }

        switch (event.type) {
        case YAML_STREAM_END_EVENT:
            goto done;

        case YAML_MAPPING_START_EVENT:
            if (ctx == CTX_ROOT && key) {
                if (strcmp(key, "audio") == 0)
                    ctx = CTX_AUDIO;
                else if (strcmp(key, "mapping") == 0)
                    ctx = CTX_MAPPING;
            }
            key = NULL;
            break;

        case YAML_MAPPING_END_EVENT:
            if (ctx != CTX_ROOT)
                ctx = CTX_ROOT;
            key = NULL;
            break;

        case YAML_SEQUENCE_START_EVENT:
            in_seq = 1;
            if (ctx == CTX_MAPPING && key &&
                strcmp(key, "virtual_keys") == 0)
                vk_n = 0;
            break;

        case YAML_SEQUENCE_END_EVENT:
            in_seq = 0;
            if (ctx == CTX_MAPPING && key &&
                strcmp(key, "virtual_keys") == 0) {
                if (vk_n != 2) {
                    fprintf(stderr,
                            "config error: virtual_keys must have exactly 2 elements\n");
                    goto err;
                }
            }
            break;

        case YAML_SCALAR_EVENT: {
            const char *s = (const char *)event.data.scalar.value;

            if (in_seq) {
                long val = strtol(s, NULL, 10);
                if (ctx == CTX_MAPPING && key &&
                    strcmp(key, "trigger_keys") == 0) {
                    if (trigger_n >= trigger_cap) {
                        size_t nc = trigger_cap == 0 ? 4 : trigger_cap * 2;
                        int *buf = realloc(trigger_buf, nc * sizeof(int));
                        if (!buf) {
                            fprintf(stderr, "config error: out of memory\n");
                            goto err;
                        }
                        trigger_buf = buf;
                        trigger_cap = nc;
                    }
                    trigger_buf[trigger_n++] = (int)val;
                } else if (ctx == CTX_MAPPING && key &&
                           strcmp(key, "virtual_keys") == 0) {
                    cfg->virtual_keys[vk_n++] = (int)val;
                }
            } else if (key) {
                if (ctx == CTX_ROOT && strcmp(key, "device") == 0) {
                    cfg->device_path = strdup(s);
                } else if (ctx == CTX_AUDIO &&
                           strcmp(key, "wav_path") == 0) {
                    free(cfg->wav_path);
                    cfg->wav_path = strdup(s);
                }
                key = NULL;
            } else {
                key = s;
            }
            break;
        }

        default:
            break;
        }
    }

done:
    yaml_event_delete(&event);
    yaml_parser_delete(&parser);
    fclose(f);

    cfg->trigger_keys = trigger_buf;
    cfg->trigger_keys_count = trigger_n;

    if (!cfg->device_path || cfg->device_path[0] == '\0') {
        fprintf(stderr, "config error: device is required\n");
        config_free(cfg);
        return -1;
    }

    return 0;

err:
    yaml_event_delete(&event);
    yaml_parser_delete(&parser);
    fclose(f);
    free(trigger_buf);
    free(cfg->device_path);
    free(cfg->wav_path);
    cfg->device_path = NULL;
    cfg->wav_path = NULL;
    cfg->trigger_keys = NULL;
    cfg->trigger_keys_count = 0;
    return -1;
}

void config_free(struct config *cfg)
{
    free(cfg->device_path);
    free(cfg->wav_path);
    free(cfg->trigger_keys);
    memset(cfg, 0, sizeof(*cfg));
}

int config_validate_device(const struct config *cfg)
{
    if (!cfg || !cfg->device_path)
        return -1;
    if (access(cfg->device_path, R_OK) != 0) {
        fprintf(stderr, "Cannot access device: %s: %s\n",
                cfg->device_path, strerror(errno));
        return -1;
    }
    return 0;
}

int config_lookup(int argc, char *argv[], struct config *cfg)
{
    const char *config_path = CONFIG_DEFAULT_PATH;
    if (argc >= 2)
        config_path = argv[1];

    if (config_parse(config_path, cfg) != 0) {
        fprintf(stderr, "Failed to parse config: %s\n", config_path);
        return -1;
    }

    if (config_validate_device(cfg) != 0) {
        config_free(cfg);
        return -1;
    }

    if (cfg->wav_path && access(cfg->wav_path, R_OK) != 0) {
        fprintf(stderr, "Warning: WAV file not accessible: %s\n",
                cfg->wav_path);
    }

    fprintf(stderr,
            "Config: device=%s, trigger_keys=%zu, v1=%d v2=%d, audio=%s\n",
            cfg->device_path, cfg->trigger_keys_count,
            cfg->virtual_keys[0], cfg->virtual_keys[1], cfg->wav_path);

    return 0;
}
