#ifndef AUDIO_H
#define AUDIO_H

int audio_init(const char *wav_path);
void audio_trigger(void);
void audio_cleanup(void);

#endif
