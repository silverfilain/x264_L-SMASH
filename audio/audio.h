#ifndef AUDIO_AUDIO_H_
#define AUDIO_AUDIO_H_

#include <stdint.h>
#include "x264cli.h"

enum AudioResult
{
    AUDIO_OK    =  1,
    AUDIO_ERROR = -1
};

enum AudioTrack
{
    TRACK_ANY  = -1,
    TRACK_NONE = -2
};

enum AudioFlags
{
    AUDIO_FLAG_NONE = 0,
    AUDIO_FLAG_EOF = 1
};

typedef struct audio_samples_t
{
    uint8_t *data;
    intptr_t len;
    unsigned samplecount;
    enum AudioFlags flags;
    hnd_t owner;
    hnd_t ownerdata;
} audio_samples_t;

#include "filters/audio/audio_filters.h"

hnd_t audio_open_from_file( audio_filter_t *preferred_filter, char *path, int trackno );
int audio_close( hnd_t chain );

#include <audio/encoders.h>

#endif /* AUDIO_AUDIO_H_ */
