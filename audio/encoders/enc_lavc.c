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
    int64_t last_dts;

    AVCodecContext *ctx;
    enum AVSampleFormat smpfmt;
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

    return -1;
}

#define MODE_VBR     0x01
#define MODE_BITRATE 0x02
#define MODE_IGNORED MODE_VBR|MODE_BITRATE

static const struct {
    enum CodecID id;
    const char *name;
    uint8_t mode;
    float default_brval;
} ffcodecs[] = {
   /* CODEC_ID,           name,        allowed mode,  default quality/bitrate */
    { CODEC_ID_MP2,       "mp2",       MODE_BITRATE,    112 },
    { CODEC_ID_VORBIS,    "vorbis",    MODE_VBR,        5.0 },
    { CODEC_ID_AAC,       "aac",       MODE_BITRATE,     96 },
    { CODEC_ID_AC3,       "ac3",       MODE_BITRATE,     96 },
    { CODEC_ID_ALAC,      "alac",      MODE_IGNORED,     64 },
    { CODEC_ID_AMR_NB,    "amrnb",     MODE_BITRATE,   12.2 },
    { CODEC_ID_AMR_WB,    "amrwb",     MODE_BITRATE,  12.65 },
    { CODEC_ID_PCM_F32BE, "pcm_f32be", MODE_IGNORED,      0 },
    { CODEC_ID_PCM_F32LE, "pcm_f32le", MODE_IGNORED,      0 },
    { CODEC_ID_PCM_F64BE, "pcm_f64be", MODE_IGNORED,      0 },
    { CODEC_ID_PCM_F64LE, "pcm_f64le", MODE_IGNORED,      0 },
    { CODEC_ID_PCM_S16BE, "pcm_s16be", MODE_IGNORED,      0 },
    { CODEC_ID_PCM_S16LE, "pcm_s16le", MODE_IGNORED,      0 },
    { CODEC_ID_PCM_S24BE, "pcm_s24be", MODE_IGNORED,      0 },
    { CODEC_ID_PCM_S24LE, "pcm_s24le", MODE_IGNORED,      0 },
    { CODEC_ID_PCM_S32BE, "pcm_s32be", MODE_IGNORED,      0 },
    { CODEC_ID_PCM_S32LE, "pcm_s32le", MODE_IGNORED,      0 },
    { CODEC_ID_PCM_S8,    "pcm_s8",    MODE_IGNORED,      0 },
    { CODEC_ID_PCM_U16BE, "pcm_u16be", MODE_IGNORED,      0 },
    { CODEC_ID_PCM_U16LE, "pcm_u16le", MODE_IGNORED,      0 },
    { CODEC_ID_PCM_U24BE, "pcm_u24be", MODE_IGNORED,      0 },
    { CODEC_ID_PCM_U24LE, "pcm_u24le", MODE_IGNORED,      0 },
    { CODEC_ID_PCM_U32BE, "pcm_u32be", MODE_IGNORED,      0 },
    { CODEC_ID_PCM_U32LE, "pcm_u32le", MODE_IGNORED,      0 },
    { CODEC_ID_PCM_U8,    "pcm_u8",    MODE_IGNORED,      0 },
    { CODEC_ID_NONE,      NULL, },
};

static int encode_audio( AVCodecContext *ctx, audio_packet_t *out, AVFrame *frame )
{
    AVPacket avpkt;
    av_init_packet( &avpkt );
    avpkt.data = NULL;
    avpkt.size = 0;

    int got_packet = 0;

    if( avcodec_encode_audio2( ctx, &avpkt, frame, &got_packet ) < 0 )
        return -1;

    if( got_packet )
    {
        out->size = avpkt.size;
        memcpy( out->data, avpkt.data, avpkt.size );
    }
    else
        out->size = 0;

    return 0;
}

#define ISCODEC( name ) (!strcmp( h->info.codec_name, #name ))

static hnd_t init( hnd_t filter_chain, const char *opt_str )
{
    assert( filter_chain );
    enc_lavc_t *h = calloc( 1, sizeof( enc_lavc_t ) );
    audio_hnd_t *chain = h->filter_chain = filter_chain;
    h->preinfo = h->info = chain->info;

    char **opts = x264_split_options( opt_str, (const char*[]){ AUDIO_CODEC_COMMON_OPTIONS, NULL } );
    if( !opts )
    {
        x264_cli_log( "lavc", X264_LOG_ERROR, "wrong audio options.\n" );
        return NULL;
    }

    const char *codecname = x264_get_option( "codec", opts );
    RETURN_IF_ERR( !codecname, "lavc", NULL, "codec not specified" );

    avcodec_register_all();

    AVCodec *codec = NULL;
    RETURN_IF_ERR( is_encoder_available( codecname, (void **)&codec ),
                   "lavc", NULL, "codec %s not supported or compiled in\n", codecname );

    int i, j;
    h->info.codec_name = NULL;
    for( i = 0; ffcodecs[i].id != CODEC_ID_NONE; i++ )
    {
        if( codec->id == ffcodecs[i].id )
        {
            h->info.codec_name = ffcodecs[i].name;
            break;
        }
    }
    RETURN_IF_ERR( !h->info.codec_name, "lavc", NULL, "failed to set codec name for muxer\n" );

    for( j = 0; codec->sample_fmts[j] != -1; j++ )
    {
        // Prefer floats...
        if( codec->sample_fmts[j] == AV_SAMPLE_FMT_FLT )
        {
            h->smpfmt = AV_SAMPLE_FMT_FLT;
            break;
        }
        else if( h->smpfmt < codec->sample_fmts[j] ) // or the best possible sample format (is this really The Right Thing?)
            h->smpfmt = codec->sample_fmts[j];
    }

    h->ctx                  = avcodec_alloc_context3( NULL );
    h->ctx->sample_fmt      = h->smpfmt;
    h->ctx->sample_rate     = h->info.samplerate;
    h->ctx->channels        = h->info.channels;
    h->ctx->channel_layout  = h->info.chanlayout;
    h->ctx->time_base       = (AVRational){ 1, h->ctx->sample_rate };

    AVDictionary *avopts = NULL;
    av_dict_set( &avopts, "flags", "global_header", 0 ); // aac
    av_dict_set( &avopts, "reservoir", "1", 0 ); // mp3

    if( ISCODEC( aac ) )
        av_dict_set( &avopts, "profile", "aac_low", 0 ); // TODO: decide by bitrate / quality

    int is_vbr = x264_otob( x264_get_option( "is_vbr", opts ), ffcodecs[i].mode & MODE_VBR ? 1 : 0 );

    RETURN_IF_ERR( ( !(ffcodecs[i].mode & MODE_BITRATE) && !is_vbr ) || ( !(ffcodecs[i].mode & MODE_VBR) && is_vbr ),
                   "lavc", NULL, "libavcodec's %s encoder doesn't allow %s mode.\n", codecname, is_vbr ? "VBR" : "bitrate" );

    float default_brval = is_vbr ? ffcodecs[i].default_brval : ffcodecs[i].default_brval * h->ctx->channels;
    float brval = x264_otof( x264_get_option( "bitrate", opts ), default_brval );

    h->ctx->compression_level = x264_otof( x264_get_option( "quality", opts ), FF_COMPRESSION_DEFAULT );

    x264_free_string_array( opts );

    if( is_vbr )
    {
        av_dict_set( &avopts, "flags", "qscale", 0 );
        h->ctx->global_quality = FF_QP2LAMBDA * brval;
    }
    else
        h->ctx->bit_rate = lrintf( brval * 1000.0f );

    RETURN_IF_ERR( avcodec_open2( h->ctx, codec, &avopts ), "lavc", NULL, "could not open the %s encoder\n", codec->name );

    if( ISCODEC( ac3 ) )
    {
        audio_packet_t *pkt = x264_af_get_samples( h->filter_chain, 0, h->ctx->frame_size );
        RETURN_IF_ERR( !pkt, "lavc", NULL, "could not get a audio frame\n" );

        pkt->data = malloc( FF_MIN_BUFFER_SIZE * 3 / 2 );
        RETURN_IF_ERR( !pkt->data, "lavc", NULL, "malloc failed!\n" );

        AVFrame frame;
        avcodec_get_frame_defaults( &frame );
        frame.nb_samples  = pkt->samplecount;
        frame.linesize[0] = pkt->channels * pkt->samplecount;
        frame.data[0]     = x264_af_interleave2( h->smpfmt, pkt->samples, pkt->channels, pkt->samplecount );

        if( encode_audio( h->ctx, pkt, &frame ) < 0 )
        {
            x264_cli_log( "lavc", X264_LOG_ERROR, "error encoding audio! (%s)\n", strerror( -pkt->size ) );
            x264_af_free_packet( pkt );
            return NULL;
        }

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
    h->info.timebase        = (timebase_t) { 1, h->ctx->sample_rate };
    h->info.last_delta      = h->info.framelen;
    h->info.depth           = av_get_bits_per_sample( h->ctx->codec->id );
    h->info.chansize        = IS_LPCM_CODEC_ID( h->ctx->codec->id )
                            ? h->info.depth / 8
                            : av_get_bytes_per_sample( h->ctx->sample_fmt );
    h->info.samplesize      = h->info.chansize * h->info.channels;
    h->info.framesize       = h->info.framelen * h->info.samplesize;

    if( ISCODEC( alac ) )
        h->buf_size = 2 * (8 + h->info.framesize);
    else if( h->info.framelen == 0 )
    {
        /* framelen == 0 indicates the actual frame size is based on the buf_size passed to avcodec_encode_audio2(). */
        h->info.framelen = 1;   /* arbitrary */
        h->buf_size = h->info.framesize = h->info.framelen * h->info.samplesize;
    }
    else
        h->buf_size = FF_MIN_BUFFER_SIZE * 3 / 2;
    h->last_dts = INVALID_DTS;

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

    audio_packet_t *smp = NULL;
    audio_packet_t *out = calloc( 1, sizeof( audio_packet_t ) );
    out->info = h->info;
    out->data = malloc( h->buf_size );

    while( out->size == 0 )
    {
        smp = x264_af_get_samples( h->filter_chain, h->last_sample, h->last_sample + h->info.framelen );
        if( !smp )
            goto error; // not an error but need same handling

        out->samplecount = smp->samplecount;
        out->channels    = smp->channels;

        /* x264_af_interleave2 allocates only samplecount * channels * bytes per sample per single channel, */
        /* but lavc expects sample buffer contains h->ctx->frame_size samples (at least, alac encoder does). */
        /* If codec has capabilitiy to accept samples < default frame length, need to modify frame_size to */
        /* specify real sample counts in sample buffer, and if not, need to padding buffer. */
        if( smp->samplecount < h->info.framelen )
        {
            h->finishing = 1;
            if( !(smp->flags & AUDIO_FLAG_EOF) )
            {
                x264_cli_log( "lavc", X264_LOG_ERROR, "samples too few but not EOF???\n" );
                goto error;
            }

            if( h->ctx->codec->capabilities & CODEC_CAP_SMALL_LAST_FRAME )
                h->ctx->frame_size = h->info.last_delta = smp->samplecount;
            else
            {
                if( x264_af_resize_fill_buffer( smp->samples, h->info.framelen, h->info.channels, smp->samplecount, 0.0f ) )
                {
                    x264_cli_log( "lavc", X264_LOG_ERROR, "failed to expand buffer.\n" );
                    goto error;
                }
                smp->samplecount = h->info.last_delta = h->info.framelen;
            }
        }

        if( h->last_dts == INVALID_DTS )
            h->last_dts = h->last_sample;
        h->last_sample += smp->samplecount;

        AVFrame frame;
        avcodec_get_frame_defaults( &frame );
        frame.nb_samples  = smp->samplecount;
        frame.linesize[0] = h->buf_size;
        frame.data[0]     = x264_af_interleave2( h->smpfmt, smp->samples, smp->channels, smp->samplecount );

        if( encode_audio( h->ctx, out, &frame ) < 0 )
        {
            x264_cli_log( "lavc", X264_LOG_ERROR, "error encoding audio! (%s)\n", strerror( -out->size ) );
            goto error;
        }

        x264_af_free_packet( smp );
    }

    if( out->size < 0 )
    {
        x264_cli_log( "lavc", X264_LOG_ERROR, "error encoding audio! (%s)\n", strerror( -out->size ) );
        goto error;
    }

    out->dts     = h->last_dts;
    h->last_dts += h->info.framelen;
    return out;

error:
    if( smp )
        x264_af_free_packet( smp );
    x264_af_free_packet( out );
    return NULL;
}

static void skip_samples( hnd_t handle, uint64_t samplecount )
{
    ((enc_lavc_t*)handle)->last_sample += samplecount;
}

static audio_packet_t *finish( hnd_t handle )
{
    enc_lavc_t *h = handle;

    h->finishing = 1;

    audio_packet_t *out = calloc( 1, sizeof( audio_packet_t ) );
    out->info     = h->info;
    out->channels = h->info.channels;
    out->data     = malloc( h->buf_size );

    if( encode_audio( h->ctx, out, NULL ) < 0 )
        goto error;

    if( out->size <= 0 )
        goto error;

    out->dts = h->last_dts;
    out->samplecount = h->info.framelen;
    h->last_dts     += h->info.framelen;
    return out;

error:
    x264_af_free_packet( out );
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

static void lavc_help_amrnb( const char * const encoder_name )
{
    printf( "      * (ff)%s encoder help\n", encoder_name );
    printf( "        Accepts only mono (1ch), 8000Hz audio and not capable of quality based VBR\n" );
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
