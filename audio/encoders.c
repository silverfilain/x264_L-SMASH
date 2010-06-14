#include "audio/encoders.h"

#include <assert.h>
#include <stdlib.h>

struct aenc_t
{
    const audio_encoder_t *enc;
    hnd_t handle;
};

hnd_t audio_encoder_open( const audio_encoder_t *encoder, hnd_t filter_chain, const char *opts[] )
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

audio_samples_t *audio_encode_frame( hnd_t encoder )
{
    assert( encoder );
    struct aenc_t *enc = encoder;
    
    return enc->enc->get_next_packet( enc->handle );    
}

void audio_free_frame( hnd_t encoder, audio_samples_t *frame )
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
