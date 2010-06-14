#ifndef FILTERS_AUDIO_AUDIO_FILTERS_H_
#define FILTERS_AUDIO_AUDIO_FILTERS_H_

#include <stdint.h>
#include "filters/common.h"

#define hnd_t void*

typedef struct audio_filter_t
{
    enum AudioResult (*init)( hnd_t *handle, hnd_t previous, const char *opts );
    struct AVPacket *(*get_samples)( hnd_t handle, int64_t first_sample, int64_t last_sample );
    int (*free_packet)( hnd_t handle, struct AVPacket *frame );
    enum AudioResult (*close)( hnd_t handle );
    char *name, *longname, *description, *help;
    void (*help_callback)( int longhelp );
} audio_filter_t;

#include "libavcodec/avcodec.h"

typedef struct audio_info_t
{
    char *codec_name;
    int samplerate; //< Sample Rate in Hz
    enum SampleFormat samplefmt; //< Sample Format in SampleFormat
    size_t samplesize; //< How many bytes per sample
    int channels; //< How many channels
    int64_t chanlayout; //< Channel layout (CH_* on avcodec.h)
    size_t chansize; //< How many bytes per channel
    int framelen; //< Frame length in samples
    size_t framesize; //< Frame size in bytes
    AVRational time_base;
    uint8_t *extradata;
    int extradata_size;
} audio_info_t;

/* NOTE: this enum must be synchronized with audio_internal.c:register_all */
enum AudioFilter
{
    AUDIO_SOURCE_LAVF = 0,
};

#include "audio/audio.h"

audio_info_t *af_get_info( hnd_t handle );
audio_filter_t *af_get_filter( enum AudioFilter filterid );
enum AudioResult af_add( hnd_t base, audio_filter_t *filter, const char *options );
enum AudioResult af_get_samples( audio_samples_t *samples, hnd_t handle, int64_t first_sample, int64_t last_sample );
void af_free_samples( audio_samples_t *samples );
int af_close( hnd_t chain );

#endif /* AUDIO_H_ */
