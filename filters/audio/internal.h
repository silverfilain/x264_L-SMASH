#ifndef FILTERS_AUDIO_INTERNAL_H_
#define FILTERS_AUDIO_INTERNAL_H_

#include "filters/audio/audio_filters.h"

#define AUDIO_FILTER_COMMON     \
    const audio_filter_t *self; \
    audio_info_t info;          \
    struct audio_hnd_t *prev;

#define INIT_FILTER_STRUCT(filterstruct, structname)            \
    structname *h;                                              \
    do                                                          \
    {                                                           \
        h = calloc( 1, sizeof( structname ) );                  \
        if( !h )                                                \
            goto fail;                                          \
        h->self = &filterstruct;                                \
        h->prev = *handle;                                      \
        if( h->prev )                                           \
            h->info = h->prev->info;                            \
        *handle = h;                                            \
    } while( 0 )

// Generic audio handle (used to access fields from AUDIO_FILTER_COMMON)
typedef struct audio_hnd_t
{
    AUDIO_FILTER_COMMON
} audio_hnd_t;

#define AF_LOG( handle, level, ... ) do { x264_cli_log( ((audio_hnd_t*)handle)->self->name, (level), __VA_ARGS__ ); } while (0)

#define AF_LOG_ERR( handle, ... )  AF_LOG( (handle), X264_LOG_ERROR  , __VA_ARGS__ )
#define AF_LOG_WARN( handle, ... ) AF_LOG( (handle), X264_LOG_WARNING, __VA_ARGS__ )

enum SampleFmt {
    SMPFMT_NONE = -1,
    SMPFMT_U8,
    SMPFMT_S16,
    SMPFMT_S32,
    SMPFMT_FLT,
    SMPFMT_DBL
};

float  **af_get_buffer   ( unsigned channels, unsigned samplecount );
int      af_resize_buffer( float **buffer, unsigned channels, unsigned samplecount );
void     af_free_buffer  ( float **buffer, unsigned channels );
float  **af_dup_buffer   ( float **buffer, unsigned channels, unsigned samplecount );
int      af_cat_buffer   ( float **buf, unsigned bufsamples, float **in, unsigned insamples, unsigned channels );

float  **af_deinterleave ( float *input, unsigned channels, unsigned samplecount );
float   *af_interleave   ( float **input, unsigned channels, unsigned samplecount );

float  **af_deinterleave2( uint8_t *input, enum SampleFmt fmt, unsigned channels, unsigned samplecount );
uint8_t *af_interleave2  ( enum SampleFmt outfmt, float **input, unsigned channels, unsigned samplecount );
uint8_t *af_convert      ( enum SampleFmt outfmt, uint8_t *input, enum SampleFmt fmt, unsigned channels, unsigned samplecount );

#endif /* FILTERS_AUDIO_INTERNAL_H_ */
