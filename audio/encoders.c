#include "audio/encoders.h"

#include <assert.h>
#include <stdlib.h>

struct aenc_t
{
    const audio_encoder_t *enc;
    hnd_t handle;
};

hnd_t audio_encoder_open( const audio_encoder_t *encoder, hnd_t filter_chain, const char *opts )
{
    assert( encoder && filter_chain );
    struct aenc_t *enc = calloc( 1, sizeof( struct aenc_t ) );
    enc->enc = encoder;
    enc->handle = encoder->init( filter_chain, opts );

    return enc;
}

audio_info_t *audio_encoder_info( hnd_t encoder )
{
    assert( encoder );
    struct aenc_t *enc = encoder;

    return enc->enc->get_info( enc->handle );
}

audio_packet_t *audio_encode_frame( hnd_t encoder )
{
    assert( encoder );
    struct aenc_t *enc = encoder;

    return enc->enc->get_next_packet( enc->handle );
}

void audio_free_frame( hnd_t encoder, audio_packet_t *frame )
{
    assert( encoder );
    struct aenc_t *enc = encoder;

    return enc->enc->free_packet( enc->handle, frame );
}

void audio_encoder_close( hnd_t encoder )
{
    if( !encoder )
        return;
    struct aenc_t *enc = encoder;

    enc->enc->close( enc->handle );
    free( enc );
}

static const audio_encoder_t *encoder_by_name( char *name )
{
#define IFRET( enc ) if( !strcmp( #enc, name ) ) return &audio_encoder_ ## enc;
#if HAVE_LAME
    IFRET( mp3 );
#endif
    IFRET( raw );
#undef IFRET
    return NULL;
}

const audio_encoder_t *select_audio_encoder( char *encoder, char* allowed_list[] )
{
    if( !encoder )
        return NULL;
    if( allowed_list )
    {
        if( !strcmp( encoder, "default" ) )
            return encoder_by_name( allowed_list[0] );
        int valid = 0;
        for( int i = 0; allowed_list[i] != NULL; i++ )
            if( !strcmp( encoder, allowed_list[i] ) )
            {
                valid = 1;
                break;
            }
        if( !valid )
            return NULL;
    }
    return encoder_by_name( encoder );
}
