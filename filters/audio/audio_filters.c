#include "filters/audio/internal.h"

#include <assert.h>

audio_info_t *af_get_info( hnd_t handle )
{
    audio_hnd_t *h = af_get_last_filter( handle );
    return h->info;
}

audio_filter_t *af_get_filter( char *name )
{
#define CHECK( filter, type )                             \
    extern audio_filter_t audio_##type##_##filter;        \
    if ( !strcmp( name, audio_##type##_##filter.name ) ) \
    {                                                     \
        return &audio_##type##_##filter;                  \
    }
#define CHECKFLT( fname ) CHECK( fname, filter )
    CHECK( lavf, source )
    return NULL;
}

int af_add( hnd_t base, audio_filter_t *filter, const char *options )
{
    assert( base );
    assert( filter );

    audio_hnd_t *h = af_get_last_filter( base );

    return filter->init( (void**) &h->next, h, options );
}

audio_packet_t *af_get_samples( hnd_t handle, int64_t first_sample, int64_t last_sample )
{
    audio_hnd_t *last   = af_get_last_filter( handle );
    audio_packet_t *out = last->self->get_samples( last, first_sample, last_sample );
    if( out )
    {
        out->owner       = last;
        out->samplecount = out->size / last->info->samplesize;
        return out;
    }
    return 0;
}

void af_free_packet( audio_packet_t *pkt )
{
    audio_hnd_t *owner = pkt->owner;
    if( owner )
        owner->self->free_packet( owner, pkt );
    else
    {
        if( pkt->priv )
            free( pkt->priv );
        free( pkt->data );
        free( pkt );
    }
}

void af_close( hnd_t chain )
{
    audio_hnd_t *last = af_get_last_filter( chain );
    while( last->prev )
    {
        last = last->prev;
        last->next->self->close( last->next );
    }
    last->self->close( last );
}
