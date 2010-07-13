#include "filters/audio/internal.h"

#include <assert.h>

audio_info_t *af_get_info( hnd_t handle )
{
    return &((audio_hnd_t*)handle)->info;
}

audio_filter_t *af_get_filter( char *name )
{
#define CHECK( filter, type )                             \
    extern audio_filter_t audio_##type##_##filter;        \
    if ( !strcmp( name, audio_##type##_##filter.name ) )  \
        return &audio_##type##_##filter
#define CHECKFLT( fname ) CHECK( fname, filter )
    CHECK( lavf, source );
#undef CHECKFLT
#undef CHECK
    return NULL;
}

audio_packet_t *af_get_samples( hnd_t handle, int64_t first_sample, int64_t last_sample )
{
    audio_hnd_t *h = handle;
    audio_packet_t *out = h->self->get_samples( h, first_sample, last_sample );
    if( out )
    {
        out->owner = h;
        return out;
    }
    return 0;
}

void af_free_packet( audio_packet_t *pkt )
{
    if( !pkt )
        return;
    audio_hnd_t *owner = pkt->owner;
    if( owner )
        owner->self->free_packet( owner, pkt );
    else
    {
        if( pkt->priv )
            free( pkt->priv );
        if( pkt->rawdata )
            free( pkt->rawdata );
        if( pkt->data && pkt->channels )
            af_free_buffer( pkt->data, pkt->channels );
        free( pkt );
    }
}

void af_close( hnd_t chain )
{
    audio_hnd_t *h = chain;
    if( h->prev )
        af_close( h->prev );
    h->self->close( h );
}
