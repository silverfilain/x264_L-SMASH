#include "filters/audio/audio_internal.h"

#include <assert.h>

typedef struct muxer_raw_t
{
    AUDIO_FILTER_COMMON
    FILE *fh;
} muxer_raw_t;

static enum AudioResult init( const struct audio_filter_t *self, hnd_t previous, hnd_t *handle, const char *opt_str )
{
    if( !previous )
    {
        fprintf( stderr, "muxer_raw [error]: muxer_raw requires a previous filter!" );
        return AUDIO_ERROR;
    }
    char *optlist[] = { "filename", NULL };
    char **opts     = split_options( opt_str, optlist );

    if( !opts )
        return AUDIO_ERROR;

    char *filename = get_option( "filename", opts );
    assert( filename );

    INIT_FILTER_STRUCT( muxer_raw_t );

    if( !strcmp( filename, "-" ) )
        h->fh = stdout;
    else
    {
        h->fh = fopen( filename, "wb" );
        if( !h->fh )
        {
            fprintf( stderr, "muxer_raw [error]: error opening \"%s\" for writing!\n", filename );
            goto fail;
        }
    }

    fprintf( stderr, "muxer_raw [verbose]: writing raw audio to \"%s\".\n", filename );

    free_string_array( opts );
    return AUDIO_OK;

fail:
    free_string_array( opts );
    if( h->fh )
        fclose( h->fh );
    free( h );
    *handle = NULL;
    return AUDIO_ERROR;
}

static struct AVPacket *get_samples( hnd_t handle, int64_t first_sample, int64_t last_sample )
{
    muxer_raw_t *h = handle;
    
    AVPacket *samples = h->prev->self->get_samples( h->prev, first_sample, last_sample );
    if( samples )
        fwrite( samples->data, samples->size, 1, h->fh );

    return samples;
}

static int free_packet( hnd_t handle, struct AVPacket *frame )
{
    muxer_raw_t *h = handle;
    return h->prev->self->free_packet( h->prev, frame );
}

static enum AudioResult close( hnd_t handle )
{
    assert( handle );
    muxer_raw_t *h = handle;
    fclose( h->fh );
    free( h->info );
    free( h );
    return AUDIO_OK;
}

const audio_filter_t audio_muxer_raw =
{
    .name = "muxer_raw",
    .description = "Writes raw audio to a specified file",
    .help = "Arguments: filename",
    .init = init,
    .get_samples = get_samples,
    .free_packet = free_packet,
    .close = close
};
