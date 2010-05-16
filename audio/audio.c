#include "filters/audio/internal.h"

#include <assert.h>

hnd_t audio_open_from_file( audio_filter_t *preferred_filter, char *path, int trackno )
{
    audio_filter_t *source = preferred_filter ? preferred_filter : af_get_filter( AUDIO_SOURCE_LAVF );
    hnd_t h;
    size_t init_arg_size = sizeof( path ) + 10;
    char *init_arg = malloc( init_arg_size );
    assert( snprintf( init_arg, init_arg_size, "%s:%d", path, trackno ) < init_arg_size );
    if( source->init( source, NULL, &h, init_arg ) != AUDIO_OK || !h )
    {
        fprintf( stderr, "audio [error]: error initializing source filter!\n" );
        return NULL;
    }
    free( init_arg );
    return h;
}
int audio_close( hnd_t chain )
{
    return af_close( chain );
}
