#ifndef AUDIO_ENCODERS_H_
#define AUDIO_ENCODERS_H_

#include <audio/audio.h>
#include <filters/audio/audio_filters.h>

typedef struct audio_encoder_t
{
    hnd_t (*init)( hnd_t filter_chain, const char *opts[] );
    audio_samples_t *(*get_next_packet)( hnd_t handle );
    void (*free_packet)( hnd_t handle, audio_samples_t *samples );
    void (*close)( hnd_t handle );
} audio_encoder_t;



#endif
