#include "audio/audio.h"
#include "audio/audio_internal.h"

#include <assert.h>

int registered = 0;

audio_filter_t *audio_get_filter( enum AudioFilter filterid )
{
    if( !registered )
    {
        register_all();
        registered = 1;
    }

    return get_filter_by_id( filterid );
}

hnd_t audio_open_from_file( audio_filter_t *preferred_filter, char *path, int trackno )
{
    audio_filter_t *source = preferred_filter ? preferred_filter : audio_get_filter( AUDIO_SOURCE_LAVF );
    hnd_t h;
    size_t init_arg_size = sizeof( path ) + 10;
    char *init_arg = malloc( init_arg_size );
    assert( snprintf( init_arg, init_arg_size, "%s:%d", path, trackno ) < init_arg_size );
    if( source->init( source, NULL, &h, init_arg ) != AUDIO_OK || !h )
    {
        fprintf( stderr, "audio [error]: error initializing source filter!" );
        return NULL;
    }
    free( init_arg );
    return h;
}

enum AudioResult audio_add_filter( hnd_t base, audio_filter_t *filter, const char *options )
{
    assert( base );
    assert( filter );
    
    audio_hnd_t *h = get_last_filter( base );

    return filter->init( filter, h, (void**) &h->next, options );
}

enum AudioResult audio_filter_samples( audio_samples_t *out, hnd_t chain, int64_t first_sample, int64_t last_sample )
{
    audio_hnd_t *last = get_last_filter( chain );
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

void audio_free_samples( audio_samples_t *samples )
{
    audio_hnd_t *owner = samples->owner;
    AVPacket *pkt      = samples->ownerdata;
    owner->self->free_packet( owner, pkt );

    samples->owner = samples->ownerdata = samples->data = NULL;
    samples->len = 0;
}

int audio_close( hnd_t chain )
{
    audio_hnd_t *last = get_last_filter( chain );
    while( last->prev )
    {
        last = last->prev;
        last->next->self->close( last->next );
    }
    last->self->close( last );

    unregister_all();
    registered = 0;

    return AUDIO_OK;
}
