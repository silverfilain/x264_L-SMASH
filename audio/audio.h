#ifndef AUDIO_H_
#define AUDIO_H_

typedef void* hnd_t;

#include <stdint.h>

enum AudioResult
{
    AUDIO_OK = 1,
    AUDIO_ERROR = -1
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

#endif /* AUDIO_H_ */
