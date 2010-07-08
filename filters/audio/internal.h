#ifndef FILTERS_AUDIO_INTERNAL_H_
#define FILTERS_AUDIO_INTERNAL_H_

#include "filters/audio/audio_filters.h"

#define AUDIO_FILTER_COMMON \
    const audio_filter_t *self; \
    audio_info_t *info; \
    struct audio_hnd_t *next, *prev;

#define INIT_FILTER_STRUCT(filterstruct, structname)            \
    structname *h;                                              \
    do                                                          \
    {                                                           \
        h = *handle = calloc( 1, sizeof( structname ) );        \
        if( !h )                                                \
            goto fail;                                          \
        h->self = &filterstruct;                                \
        if( previous )                                          \
        {                                                       \
            audio_hnd_t *p = previous;                          \
            p->next = (audio_hnd_t*) h;                         \
            assert( p->info );                                  \
            h->info = malloc( sizeof( audio_info_t ) );         \
            memcpy( h->info, p->info, sizeof( audio_info_t ) ); \
        }                                                       \
        h->prev = previous;                                     \
    } while( 0 )

// Generic audio handle (used to access fields from AUDIO_FILTER_COMMON)
typedef struct audio_hnd_t
{
    AUDIO_FILTER_COMMON
} audio_hnd_t;

#define AF_LOG( handle, level, ... ) do { x264_cli_log( ((audio_hnd_t*)handle)->self->name, (level), __VA_ARGS__ ); } while (0)
#define AF_LOG_ERR( handle, ... ) AF_LOG( (handle), X264_LOG_ERROR, __VA_ARGS__ )
#define AF_LOG_WARN( handle, ... ) AF_LOG( (handle), X264_LOG_WARNING, __VA_ARGS__ )

static inline audio_hnd_t *af_get_last_filter( audio_hnd_t *chain )
{
    if( !chain )
        return NULL;
    while( chain->next )
        chain = chain->next;
    return chain;
}

#endif /* FILTERS_AUDIO_INTERNAL_H_ */
