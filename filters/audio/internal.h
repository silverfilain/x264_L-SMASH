#ifndef FILTERS_AUDIO_INTERNAL_H_
#define FILTERS_AUDIO_INTERNAL_H_

#include "filters/audio/audio_filters.h"

#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/avutil.h"

#define AUDIO_FILTER_COMMON \
    const audio_filter_t *self; \
    audio_info_t *info; \
    struct audio_hnd_t *next, *prev;

#define INIT_FILTER_STRUCT(structname)                          \
    structname *h;                                              \
    do                                                          \
    {                                                           \
        h = *handle = calloc( 1, sizeof( structname ) );        \
        if( !h )                                                \
            goto fail;                                          \
        h->self = self;                                         \
        if( previous )                                          \
        {                                                       \
            audio_hnd_t *p = previous;                          \
            assert( p->info );                                  \
            h->info = malloc( sizeof( audio_info_t ) );         \
            memcpy( h->info, p->info, sizeof( audio_info_t ) ); \
        }                                                       \
        h->prev = previous;                                     \
    } while( 0 )

typedef struct audio_info_t
{
    char *codec_name;
    int samplerate; //< Sample Rate in Hz
    enum SampleFormat samplefmt; //< Sample Format in SampleFormat
    size_t samplesize; //< How many bytes per sample
    int channels; //< How many channels
    int64_t chanlayout; //< Channel layout (CH_* on avcodec.h)
    int framelen; //< Frame length in samples
    size_t framesize; //< Frame size in bytes
    AVRational time_base;
    uint8_t *extradata;
    int extradata_size;
} audio_info_t;

// Generic audio handle (used to access fields from AUDIO_FILTER_COMMON)
typedef struct audio_hnd_t
{
    AUDIO_FILTER_COMMON
} audio_hnd_t;

static inline audio_hnd_t *af_get_last_filter( audio_hnd_t *chain )
{
    if( !chain )
        return NULL;
    while( chain->next )
        chain = chain->next;
    return chain;
}

void af_register_all( void );
void af_unregister_all( void );
audio_filter_t *af_get_filter_by_id( enum AudioFilter id );

#endif /* FILTERS_AUDIO_INTERNAL_H_ */
