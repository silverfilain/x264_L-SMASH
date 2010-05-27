#include "audio/muxers.h"
#include "filters/audio/internal.h"

#include <assert.h>
#include <stdio.h>

typedef struct muxer_raw_t
{
    const audio_muxer_t *self;
    audio_info_t *info;
    hnd_t *prev;
    
    FILE *fh;
    int64_t pts;
    audio_samples_t samples;
} muxer_raw_t;

static hnd_t *init( const struct audio_muxer_t *self, hnd_t filter_chain, const char *opts )
{
    assert( self );
    assert( filter_chain );

    char *filename = "audio.raw";
    if ( opts ) {
        char *optlist[] = { "filename", NULL };
        char **opt = split_options( opts, optlist );
        if( !opt )
            return NULL;

        filename = get_option( "filename", opt );
        free_string_array( opt );
        
        assert( filename );
    }
    muxer_raw_t *h = calloc( 1, sizeof( muxer_raw_t ) );
    if( !h )
        return NULL;
    
    h->self = self;
    audio_hnd_t *p = filter_chain;
    h->prev = filter_chain;

    h->info = malloc( sizeof( audio_info_t ) );
    memcpy( h->info, p->info, sizeof( audio_info_t ) );
    
    if( !strcmp( filename, "-" ) )
        h->fh = stdout;
    else
    {
        h->fh = fopen( filename, "wb" );
        if( !h->fh )
        {
            fprintf( stderr, "muxer_raw [error]: error opening \"%s\" for writing!\n", filename );
            free( h->info );
            free( h );
            p->next = NULL;
            return NULL;
        }
    }
    return (hnd_t)h;
}

static int write_audio( hnd_t handle, int64_t pts )
{
    muxer_raw_t *h = handle;
    int len = 0;
    while ( h->pts < pts )
    {
        if( af_get_samples( &h->samples, h->prev, h->pts, h->pts + h->info->framelen ) != AUDIO_OK )
            return -1;
        
        len += h->samples.len;
        fwrite( h->samples.data, 1, h->samples.len, h->fh );
        
        af_free_samples( &h->samples );
        h->pts += h->info->framelen;
    }

    return len;
}

static enum AudioResult close( hnd_t handle )
{
    muxer_raw_t *h = handle;
    
    fclose( h->fh );
    free( h->info );
    free( h );

    return AUDIO_OK;
}

const audio_muxer_t audio_muxer_raw =
{
    .init = init,
    .write_audio = write_audio,
    .close = close
};
