#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>
#include <signal.h>
#include <linux/input.h>

#include <spa/param/audio/format-utils.h>
#include <pipewire/pipewire.h>

#define V1 KEY_F13
#define V2 KEY_F14
#define K1 KEY_Z //set the physical keys used
#define K2 KEY_X //to control the virtual keys
#define WAV_PATH "/usr/local/share/osu-intercept/click.wav"

static volatile sig_atomic_t g_running = 1;

static int audio_available;

static struct {
	float   *samples;
	size_t   num_frames;
	int      channels;
	int      sample_rate;

	struct pw_thread_loop *loop;
	struct pw_stream      *stream;

	atomic_int   pending;
	atomic_bool  playing;
	atomic_bool  reset;
	atomic_size_t frame_pos;
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

static int wav_load(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) { fprintf(stderr, "Cannot open %s\n", path); return -1; }

	char riff[4]; uint32_t rsize; char wave[4];
	if (fread(riff, 4, 1, f) != 1 || fread(&rsize, 4, 1, f) != 1 ||
	    fread(wave, 4, 1, f) != 1 ||
	    memcmp(riff, "RIFF", 4) || memcmp(wave, "WAVE", 4))
		{ fclose(f); return -1; }

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

	if (fmt.fmt != 1 || ds == 0 || fmt.bps == 0 || fmt.ch == 0)
		{ fclose(f); return -1; }

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

	fprintf(stderr, "Loaded %s: %zu frames, %d ch, %d Hz\n",
	        path, audio.num_frames, audio.channels, audio.sample_rate);
	return 0;
}

static void on_process(void *userdata)
{
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
	buf->datas[0].chunk->size = nf * (size_t)stride;

	pw_stream_queue_buffer(audio.stream, b);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.process = on_process,
};

static int audio_init(void)
{
	const struct spa_pod *params[1];
	uint8_t podbuf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(podbuf, sizeof(podbuf));

	pw_init(NULL, NULL);

	audio.loop = pw_thread_loop_new("osu-audio", NULL);
	if (!audio.loop) { pw_deinit(); return -1; }

	pw_thread_loop_lock(audio.loop);

	struct pw_loop *pl = pw_thread_loop_get_loop(audio.loop);
	struct pw_properties *props = pw_properties_new(
		PW_KEY_MEDIA_TYPE, "Audio",
		PW_KEY_MEDIA_CATEGORY, "Playback",
		PW_KEY_MEDIA_ROLE, "Game",
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
	                  PW_STREAM_FLAG_MAP_BUFFERS |
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

static void audio_trigger(void)
{
	if (!audio_available) return;
	if (atomic_load(&audio.playing))
		atomic_store(&audio.reset, true);
	else
		atomic_fetch_add(&audio.pending, 1);
}

static void audio_cleanup(void)
{
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
}

static void on_signal(int sig)
{
	(void)sig;
	g_running = 0;
}

void print_usage(FILE *stream, const char *program) {
	fprintf(stream,
	        "osu-intercept-passthrough - dynamic negative edge filter with physical passthrough\n"
	        "\n"
            "usage: %s [-h | -z key1 | -x key2 | -Z virtual1 | -X virtual2]\n"
	        "\n"
            "options:\n"
            "    -h show this message and exit\n"
            "    -z key code for first physical key (default: 'z')\n"
            "    -x key code for second physical key (default: 'x')\n"
            "    -Z key code for first virtual key (default: 'F13')\n"
            "    -X key code for second virtual key (default: 'F14')\n",
	        program);
}

static inline int read_ev(struct input_event *ev) {
	return fread(ev, sizeof(struct input_event), 1, stdin) == 1;
}

static inline void write_ev(const struct input_event *ev) {
	if (fwrite(ev, sizeof(struct input_event), 1, stdout) != 1)
		exit(EXIT_FAILURE);
}

int main(int argc, char *argv[]) {
	  int k1_code = K1;
	  int k2_code = K2;
	  int v1_code = V1;
	  int v2_code = V2;
    for (int opt; (opt = getopt(argc, argv, "hz:x:Z:X:")) != -1;) {
        switch (opt) {
            case 'h':
            return print_usage(stdout, argv[0]), EXIT_SUCCESS;
            case 'z':
            k1_code = atoi(optarg);
            continue;
            case 'x':
            k2_code = atoi(optarg);
            continue;
            case 'Z':
            v1_code = atoi(optarg);
            continue;
            case 'X':
            v2_code = atoi(optarg);
            continue;
        }

        return print_usage(stderr, argv[0]), EXIT_FAILURE;
    }
    if (!k1_code) k1_code = K1;
    if (!k2_code) k2_code = K2;
    if (!v1_code) v1_code = k1_code;
    if (!v2_code) v2_code = k2_code;
    setbuf(stdin, NULL);
    setbuf(stdout, NULL);

	if (wav_load(WAV_PATH) == 0 && audio_init() == 0)
		audio_available = 1;

	struct sigaction sa = { .sa_handler = on_signal };
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

    struct input_event ie;
    struct input_event v1_up = {
          .type = EV_KEY,
          .code = v1_code,
          .value = 0
        };
    struct input_event v2_up = {
          .type = EV_KEY,
          .code = v2_code,
          .value = 0
        };
    struct input_event syn = {
        .type = EV_SYN,
        .code = SYN_REPORT, 
        .value = 0
    };

    enum { NONE, RELEASE, SINGLE, PRESS } state = NONE;
    int k1 = 0; int k2 = 0;
    int act = 0;

    while (g_running && read_ev(&ie)) {
        if (ie.type == EV_MSC && ie.code == MSC_SCAN)
            continue;
        if ((ie.type != EV_KEY) || 
            ((ie.code != k1_code) && (ie.code != k2_code))) {
              write_ev(&ie);
            continue;
        }
        if (ie.value == 2) continue; //2 is key repeat
        if (ie.code == k1_code) k1 = ie.value;
        else k2 = ie.value;
        state = k1 + k2 + ie.value;
        switch (state) {
            case SINGLE: //key pressed alone
            act = (ie.code == k1_code); //k1_code==v1_code press
            ie.code = act ? v1_code : v2_code;
            break;
            case RELEASE: //two keys released to one or
            case PRESS:   //one key pressed to two, toggle time
            write_ev(act ? &v1_up : &v2_up);
            write_ev(&syn); //emit release and flush buffer
            ie.value = 1; //hijack press event
            ie.code = act ? v2_code : v1_code; //map using active flag
            act = !act; //toggle
            break;
            case NONE: //no keys pressed
            ie.code = act ? v1_code : v2_code;
            act = 0;  //reset active flag
        }
            write_ev(&ie); //pass ev to stdout
			if (ie.code) audio_trigger();
    }
	audio_cleanup();
    return 0;
}
