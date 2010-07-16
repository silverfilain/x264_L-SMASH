#ifndef AUDIO_AUDIO_H_
#define AUDIO_AUDIO_H_

#include <stdint.h>
#include "x264cli.h"
#include "filters/audio/audio_filters.h"

enum AudioTrack
{
    TRACK_ANY  = -1,
    TRACK_NONE = -2
};

hnd_t x264_audio_open_from_file( audio_filter_t *preferred_filter, char *path, int trackno );

#include "audio/encoders.h"

#endif /* AUDIO_AUDIO_H_ */
