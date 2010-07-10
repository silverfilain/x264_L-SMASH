#include "audio/encoders.h"
#include "filters/audio/internal.h"

#include "lame/lame.h"
#include <assert.h>

typedef struct enc_lame_t {
    audio_info_t *info;
    audio_info_t *af_info;
    hnd_t filter_chain;

    lame_global_flags *lame;
    int64_t last_sample;
    uint8_t *buffer;
    size_t bufsize;
    audio_packet_t *in;
} enc_lame_t;

static hnd_t init( hnd_t filter_chain, const char *opt_str )
{
    assert( filter_chain );
    enc_lame_t *h = calloc( 1, sizeof( enc_lame_t ) );
    audio_hnd_t *chain = h->filter_chain = filter_chain;
    h->af_info = af_get_info(chain);
    h->info = malloc( sizeof( audio_info_t ) );
    memcpy( h->info, h->af_info, sizeof( audio_info_t ) );

    if( h->info->chansize != 2 )
    {
        // FIXME: auto-insert converter when it is implemented
        x264_cli_log("lame", X264_LOG_ERROR, "lame only sanely supports 16 bits per channel.\n" );
        goto error;
    }

    char *optlist[] = { "bitrate", "vbr", "quality", NULL };
    char **opts     = split_options( opt_str, optlist );
    assert( opts );

    char *cbr = get_option( "bitrate", opts );
    char *vbr = get_option( "vbr"    , opts );
    char *qua = get_option( "quality", opts );
    float brval = cbr ? (float) atoi( cbr ) : vbr ? atof( vbr ) : 6.0;

    free_string_array( opts );

    assert( ( cbr && !vbr ) || ( !cbr && vbr ) );

    h->info->codec_name     = "mp3";
    h->info->extradata      = NULL;
    h->info->extradata_size = 0;

    h->lame = lame_init();
    lame_set_in_samplerate( h->lame, h->info->samplerate );
    lame_set_num_channels( h->lame, h->info->channels );
    lame_set_quality( h->lame, 0 );
    lame_set_VBR( h->lame, vbr_default );

    if( cbr )
    {
        lame_set_VBR( h->lame, vbr_off );
        lame_set_brate( h->lame, (int) brval );
    }
    else
        lame_set_VBR_quality( h->lame, brval );
    if( qua )
        lame_set_quality( h->lame, atoi( qua ) );

    lame_init_params( h->lame );

    h->info->framelen = lame_get_framesize( h->lame );
    h->info->framesize = h->info->framelen * h->info->samplesize;

    h->bufsize = 125 * h->info->framelen / 100 + 7200;

    x264_cli_log( "audio", X264_LOG_INFO, "opened lame mp3 encoder (%s: %g%s)\n",
             ( cbr ? "bitrate" : "VBR" ), brval,
             ( cbr ? "kbps" : "" ) );

    return h;

error:
    free( h->info );
    free( h );
    return NULL;
}

static audio_info_t *get_info( hnd_t handle )
{
    assert( handle );
    enc_lame_t *h = handle;

    return h->info;
}

static void free_packet( hnd_t handle, audio_packet_t *packet )
{
    packet->owner = NULL;
    af_free_packet( packet );
}

static audio_packet_t *get_next_packet( hnd_t handle )
{
    enc_lame_t *h = handle;

    audio_packet_t *out = calloc( 1, sizeof( audio_packet_t ) );
    out->data = malloc( h->bufsize );

    while( !out->size )
    {
        if( h->in && h->in->flags & AUDIO_FLAG_EOF )
        {
            out->size = lame_encode_flush( h->lame, out->data, h->bufsize );
            if( !out->size )
                goto error;
            break;
        }

        if( !( h->in = af_get_samples( h->filter_chain, h->last_sample, h->last_sample + h->info->framelen ) ) )
            goto error;
        h->last_sample += h->in->samplecount;

        out->size = lame_encode_buffer_interleaved( h->lame, (short*) h->in->data,
                                                   h->in->samplecount,
                                                   out->data, h->bufsize );
        af_free_packet( h->in );
    }

    return out;

error:
    free_packet( h, out );
    return NULL;
}

static void close( hnd_t handle )
{
    enc_lame_t *h = handle;

    lame_close( h->lame );
    free( h->info );
    free( h );
}

const audio_encoder_t audio_encoder_mp3 = {
    .init = init,
    .get_info = get_info,
    .get_next_packet = get_next_packet,
    .free_packet = free_packet,
    .close = close
};

