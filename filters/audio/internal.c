#include "filters/audio/internal.h"

#include <assert.h>

typedef struct audio_filter_list_t
{
    struct audio_filter_list_t *next;
    audio_filter_t *filter;
} audio_filter_list_t;

audio_filter_list_t *filter_list;

audio_filter_t *af_get_filter_by_id( enum AudioFilter id )
{
    assert( filter_list );
    int i = 0;
    audio_filter_list_t *f = filter_list;
    while( i++ < id )
    {
        f = f->next;
        if( !f )
        {
            fprintf( stderr, "audio [error]: Invalid filter ID requested: %d\n", id );
            return NULL;
        }
    }
    return f->filter;
}

static void append_to_filter_list( audio_filter_t *f )
{
    if( !filter_list )
    {
        filter_list = calloc( 1, sizeof( audio_filter_list_t ) );
        filter_list->filter = f;
    }
    else
    {
        audio_filter_list_t *last_filter = filter_list;
        while( last_filter->next )
            last_filter = last_filter->next;
        last_filter->next = calloc( 1, sizeof( audio_filter_list_t ) );
        last_filter->next->filter = f;
    }
}

/* NOTE: this function must be synchronized with the AudioFilter enum (audio.h) */
void af_register_all( void )
{
#define REGISTER(typename) { extern audio_filter_t typename; append_to_filter_list( &typename ); }
    REGISTER( audio_source_lavf );
#undef REGISTER
}

static void free_filter_list( audio_filter_list_t *f )
{
    if( !f )
        return;
    if( f->next )
        free_filter_list( f->next );
    free( f );
}

void af_unregister_all( void )
{
    free_filter_list( filter_list );
    filter_list = NULL;
}
