#include "audio/encoders.h"
#include "filters/audio/internal.h"
#undef DECLARE_ALIGNED
#include "libavcodec/avcodec.h"

#include <assert.h>

typedef struct enc_lavc_t
{
    audio_info_t info;
    audio_info_t preinfo;
    hnd_t filter_chain;
    int finishing;
    int64_t last_sample;
    int buf_size;

    AVCodecContext *ctx;
    enum SampleFormat smpfmt;
} enc_lavc_t;

static int is_encoder_available( const char *name, void **priv )
{
    avcodec_register_all();
    AVCodec *enc = NULL;

    if( (enc = avcodec_find_encoder_by_name( name )) )
    {
        if( priv )
            *priv = enc;
        return 0;
    }

    if( name[0] == 'f' && name[1] == 'f' )
    {
        enc = avcodec_find_encoder_by_name( &name[2] );
        if( enc )
        {
            if( priv )
                *priv = enc;
            return 0;
        }
    }

    return -1;
}

static const struct {
    enum CodecID id;
    const char *name;
} ffcodecid_to_codecname[] = {
    { CODEC_ID_MP3,    "mp3", },
    { CODEC_ID_VORBIS, "vorbis", },
    { CODEC_ID_AAC,    "aac", },
    { CODEC_ID_AC3,    "ac3", },
    { CODEC_ID_ALAC,   "alac", },
    { CODEC_ID_AMR_NB, "amrnb", },
    { CODEC_ID_NONE,   NULL, },
};

#define ISCODEC( name ) (!strcmp( h->info.codec_name, #name ))

static hnd_t init( hnd_t filter_chain, const char *opt_str )
{
    assert( filter_chain );
    enc_lavc_t *h = calloc( 1, sizeof( enc_lavc_t ) );
    audio_hnd_t *chain = h->filter_chain = filter_chain;
    h->preinfo = h->info = chain->info;

    char **opts = x264_split_options( opt_str, (const char*[]){ AUDIO_CODEC_COMMON_OPTIONS, NULL } );
    assert( opts );

    const char *codecname = x264_get_option( "codec", opts );
    RETURN_IF_ERR( !codecname, "lavc", NULL, "codec not specified" );

    avcodec_register_all();

    AVCodec *codec = NULL;
    RETURN_IF_ERR( is_encoder_available( codecname, (void **)&codec ),
                   "lavc", NULL, "codec %s not supported or compiled in\n", codecname );

    int i;
    h->info.codec_name = NULL;
    for( i = 0; ffcodecid_to_codecname[i].id != CODEC_ID_NONE; i++ )
    {
        if( codec->id == ffcodecid_to_codecname[i].id )
            h->info.codec_name = ffcodecid_to_codecname[i].name;
    }
    RETURN_IF_ERR( !h->info.codec_name, "lavc", NULL, "failed to set codec name for muxer\n" );

    for( i = 0; codec->sample_fmts[i] != -1; i++ )
    {
        // Prefer floats...
        if( codec->sample_fmts[i] == SAMPLE_FMT_FLT )
        {
            h->smpfmt = SAMPLE_FMT_FLT;
            break;
        }
        else if( h->smpfmt < codec->sample_fmts[i] ) // or the best possible sample format (is this really The Right Thing?)
            h->smpfmt = codec->sample_fmts[i];
    }
    h->ctx                  = avcodec_alloc_context();
    h->ctx->sample_fmt      = h->smpfmt;
    h->ctx->sample_rate     = h->info.samplerate;
    h->ctx->channels        = h->info.channels;
    h->ctx->channel_layout  = h->info.chanlayout;
    h->ctx->flags2         |= CODEC_FLAG2_BIT_RESERVOIR; // mp3
    h->ctx->flags          |= CODEC_FLAG_GLOBAL_HEADER; // aac

    if( ISCODEC( aac ) )
        h->ctx->profile = FF_PROFILE_AAC_LOW; // TODO: decide by bitrate / quality

    int is_vbr  = x264_otob( x264_get_option( "is_vbr", opts ), 1 );
    float brval;
    if( is_vbr )
        brval = x264_otof( x264_get_option( "bitrate", opts ), ISCODEC( aac ) ? 100 : // Should this really be per-codec? /me thinks libavcodec fails at wrapping
                                                               ISCODEC( mp3 ) ? 6 : 1 );
    else
        brval = x264_otof( x264_get_option( "bitrate", opts ), 128 ); // dummy default value, must never be used

    h->ctx->compression_level = x264_otof( x264_get_option( "quality", opts ), FF_COMPRESSION_DEFAULT );

    x264_free_string_array( opts );

    if( is_vbr )
    {
        h->ctx->flags         |= CODEC_FLAG_QSCALE;
        h->ctx->global_quality = FF_QP2LAMBDA * brval;
    }
    else
        h->ctx->bit_rate = lrintf( brval * 1000.0f );

    RETURN_IF_ERR( avcodec_open( h->ctx, codec ), "lavc", NULL, "could not open the %s encoder\n", codec->name );

    if( ISCODEC( ac3 ) )
    {
        audio_packet_t *pkt = x264_af_get_samples( h->filter_chain, 0, h->ctx->frame_size );
        RETURN_IF_ERR( !pkt, "lavc", NULL, "could not get a audio frame\n" );

        pkt->data = malloc( FF_MIN_BUFFER_SIZE * 3 / 2 );

        void *indata = x264_af_interleave2( h->smpfmt, pkt->samples, pkt->channels, pkt->samplecount );
        pkt->size = avcodec_encode_audio( h->ctx, pkt->data, pkt->channels * pkt->samplecount, indata );

        h->ctx->frame_number = 0;
        h->ctx->extradata_size = pkt->size;
        h->ctx->extradata = av_malloc( h->ctx->extradata_size );
        RETURN_IF_ERR( !h->ctx->extradata, "lavc", NULL, "malloc failed!\n" );
        memcpy( h->ctx->extradata, pkt->data, h->ctx->extradata_size );

        x264_af_free_packet( pkt );
    }

    h->info.extradata       = h->ctx->extradata;
    h->info.extradata_size  = h->ctx->extradata_size;
    h->info.framelen        = h->ctx->frame_size;
    h->info.chansize        = av_get_bits_per_sample_format( h->ctx->sample_fmt ) / 8;
    h->info.samplesize      = h->info.chansize * h->info.channels;
    h->info.framesize       = h->info.framelen * h->info.samplesize;
    h->info.timebase        = (timebase_t) { 1, h->ctx->sample_rate };

    h->buf_size = !ISCODEC( alac )
                      ? FF_MIN_BUFFER_SIZE * 3 / 2
                      : 2 * (8 + h->info.framesize);

    x264_cli_log( "audio", X264_LOG_INFO, "opened libavcodec's %s encoder (%s%.1f%s, %dbits, %dch, %dhz)\n", codec->name,
                  is_vbr ? "V" : "", brval, is_vbr ? "" : "kbps", h->info.chansize * 8, h->info.channels, h->info.samplerate );
    return h;
}

static audio_info_t *get_info( hnd_t handle )
{
    assert( handle );
    enc_lavc_t *h = handle;

    return &h->info;
}

static audio_packet_t *get_next_packet( hnd_t handle )
{
    enc_lavc_t *h = handle;
    if( h->finishing )
        return NULL;
    assert( h->ctx );


    audio_packet_t *out = calloc( 1, sizeof( audio_packet_t ) );
    out->info = h->info;
    out->dts  = h->last_sample;
    out->data = malloc( h->buf_size );
    while( out->size == 0 )
    {
        audio_packet_t *smp = x264_af_get_samples( h->filter_chain, h->last_sample, h->last_sample + h->info.framelen );
        if( !smp )
            return NULL;

        out->samplecount = smp->samplecount;
        out->channels    = smp->channels;

        void *indata   = x264_af_interleave2( h->smpfmt, smp->samples, smp->channels, smp->samplecount );
        out->size       = avcodec_encode_audio( h->ctx, out->data, h->buf_size, indata );
        h->last_sample += h->info.framelen;

        x264_af_free_packet( smp );
    }
    if( out->size < 0 )
    {
        x264_cli_log( "lavc", X264_LOG_ERROR, "error encoding audio! (%s)\n", strerror( -out->size ) );
        x264_af_free_packet( out );
        return NULL;
    }

    return out;
}

static void skip_samples( hnd_t handle, uint64_t samplecount )
{
    ((enc_lavc_t*)handle)->last_sample += samplecount;
}

static audio_packet_t *finish( hnd_t handle )
{
    ((enc_lavc_t*)handle)->finishing = 1;
    return NULL;
}

static void free_packet( hnd_t handle, audio_packet_t *packet )
{
    packet->owner = NULL;
    x264_af_free_packet( packet );
}

static void lavc_close( hnd_t handle )
{
    enc_lavc_t *h = handle;

    avcodec_close( h->ctx );
    av_free( h->ctx );
    free( h );
}

static void lavc_help_default( const char * const encoder_name )
{
    printf( "      * (ff)%s encoder help\n", encoder_name );
    printf( "        No detailed help available at present\n" );
    printf( "\n" );
}

static void lavc_help_amrnb( const char * const encoder_name )
{
    printf( "      * (ff)%s encoder help\n", encoder_name );
    printf( "        This encoder accepts only mono (1ch), 8000Hz audio.\n" );
    printf( "        --aquality        Cannot be used\n" );
    printf( "        --abitrate        Only one of the values below can be acceptable\n" );
    printf( "                             4.75, 5.15, 5.9, 6.7, 7.4, 7.95, 10.2, 12.2\n" );
    printf( "\n" );
}

static void lavc_help( const char * const encoder_name )
{
    AVCodec *enc = NULL;

    if( is_encoder_available( encoder_name, (void **)&enc ) )
        return;

#define SHOWHELP( encoder, helpname ) if( !strcmp( enc->name, #encoder ) ) lavc_help_##helpname ( #encoder );

#if 0
    SHOWHELP( libmp3lame, default );
    SHOWHELP( libfaac, default );
    SHOWHELP( aac, default );
    SHOWHELP( ac3, default );
    SHOWHELP( alac, default );
    SHOWHELP( libvorbis, default );
    SHOWHELP( vorbis, default );
#endif
    SHOWHELP( libopencore_amrnb, amrnb );

#undef SHOWHELP

    return;
}

const audio_encoder_t audio_encoder_lavc =
{
    .init            = init,
    .get_info        = get_info,
    .get_next_packet = get_next_packet,
    .skip_samples    = skip_samples,
    .finish          = finish,
    .free_packet     = free_packet,
    .close           = lavc_close,
    .show_help       = lavc_help,
    .is_valid_encoder = is_encoder_available
};
