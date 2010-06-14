#include "filters/audio/internal.h"

#include <assert.h>

static int registered = 0;

audio_info_t *af_get_info( hnd_t handle )
{
    audio_hnd_t *h = af_get_last_filter( handle );
    return h->info;
}

audio_filter_t *af_get_filter( enum AudioFilter filterid )
{
    if( !registered )
    {
        af_register_all();
        registered = 1;
    }

    return af_get_filter_by_id( filterid );
}

enum AudioResult af_add( hnd_t base, audio_filter_t *filter, const char *options )
{
    assert( base );
    assert( filter );

    audio_hnd_t *h = af_get_last_filter( base );

    return filter->init( (void**) &h->next, h, options );
}

enum AudioResult af_get_samples( audio_samples_t *out, hnd_t handle, int64_t first_sample, int64_t last_sample )
{
    audio_hnd_t *last = af_get_last_filter( handle );
    AVPacket *pkt = last->self->get_samples( last, first_sample, last_sample );
    if( !pkt )
        return AUDIO_ERROR;
    out->len         = pkt->size;
    out->samplecount = pkt->size / last->info->samplesize;
    out->data        = pkt->data;
    out->owner       = last;
    out->ownerdata   = pkt;
    out->flags       = AUDIO_FLAG_NONE;
    
    intptr_t expected_len = ( last_sample - first_sample ) * last->info->samplesize;
    if( out->len < expected_len )
        out->flags |= AUDIO_FLAG_EOF;
    
    return AUDIO_OK;
}

void af_free_samples( audio_samples_t *samples )
{
    audio_hnd_t *owner = samples->owner;
    AVPacket *pkt      = samples->ownerdata;
    owner->self->free_packet( owner, pkt );

    samples->owner = samples->ownerdata = samples->data = NULL;
    samples->len = 0;
}

int af_close( hnd_t chain )
{
    audio_hnd_t *last = af_get_last_filter( chain );
    while( last->prev )
    {
        last = last->prev;
        last->next->self->close( last->next );
    }
    last->self->close( last );

    af_unregister_all();
    registered = 0;

    return AUDIO_OK;
}
