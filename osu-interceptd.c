/*
 * osu-interceptd - Rapid-fire virtual keyboard daemon
 *
 * The daemon directly opens (and exclusively grabs) one or more evdev
 * keyboard devices, applies a "last-input + reverting toggle" SOCD state
 * machine to two configurable physical keys (k1, k2 -> v1, v2), mirrors
 * every other event verbatim into a single uinput virtual keyboard, and
 * plays a click sound through PipeWire on every virtual key-press.
 *
 * Copyright 2026 Zachary Kessler
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <sys/epoll.h>
#include <sys/types.h>

#include <linux/input.h>

#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>

#include <yaml.h>

#include <spa/param/audio/format-utils.h>
#include <pipewire/pipewire.h>

/* ------------------------------------------------------------------------- */
/* Defaults                                                                  */
/* ------------------------------------------------------------------------- */

#define DEF_K1     KEY_Z
#define DEF_K2     KEY_X
#define DEF_V1     KEY_F13
#define DEF_V2     KEY_F14
#define DEF_NAME   "osu-intercept virtual keyboard"
#define DEF_WAV    "/usr/share/osu-intercept/click.wav"
#define DEF_CONFIG "/usr/share/osu-intercept/config.yaml"

/* ------------------------------------------------------------------------- */
/* Logging                                                                   */
/* ------------------------------------------------------------------------- */

static void logf_(const char *level, const char *fmt, ...)
__attribute__((format(printf, 2, 3)));

static void logf_(const char *level, const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "[osu-interceptd] %s: ", level);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

#define LOG_INFO(...) logf_("info",  __VA_ARGS__)
#define LOG_WARN(...) logf_("warn",  __VA_ARGS__)
#define LOG_ERR(...)  logf_("error", __VA_ARGS__)

/* ------------------------------------------------------------------------- */
/* Audio (PipeWire)                                                          */
/* ------------------------------------------------------------------------- */

static int audio_available;

static struct {
    float                    *samples;
    size_t                    num_frames;
    int                       channels;
    int                       sample_rate;

    struct pw_thread_loop    *loop;
    struct pw_stream         *stream;

    atomic_int                pending;
    atomic_bool               playing;
    atomic_bool               reset;
    atomic_size_t             frame_pos;
} audio;

typedef struct { char id[4]; uint32_t size; } wav_chunk;
typedef struct {
    uint16_t fmt;
    uint16_t ch;
    uint32_t rate;
    uint32_t br;
    uint16_t ba;
    uint16_t bps;
} wav_fmt;

static int wav_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        LOG_WARN("Cannot open WAV %s: %s", path, strerror(errno));
        return -1;
    }

    char riff[4]; uint32_t rsize; char wave[4];
    if (fread(riff, 4, 1, f) != 1 || fread(&rsize, 4, 1, f) != 1 ||
        fread(wave, 4, 1, f) != 1 ||
        memcmp(riff, "RIFF", 4) || memcmp(wave, "WAVE", 4)) {
        fclose(f);
        LOG_WARN("Not a RIFF/WAVE file: %s", path);
        return -1;
    }

    wav_fmt fmt = {0};
    uint32_t ds = 0;

    for (;;) {
        wav_chunk ch;
        if (fread(&ch, sizeof(ch), 1, f) != 1) break;
        if (!memcmp(ch.id, "fmt ", 4)) {
            size_t rs = ch.size < sizeof(fmt) ? ch.size : sizeof(fmt);
            if (fread(&fmt, rs, 1, f) != 1) { fclose(f); return -1; }
            if (ch.size > sizeof(fmt))
                fseek(f, (long)(ch.size - sizeof(fmt)), SEEK_CUR);
        } else if (!memcmp(ch.id, "data", 4)) {
            ds = ch.size; break;
        } else {
            fseek(f, (long)ch.size, SEEK_CUR);
        }
    }

    if (fmt.fmt != 1 || ds == 0 || fmt.bps == 0 || fmt.ch == 0) {
        fclose(f);
        LOG_WARN("Unsupported WAV format in %s", path);
        return -1;
    }

    audio.channels    = fmt.ch;
    audio.sample_rate = (int)fmt.rate;
    audio.num_frames  = ds / (fmt.bps / 8u) / fmt.ch;
    audio.samples     = calloc(audio.num_frames * audio.channels, sizeof(float));
    if (!audio.samples) { fclose(f); return -1; }

    {
        uint8_t *raw = malloc(ds);
        if (!raw || fread(raw, ds, 1, f) != 1) {
            free(raw); fclose(f);
            free(audio.samples); audio.samples = NULL;
            return -1;
        }
        fclose(f);

        size_t total = audio.num_frames * audio.channels;
        switch (fmt.bps) {
            case 16:
            for (size_t i = 0; i < total; i++)
                audio.samples[i] = ((int16_t *)raw)[i] / 32768.0f;
            break;
            case 24:
            for (size_t i = 0; i < total; i++) {
                int32_t s = (int32_t)(raw[i*3] | ((uint32_t)raw[i*3+1] << 8) |
                                     ((int32_t)((int8_t)raw[i*3+2]) << 16));
                audio.samples[i] = s / 8388608.0f;
            }
            break;
            case 32:
            for (size_t i = 0; i < total; i++)
                audio.samples[i] = ((int32_t *)raw)[i] / 2147483648.0f;
            break;
            default:
            free(raw);
            free(audio.samples); audio.samples = NULL;
            return -1;
        }
        free(raw);
    }

    LOG_INFO("Loaded %s: %zu frames, %d ch, %d Hz",
             path, audio.num_frames, audio.channels, audio.sample_rate);
    return 0;
}

static void on_process(void *userdata) {
    (void)userdata;
    struct pw_buffer *b;
    struct spa_buffer *buf;

    if ((b = pw_stream_dequeue_buffer(audio.stream)) == NULL) {
        pw_log_warn("out of buffers: %m");
        return;
    }

    buf = b->buffer;
    float *dst = buf->datas[0].data;
    if (!dst) return;

    int stride = (int)(sizeof(float) * audio.channels);
    int n_frames = buf->datas[0].maxsize / stride;
    if (b->requested)
        n_frames = SPA_MIN((int)b->requested, n_frames);
    size_t nf = (size_t)n_frames;

    if (!atomic_load_explicit(&audio.playing, memory_order_acquire) &&
        atomic_load(&audio.pending) > 0) {
        atomic_store_explicit(&audio.playing, true, memory_order_relaxed);
        atomic_store(&audio.frame_pos, 0);
        atomic_fetch_sub(&audio.pending, 1);
    }

    if (atomic_load_explicit(&audio.playing, memory_order_acquire)) {
        if (atomic_exchange(&audio.reset, false))
            atomic_store(&audio.frame_pos, 0);

        size_t pos = atomic_load(&audio.frame_pos);
        size_t rem = audio.num_frames - pos;
        size_t tc  = nf < rem ? nf : rem;

        if (tc > 0)
            memcpy(dst, audio.samples + pos * audio.channels, tc * (size_t)stride);
        if (nf > tc)
            memset(dst + tc * audio.channels, 0, (nf - tc) * (size_t)stride);

        pos += tc;
        if (pos >= audio.num_frames) {
            atomic_store(&audio.playing, false);
            atomic_store(&audio.frame_pos, 0);
            if (atomic_load(&audio.pending) > 0) {
                atomic_store(&audio.playing, true);
                atomic_store(&audio.frame_pos, 0);
                atomic_fetch_sub(&audio.pending, 1);
            }
        } else if (atomic_exchange(&audio.reset, false)) {
            atomic_store(&audio.frame_pos, 0);
        } else {
            atomic_store(&audio.frame_pos, pos);
        }
    } else {
        memset(dst, 0, nf * (size_t)stride);
    }

    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = stride;
    buf->datas[0].chunk->size   = nf * (size_t)stride;

    pw_stream_queue_buffer(audio.stream, b);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = on_process,
};

static int audio_init(void) {
    const struct spa_pod *params[1];
    uint8_t podbuf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(podbuf, sizeof(podbuf));

    pw_init(NULL, NULL);

    audio.loop = pw_thread_loop_new("osu-audio", NULL);
    if (!audio.loop) { pw_deinit(); return -1; }

    pw_thread_loop_lock(audio.loop);

    struct pw_loop *pl = pw_thread_loop_get_loop(audio.loop);
    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE,     "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE,     "Game",
        NULL);

    audio.stream = pw_stream_new_simple(pl, "osu-intercept", props,
                                        &stream_events, NULL);

    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
        &SPA_AUDIO_INFO_RAW_INIT(
            .format   = SPA_AUDIO_FORMAT_F32,
            .channels = audio.channels,
            .rate     = audio.sample_rate));

    pw_stream_connect(audio.stream,
                      PW_DIRECTION_OUTPUT,
                      PW_ID_ANY,
                      PW_STREAM_FLAG_AUTOCONNECT |
                      PW_STREAM_FLAG_MAP_BUFFERS  |
                      PW_STREAM_FLAG_RT_PROCESS,
                      params, 1);

    pw_thread_loop_unlock(audio.loop);

    if (pw_thread_loop_start(audio.loop) < 0) {
        pw_thread_loop_lock(audio.loop);
        pw_stream_destroy(audio.stream);
        pw_thread_loop_unlock(audio.loop);
        pw_thread_loop_destroy(audio.loop);
        audio.loop = NULL;
        pw_deinit();
        return -1;
    }

    return 0;
}

static void audio_trigger(void) {
    if (!audio_available) return;
    if (atomic_load(&audio.playing))
        atomic_store(&audio.reset, true);
    else
        atomic_fetch_add(&audio.pending, 1);
}

static void audio_cleanup(void) {
    if (!audio_available) return;
    if (audio.loop) {
        pw_thread_loop_stop(audio.loop);
        pw_thread_loop_lock(audio.loop);
        if (audio.stream) {
            pw_stream_destroy(audio.stream);
            audio.stream = NULL;
        }
        pw_thread_loop_unlock(audio.loop);
        pw_thread_loop_destroy(audio.loop);
        audio.loop = NULL;
    }
    pw_deinit();
    free(audio.samples);
    audio.samples = NULL;
    audio_available = 0;
}

/* ------------------------------------------------------------------------- */
/* Config                                                                    */
/* ------------------------------------------------------------------------- */

typedef struct {
    char   **device_paths;
    size_t   n_devices;
    int      k1, k2, v1, v2;
    int      audio_enabled;
    char    *wav_path;
    char    *uinput_name;
} oid_config_t;

static void config_init(oid_config_t *c) {
    memset(c, 0, sizeof(*c));
    c->k1 = DEF_K1;
    c->k2 = DEF_K2;
    c->v1 = DEF_V1;
    c->v2 = DEF_V2;
    c->audio_enabled = 1;
}

static void config_free(oid_config_t *c) {
    if (!c) return;
    for (size_t i = 0; i < c->n_devices; i++)
        free(c->device_paths[i]);
    free(c->device_paths);
    free(c->wav_path);
    free(c->uinput_name);
    memset(c, 0, sizeof(*c));
}

/* libyaml document-API helpers --------------------------------------------- */

static yaml_node_t* ynode(yaml_document_t *doc, int id) {
    return id ? yaml_document_get_node(doc, id) : NULL;
}

/* Look up a scalar key in a mapping node. Returns the value node or NULL. */
static yaml_node_t* map_get(yaml_document_t *doc, yaml_node_t *map, const char *key) {
    if (!map || map->type != YAML_MAPPING_NODE) return NULL;
    for (yaml_node_pair_t *p = map->data.mapping.pairs.start;
         p < map->data.mapping.pairs.top; p++) {
        yaml_node_t *k = ynode(doc, p->key);
        if (!k || k->type != YAML_SCALAR_NODE) continue;
        if (strcmp((const char *)k->data.scalar.value, key) == 0)
            return ynode(doc, p->value);
    }
    return NULL;
}

static int scalar_dup(yaml_node_t *n, char **out) {
    if (!n || n->type != YAML_SCALAR_NODE) return -1;
    char *s = strdup((const char *)n->data.scalar.value);
    if (!s) return -1;
    free(*out);
    *out = s;
    return 0;
}

static int parse_bool(yaml_node_t *n, int *out) {
    if (!n || n->type != YAML_SCALAR_NODE) return -1;
    const char *s = (const char *)n->data.scalar.value;
    if (!strcasecmp(s, "true")  || !strcasecmp(s, "yes") ||
        !strcasecmp(s, "on")    || !strcmp(s, "1"))    { *out = 1; return 0; }
    if (!strcasecmp(s, "false") || !strcasecmp(s, "no") ||
        !strcasecmp(s, "off")   || !strcmp(s, "0"))    { *out = 0; return 0; }
    return -1;
}

/* Resolve a key code from a scalar - either a symbolic name ("KEY_Z") or
 * a decimal/hex integer. Returns 0 on success. */
static int parse_key_code(yaml_node_t *n, int *out) {
    if (!n || n->type != YAML_SCALAR_NODE) return -1;
    const char *s = (const char *)n->data.scalar.value;

    /* Try numeric first. */
    if (s[0] != '\0') {
        char *end = NULL;
        long v = strtol(s, &end, 0);
        if (end && *end == '\0' && end != s &&
            v >= 0 && v < KEY_MAX) {
            *out = (int)v;
            return 0;
        }
    }

    int code = libevdev_event_code_from_name(EV_KEY, s);
    if (code < 0) return -1;
    *out = code;
    return 0;
}

static int load_config(const char *path, oid_config_t *c) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        LOG_ERR("Cannot open config %s: %s", path, strerror(errno));
        return -1;
    }

    yaml_parser_t parser;
    yaml_document_t doc;
    memset(&doc, 0, sizeof(doc));
    int rc = -1;
    int doc_loaded = 0;

    if (!yaml_parser_initialize(&parser)) {
        LOG_ERR("yaml_parser_initialize failed");
        fclose(fp);
        return -1;
    }
    yaml_parser_set_input_file(&parser, fp);

    if (!yaml_parser_load(&parser, &doc)) {
        LOG_ERR("YAML parse error in %s: %s (line %zu, col %zu)",
                path, parser.problem ? parser.problem : "unknown",
                (size_t)parser.problem_mark.line + 1,
                (size_t)parser.problem_mark.column + 1);
        goto out;
    }
    doc_loaded = 1;

    yaml_node_t *root = yaml_document_get_root_node(&doc);
    if (!root) {
        LOG_ERR("config %s is empty", path);
        goto out;
    }
    if (root->type != YAML_MAPPING_NODE) {
        LOG_ERR("config root must be a mapping");
        goto out;
    }

    /* devices: REQUIRED sequence of scalar paths --------------------- */
    yaml_node_t *devs = map_get(&doc, root, "devices");
    if (!devs) {
        LOG_ERR("config is missing 'devices' (a list of /dev/input paths)");
        goto out;
    }
    if (devs->type != YAML_SEQUENCE_NODE) {
        LOG_ERR("'devices' must be a sequence");
        goto out;
    }
    {
        size_t n = (size_t)(devs->data.sequence.items.top -
                            devs->data.sequence.items.start);
        if (n == 0) {
            LOG_ERR("'devices' list is empty");
            goto out;
        }
        c->device_paths = calloc(n, sizeof(char *));
        if (!c->device_paths) { LOG_ERR("oom"); goto out; }
        for (yaml_node_item_t *it = devs->data.sequence.items.start;
             it < devs->data.sequence.items.top; it++) {
            yaml_node_t *item = ynode(&doc, *it);
            if (scalar_dup(item, &c->device_paths[c->n_devices]) != 0) {
                LOG_ERR("'devices' entry must be a scalar string");
                goto out;
            }
            c->n_devices++;
        }
    }

    /* keys: optional mapping*/
    yaml_node_t *keys = map_get(&doc, root, "keys");
    if (keys) {
        if (keys->type != YAML_MAPPING_NODE) {
            LOG_ERR("'keys' must be a mapping");
            goto out;
        }
        struct { const char *name; int *out; } kmap[] = {
            { "k1", &c->k1 }, { "k2", &c->k2 },
            { "v1", &c->v1 }, { "v2", &c->v2 },
        };
        for (size_t i = 0; i < sizeof(kmap)/sizeof(kmap[0]); i++) {
            yaml_node_t *kn = map_get(&doc, keys, kmap[i].name);
            if (kn && parse_key_code(kn, kmap[i].out) != 0) {
                LOG_ERR("invalid key code for 'keys.%s'", kmap[i].name);
                goto out;
            }
        }
    }

    /* audio: optional mapping */
    yaml_node_t *aud = map_get(&doc, root, "audio");
    if (aud) {
        if (aud->type != YAML_MAPPING_NODE) {
            LOG_ERR("'audio' must be a mapping");
            goto out;
        }
        yaml_node_t *en = map_get(&doc, aud, "enabled");
        if (en && parse_bool(en, &c->audio_enabled) != 0) {
            LOG_ERR("'audio.enabled' must be a boolean");
            goto out;
        }
        yaml_node_t *wav = map_get(&doc, aud, "wav");
        if (wav && scalar_dup(wav, &c->wav_path) != 0) {
            LOG_ERR("'audio.wav' must be a scalar string");
            goto out;
        }
    }
    if (c->audio_enabled && !c->wav_path) {
        c->wav_path = strdup(DEF_WAV);
        if (!c->wav_path) { LOG_ERR("oom"); goto out; }
    }

    /* uinput: optional mapping */
    yaml_node_t *ui = map_get(&doc, root, "uinput");
    if (ui) {
        if (ui->type != YAML_MAPPING_NODE) {
            LOG_ERR("'uinput' must be a mapping");
            goto out;
        }
        yaml_node_t *nm = map_get(&doc, ui, "name");
        if (nm && scalar_dup(nm, &c->uinput_name) != 0) {
            LOG_ERR("'uinput.name' must be a scalar string");
            goto out;
        }
    }
    if (!c->uinput_name) {
        c->uinput_name = strdup(DEF_NAME);
        if (!c->uinput_name) { LOG_ERR("oom"); goto out; }
    }

    rc = 0;

out:
    if (doc_loaded) yaml_document_delete(&doc);
    yaml_parser_delete(&parser);
    fclose(fp);
    if (rc != 0) config_free(c);
    return rc;
}

/* ------------------------------------------------------------------------- */
/* Input devices                                                             */
/* ------------------------------------------------------------------------- */

typedef struct {
    struct libevdev  *dev;
    int               fd;
    char             *path;
    int               k1;   /* k1 down? */
    int               k2;   /* k2  down? */
    int               act;  /* 1 == v1 active, 0 == v2 (or none) */
    int               last_was_k1;
} input_dev_t;

static int input_open(const char *path, input_dev_t *out) {
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        LOG_ERR("open(%s): %s", path, strerror(errno));
        return -1;
    }

    struct libevdev *dev = NULL;
    int rc = libevdev_new_from_fd(fd, &dev);
    if (rc < 0) {
        LOG_ERR("libevdev_new_from_fd(%s): %s", path, strerror(-rc));
        close(fd);
        return -1;
    }

    rc = libevdev_grab(dev, LIBEVDEV_GRAB);
    if (rc < 0) {
        LOG_ERR("libevdev_grab(%s): %s", path, strerror(-rc));
        libevdev_free(dev);
        close(fd);
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->fd          = fd;
    out->dev         = dev;
    out->path        = strdup(path);
    LOG_INFO("Opened and grabbed %s (\"%s\")",
             path, libevdev_get_name(dev) ? libevdev_get_name(dev) : "?");
    return 0;
}

static void input_close(input_dev_t *in) {
    if (!in) return;
    if (in->dev) {
        libevdev_grab(in->dev, LIBEVDEV_UNGRAB);
        libevdev_free(in->dev);
        in->dev = NULL;
    }
    if (in->fd >= 0) {
        close(in->fd);
        in->fd = -1;
    }
    free(in->path);
    in->path = NULL;
}

/* ------------------------------------------------------------------------- */
/* Virtual uinput device                                                     */
/* ------------------------------------------------------------------------- */

static struct libevdev         *vdev  = NULL;
static struct libevdev_uinput  *uidev = NULL;

/* virtual keyboard with keys  of every source
 * devices codes, plus configured v1/v2. EV_SYN always
 * enabled. Not cloning EV_REL,EV_ABS,etx */
static int build_virtual(input_dev_t *inputs, size_t n, const oid_config_t *cfg) {
    vdev = libevdev_new();
    if (!vdev) {
        LOG_ERR("libevdev_new failed");
        return -1;
    }
    libevdev_set_name(vdev, cfg->uinput_name);
    libevdev_set_uniq(vdev, "osu-intercept");
    libevdev_set_id_bustype(vdev, BUS_VIRTUAL);
    libevdev_set_id_vendor (vdev, 0x0001);
    libevdev_set_id_product(vdev, 0x0001);
    libevdev_set_id_version(vdev, 0x0001);

    libevdev_enable_event_type(vdev, EV_SYN);
    libevdev_enable_event_code(vdev, EV_SYN, SYN_REPORT,    NULL);
    libevdev_enable_event_code(vdev, EV_SYN, SYN_MT_REPORT, NULL);

    libevdev_enable_event_type(vdev, EV_KEY);
    for (size_t i = 0; i < n; i++) {
        for (unsigned int code = 0; code < KEY_MAX; code++) {
            if (libevdev_has_event_code(inputs[i].dev, EV_KEY, code))
                libevdev_enable_event_code(vdev, EV_KEY, code, NULL);
        }
    }
    libevdev_enable_event_code(vdev, EV_KEY, cfg->v1, NULL);
    libevdev_enable_event_code(vdev, EV_KEY, cfg->v2, NULL);

    int rc = libevdev_uinput_create_from_device(
        vdev, LIBEVDEV_UINPUT_OPEN_MANAGED, &uidev);
    if (rc < 0) {
        LOG_ERR("libevdev_uinput_create_from_device: %s", strerror(-rc));
        libevdev_free(vdev);
        vdev = NULL;
        return -1;
    }

    const char *node = libevdev_uinput_get_devnode(uidev);
    LOG_INFO("Created virtual keyboard \"%s\" at %s",
             cfg->uinput_name, node ? node : "(unknown)");
    return 0;
}

static void destroy_virtual(void) {
    if (uidev) { libevdev_uinput_destroy(uidev); uidev = NULL; }
    if (vdev)  { libevdev_free(vdev);            vdev  = NULL; }
}

/* ------------------------------------------------------------------------- */
/* Radio button state machine                                                */
/* ------------------------------------------------------------------------- */

/*
 * Returns 1 if a virtual key-down was emitted (so the caller can trigger
 * audio), 0 otherwise.
 *
 *   state = k1 + k2 + ie.value   (k1, k2 post-update)
 *
 *     0 = NONE     last key released
 *     1 = RELEASE  two keys held -> one released
 *     2 = SINGLE   first key pressed (zero -> one)
 *     3 = PRESS    one key held -> second pressed (one -> two)
 */
enum { S_NONE = 0, S_RELEASE = 1, S_SINGLE = 2, S_PRESS = 3 };

static int process_event(input_dev_t *in, const struct input_event *ie, const oid_config_t *cfg) {
    if (ie->type == EV_MSC && ie->code == MSC_SCAN)
        return 0;

    if (ie->type != EV_KEY ||
        (ie->code != (unsigned)cfg->k1 && ie->code != (unsigned)cfg->k2)) {
        libevdev_uinput_write_event(uidev, ie->type, ie->code, ie->value);
        return 0;
    }

    if (ie->value == 2) /* autorepeat messes up state*/
        return 0;

    int is_k1 = (ie->code == (unsigned)cfg->k1);
    if (is_k1) in->k1 = ie->value;
    else       in->k2 = ie->value;

    int state = in->k1 + in->k2 + ie->value;
    int triggered = 0;

    switch (state) {
        case S_SINGLE:
        int code = is_k1 ? cfg->v1 : cfg->v2;
        int up_code; int down_code;
        in->act = is_k1;
        in->last_was_k1 = is_k1;
        libevdev_uinput_write_event(uidev, EV_KEY, code, 1);
        libevdev_uinput_write_event(uidev, EV_SYN, SYN_REPORT, 0);
        triggered = 1;
        break;
        case S_RELEASE:
        case S_PRESS:
        up_code   = in->act ? cfg->v1 : cfg->v2;
        down_code = in->act ? cfg->v2 : cfg->v1;
        libevdev_uinput_write_event(uidev, EV_KEY, up_code, 0);
        libevdev_uinput_write_event(uidev, EV_SYN, SYN_REPORT, 0);
        libevdev_uinput_write_event(uidev, EV_KEY, down_code, 1);
        libevdev_uinput_write_event(uidev, EV_SYN, SYN_REPORT, 0);
        in->act = !in->act;
        triggered = 1;
        break;
        case S_NONE:
        up_code = in->act ? cfg->v1 : cfg->v2;
        libevdev_uinput_write_event(uidev, EV_KEY, up_code, 0);
        libevdev_uinput_write_event(uidev, EV_SYN, SYN_REPORT, 0);
        in->act = 0;
        break;
        default:
        /* shouldn't make it here w/ sane inputs. */
        break;
    }

    return triggered;
}

/* ------------------------------------------------------------------------- */
/* loop                                                                      */
/* ------------------------------------------------------------------------- */

static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

/* drain everything libevdev has buffered, looping over SYNC drops
 * as needed. return 0 on normal EAGAIN, -1 on fatal error. */
static int drain_device(input_dev_t *in, const oid_config_t *cfg) {
    unsigned int flag = LIBEVDEV_READ_FLAG_NORMAL;
    for (;;) {
        struct input_event ie;
        int rc = libevdev_next_event(in->dev, flag, &ie);
        if (rc == -EAGAIN)
            return 0;
        if (rc == LIBEVDEV_READ_STATUS_SYNC) {
            flag = LIBEVDEV_READ_FLAG_SYNC;
            continue;
        }
        if (rc != LIBEVDEV_READ_STATUS_SUCCESS) {
            LOG_WARN("libevdev_next_event(%s): %s",
                     in->path, strerror(-rc));
            return -1;
        }
        flag = LIBEVDEV_READ_FLAG_NORMAL;
        if (process_event(in, &ie, cfg))
            audio_trigger();
    }
}

static int run_loop(input_dev_t *inputs, size_t n_inputs, const oid_config_t *cfg) {
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        LOG_ERR("epoll_create1: %s", strerror(errno));
        return -1;
    }

    for (size_t i = 0; i < n_inputs; i++) {
        struct epoll_event ev = {
            .events  = EPOLLIN,
            .data    = { .u64 = (uint64_t)i },
        };
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, inputs[i].fd, &ev) < 0) {
            LOG_ERR("epoll_ctl ADD %s: %s",
                    inputs[i].path, strerror(errno));
            close(epfd);
            return -1;
        }
    }

    struct epoll_event events[16];
    size_t alive = n_inputs;

    while (g_running && alive > 0) {
        int nfd = epoll_wait(epfd, events, 16, -1);
        if (nfd < 0) {
            if (errno == EINTR) continue;
            LOG_ERR("epoll_wait: %s", strerror(errno));
            break;
        }
        for (int i = 0; i < nfd; i++) {
            size_t idx = (size_t)events[i].data.u64;
            input_dev_t *in = &inputs[idx];

            if (!in->dev) continue; /* already closed */

            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                LOG_WARN("device %s gone (EPOLLERR/HUP), removing",
                         in->path);
                epoll_ctl(epfd, EPOLL_CTL_DEL, in->fd, NULL);
                input_close(in);
                alive--;
                continue;
            }

            if (drain_device(in, cfg) < 0) {
                LOG_WARN("dropping %s", in->path);
                epoll_ctl(epfd, EPOLL_CTL_DEL, in->fd, NULL);
                input_close(in);
                alive--;
            }
        }
    }

    close(epfd);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* main                                                                      */
/* ------------------------------------------------------------------------- */

static void print_usage(FILE *s, const char *prog) {
    fprintf(s,
        "osu-interceptd - SOCD-cleaning input daemon\n"
        "\n"
        "Grabs evdev keyboard devices, applies a last-input + reverting-toggle\n"
        "radio button filter to two configurable keys, and re-emits everything via a\n"
        "single uinput virtual keyboard. Plays a click on each virtual keypress.\n"
        "\n"
        "usage: %s [-h] [-c CONFIG]\n"
        "\n"
        "options:\n"
        "    -h          show this help and exit\n"
        "    -c CONFIG   path to YAML config (default: %s)\n",
        prog, DEF_CONFIG);
}

int
main(int argc, char **argv) {
    const char *config_path = DEF_CONFIG;

    for (int opt; (opt = getopt(argc, argv, "hc:")) != -1; ) {
        switch (opt) {
            case 'h':
            print_usage(stdout, argv[0]);
            return EXIT_SUCCESS;
            case 'c':
            config_path = optarg;
            break;
            default:
            print_usage(stderr, argv[0]);
            return EXIT_FAILURE;
        }
    }

    oid_config_t cfg;
    config_init(&cfg);
    if (load_config(config_path, &cfg) != 0)
        return EXIT_FAILURE;

    const char *k1n = libevdev_event_code_get_name(EV_KEY, cfg.k1);
    const char *k2n = libevdev_event_code_get_name(EV_KEY, cfg.k2);
    const char *v1n = libevdev_event_code_get_name(EV_KEY, cfg.v1);
    const char *v2n = libevdev_event_code_get_name(EV_KEY, cfg.v2);
    LOG_INFO("Config: %zu device(s), keys k1=%s(%d) k2=%s(%d) "
             "v1=%s(%d) v2=%s(%d), audio=%s",
             cfg.n_devices,
             k1n ? k1n : "?", cfg.k1, k2n ? k2n : "?", cfg.k2,
             v1n ? v1n : "?", cfg.v1, v2n ? v2n : "?", cfg.v2,
             cfg.audio_enabled ? "enabled" : "disabled");

    /* best-effort audio init  */
    if (cfg.audio_enabled) {
        if (wav_load(cfg.wav_path) == 0 && audio_init() == 0) {
            audio_available = 1;
        } else {
            LOG_WARN("Audio disabled (WAV load or PipeWire init failed)");
            audio_available = 0;
        }
    }

    /* open and grab every requested input device. */
    input_dev_t *inputs = calloc(cfg.n_devices, sizeof(*inputs));
    if (!inputs) {
        LOG_ERR("oom");
        audio_cleanup();
        config_free(&cfg);
        return EXIT_FAILURE;
    }
    size_t opened = 0;
    for (size_t i = 0; i < cfg.n_devices; i++) {
        if (input_open(cfg.device_paths[i], &inputs[opened]) == 0)
            opened++;
    }
    if (opened == 0) {
        LOG_ERR("Failed to open any input device - aborting");
        free(inputs);
        audio_cleanup();
        config_free(&cfg);
        return EXIT_FAILURE;
    }

    /* single virtual keyboard w/ all sources' keys. */
    if (build_virtual(inputs, opened, &cfg) != 0) {
        for (size_t i = 0; i < opened; i++) input_close(&inputs[i]);
        free(inputs);
        audio_cleanup();
        config_free(&cfg);
        return EXIT_FAILURE;
    }

    /* handlers for graceful shutdown. */
    struct sigaction sa = { .sa_handler = on_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    LOG_INFO("Running.");
    run_loop(inputs, opened, &cfg);
    LOG_INFO("Shutting down");

    destroy_virtual();
    for (size_t i = 0; i < opened; i++) input_close(&inputs[i]);
    free(inputs);
    audio_cleanup();
    config_free(&cfg);
    return EXIT_SUCCESS;
}
