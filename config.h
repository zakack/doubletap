#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>

/* Default virtual key codes */
#define CONFIG_DEFAULT_V1 183  /* KEY_F13 */
#define CONFIG_DEFAULT_V2 184  /* KEY_F14 */
#define CONFIG_DEFAULT_WAV_PATH "/usr/local/share/osu-intercept/click.wav"
#define CONFIG_DEFAULT_PATH "/etc/osu-intercept/config.yaml"

struct config {
	char       *device_path;
	char       *wav_path;
	int        *trigger_keys;
	size_t     trigger_keys_count;
	int        virtual_keys[2];
};

int  config_parse(const char *path, struct config *cfg);
void config_free(struct config *cfg);
int  config_lookup(int argc, char *argv[], struct config *cfg);
int  config_validate_device(const struct config *cfg);

#endif /* CONFIG_H */
