#ifndef FILTERS_AUDIO_AUDIO_FILTERS_H_
#define FILTERS_AUDIO_AUDIO_FILTERS_H_

#include <stdint.h>
#include "x264cli.h"
#include "filters/common.h"

// Ripped from ffmpeg
enum SampleFormats {
    SAMPLEFMT_NONE = -1,
    SAMPLEFMT_U8,
    SAMPLEFMT_S16,
    SAMPLEFMT_S32,
    SAMPLEFMT_FLT,
    SAMPLEFMT_DBL,
    SAMPLEFMT_NB
};

enum AudioFlags
{
    AUDIO_FLAG_NONE = 0,
    AUDIO_FLAG_EOF = 1
};

typedef struct audio_packet_t {
    int64_t         dts;
    uint8_t         *data;
    int             size;
    unsigned        samplecount;
    int64_t         pos;
    enum AudioFlags flags;
    hnd_t           priv;
    hnd_t           owner;
} audio_packet_t;

typedef struct audio_filter_t
{
    int (*init)( hnd_t *handle, hnd_t previous, const char *opts );
    struct audio_packet_t *(*get_samples)( hnd_t handle, int64_t first_sample, int64_t last_sample );
    void (*free_packet)( hnd_t self, struct audio_packet_t *frame );
    void (*close)( hnd_t handle );
    char *name, *longname, *description, *help;
    void (*help_callback)( int longhelp );
} audio_filter_t;

#include "libavcodec/avcodec.h"

typedef struct audio_info_t
{
    char *codec_name;
    int samplerate;              // Sample Rate in Hz
    enum SampleFormat samplefmt; // Sample Format in SampleFormat
    size_t samplesize;           // How many bytes per sample
    int channels;                // How many channels
    int64_t chanlayout;          // Channel layout (CH_* on avcodec.h)
    size_t chansize;             // How many bytes per channel
    int framelen;                // Frame length in samples
    size_t framesize;            // Frame size in bytes
    int64_t time_base_num, time_base_den;
    uint8_t *extradata;
    int extradata_size;
} audio_info_t;

#include "audio/audio.h"

audio_info_t *af_get_info( hnd_t handle );
audio_filter_t *af_get_filter( char *name );
int af_add( hnd_t base, audio_filter_t *filter, const char *options );
audio_packet_t *af_get_samples( hnd_t handle, int64_t first_sample, int64_t last_sample );
void af_free_packet( audio_packet_t *pkt );
void af_close( hnd_t chain );

#endif /* AUDIO_H_ */
