#ifndef AUDIO_H_
#define AUDIO_H_

typedef void* hnd_t;

#include <stdint.h>

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

typedef struct audio_filter_t
{
    enum AudioResult (*init)( const struct audio_filter_t *self, hnd_t previous, hnd_t *handle, const char *opts );
    struct AVPacket *(*get_samples)( hnd_t handle, int64_t first_sample, int64_t last_sample );
    int (*free_packet)( hnd_t handle, struct AVPacket *frame );
    enum AudioResult (*close)( hnd_t handle );
    char *name, *description, *help;
    void (*help_callback)( int longhelp );
} audio_filter_t;

/* NOTE: this enum must be synchronized with audio_internal.c:register_all */
enum AudioFilter
{
    AUDIO_SOURCE_LAVF = 0
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
    hnd_t owner, ownerdata;
} audio_samples_t;

audio_filter_t *audio_get_filter( enum AudioFilter filterid );
hnd_t audio_open_from_file( audio_filter_t *preferred_filter, char *path, int trackno );
enum AudioResult audio_filter_samples( audio_samples_t *samples, hnd_t chain, int64_t first_sample, int64_t last_sample );
void audio_free_samples( audio_samples_t *samples );
int audio_close( hnd_t chain );

#endif /* AUDIO_H_ */
