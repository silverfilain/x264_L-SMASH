#ifndef AUDIO_MUXERS_H_
#define AUDIO_MUXERS_H_

#include "audio/audio.h"
#include "filters/audio/audio_filters.h"

#include <stdint.h>

typedef struct audio_muxer_t
{
    hnd_t *(*init)( const struct audio_muxer_t *self, hnd_t filter_chain, const char *opts );
    int (*write_audio)( hnd_t handle, int64_t maxpts );
    enum AudioResult (*close)( hnd_t handle );
} audio_muxer_t;

#endif
