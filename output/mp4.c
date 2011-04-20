/*****************************************************************************
 * mp4.c: mp4 muxer using L-SMASH
 *****************************************************************************
 * Copyright (C) 2003-2011 x264 project
 *
 * Authors: Laurent Aimar <fenrir@via.ecp.fr>
 *          Loren Merritt <lorenm@u.washington.edu>
 *          Yusuke Nakamura <muken.the.vfrmaniac@gmail.com>
 *          Takashi Hirata <silverfilain@gmail.com>
 *          golgol7777 <golgol7777@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
 *
 * This program is also available under a commercial proprietary license.
 * For more information, contact us at licensing@x264.com.
 *****************************************************************************/

#include "common/common.h"
#include "output.h"
#include "mp4/lsmash.h"
#include "mp4/importer.h"

/*******************/

#define USE_LSMASH_IMPORTER 0

#if ( defined(HAVE_AUDIO) && HAVE_AUDIO ) || ( defined(USE_LSMASH_IMPORTER) && USE_LSMASH_IMPORTER )
#define HAVE_ANY_AUDIO 1
#else
#define HAVE_ANY_AUDIO 0
#endif

/*******************/

#define MP4_LOG_ERROR( ... )                x264_cli_log( "mp4", X264_LOG_ERROR, __VA_ARGS__ )
#define MP4_LOG_WARNING( ... )              x264_cli_log( "mp4", X264_LOG_WARNING, __VA_ARGS__ )
#define MP4_LOG_INFO( ... )                 x264_cli_log( "mp4", X264_LOG_INFO, __VA_ARGS__ )
//#define MP4_RETURN_IF_ERR( cond, ret, ... ) RETURN_IF_ERR( cond, "mp4", ret, __VA_ARGS__ )
#define MP4_FAIL_IF_ERR( cond, ... )        FAIL_IF_ERR( cond, "mp4", __VA_ARGS__ )

/* For close_file() */
#define MP4_LOG_IF_ERR( cond, ... )\
if( cond )\
{\
    MP4_LOG_ERROR( __VA_ARGS__ );\
}

/* For open_file() */
#define MP4_FAIL_IF_ERR_EX( cond, ... )\
if( cond )\
{\
    remove_mp4_hnd( p_mp4 );\
    MP4_LOG_ERROR( __VA_ARGS__ );\
    return -1;\
}

/*******************/

#if HAVE_ANY_AUDIO

#if HAVE_AUDIO
#include "audio/encoders.h"
#endif

typedef struct
{
    int i_numframe;
    uint32_t i_track;
    uint32_t i_sample_entry;
    uint64_t i_video_timescale;    /* For interleaving. */
    lsmash_audio_summary_t *summary;
    lsmash_codec_type_code codec_type;
#if HAVE_AUDIO
    audio_info_t *info;
    hnd_t encoder;
    int has_sbr;
    int b_copy;
#else
    mp4sys_importer_t* p_importer;
#endif
} mp4_audio_hnd_t;
#endif /* #if HAVE_ANY_AUDIO */

typedef struct
{
    lsmash_root_t *p_root;
    lsmash_brand_type_code major_brand;
    lsmash_video_summary_t *summary;
    int i_brand_3gpp;
    int b_brand_m4a;
    int b_brand_qt;
    char *psz_chapter;
    char *psz_language;
    uint32_t i_track;
    uint32_t i_sample_entry;
    uint64_t i_time_inc;
    int64_t i_start_offset;
    uint64_t prev_dts;
    uint32_t i_sei_size;
    uint8_t *p_sei_buffer;
    int i_numframe;
    int64_t i_init_delta;
    int i_delay_frames;
    int b_dts_compress;
    int i_dts_compress_multiplier;
    int b_use_recovery;
    int i_recovery_frame_cnt;
    int i_max_frame_num;
    uint64_t i_last_intra_cts;
    uint32_t i_display_width;
    uint32_t i_display_height;
    int b_no_remux;
    int b_no_pasp;
    int b_force_display_size;
    int b_fragments;
    lsmash_scaling_method_code scaling_method;
#if HAVE_ANY_AUDIO
    mp4_audio_hnd_t *audio_hnd;
#endif
} mp4_hnd_t;

/*******************/

static void set_recovery_param( mp4_hnd_t *p_mp4, x264_param_t *p_param )
{
    p_mp4->b_use_recovery = p_param->b_open_gop || p_param->b_intra_refresh;
    if( !p_mp4->b_use_recovery )
        return;

    /* almost copied from x264_sps_init in encoder/set.c */
    int i_num_reorder_frames = p_param->i_bframe_pyramid ? 2 : p_param->i_bframe ? 1 : 0;
    int i_num_ref_frames = X264_MIN(X264_REF_MAX, X264_MAX4(p_param->i_frame_reference, 1 + i_num_reorder_frames,
                           p_param->i_bframe_pyramid ? 4 : 1, p_param->i_dpb_size));
    i_num_ref_frames -= p_param->i_bframe_pyramid == X264_B_PYRAMID_STRICT;
    if( p_param->i_keyint_max == 1 )
        i_num_ref_frames = 0;

    p_mp4->i_max_frame_num = i_num_ref_frames * (!!p_param->i_bframe_pyramid+1) + 1;
    if( p_param->b_intra_refresh )
    {
        p_mp4->i_recovery_frame_cnt = X264_MIN( ( p_param->i_width + 15 ) / 16 - 1, p_param->i_keyint_max ) + p_param->i_bframe - 1;
        p_mp4->i_max_frame_num = X264_MAX( p_mp4->i_max_frame_num, p_mp4->i_recovery_frame_cnt + 1 );
    }

    int i_log2_max_frame_num = 4;
    while( (1 << i_log2_max_frame_num) <= p_mp4->i_max_frame_num )
        i_log2_max_frame_num++;

    p_mp4->i_max_frame_num = 1 << i_log2_max_frame_num;
}

#if HAVE_AUDIO
static void set_channel_layout( mp4_audio_hnd_t *p_audio )
{
#define CH_LAYOUT_MONO              (CH_FRONT_CENTER)
#define CH_LAYOUT_STEREO            (CH_FRONT_LEFT|CH_FRONT_RIGHT)
#define CH_LAYOUT_2_1               (CH_LAYOUT_STEREO|CH_BACK_CENTER)
#define CH_LAYOUT_SURROUND          (CH_LAYOUT_STEREO|CH_FRONT_CENTER)
#define CH_LAYOUT_4POINT0           (CH_LAYOUT_SURROUND|CH_BACK_CENTER)
#define CH_LAYOUT_2_2               (CH_LAYOUT_STEREO|CH_SIDE_LEFT|CH_SIDE_RIGHT)
#define CH_LAYOUT_QUAD              (CH_LAYOUT_STEREO|CH_BACK_LEFT|CH_BACK_RIGHT)
#define CH_LAYOUT_5POINT0           (CH_LAYOUT_SURROUND|CH_SIDE_LEFT|CH_SIDE_RIGHT)
#define CH_LAYOUT_5POINT1           (CH_LAYOUT_5POINT0|CH_LOW_FREQUENCY)
#define CH_LAYOUT_5POINT0_BACK      (CH_LAYOUT_SURROUND|CH_BACK_LEFT|CH_BACK_RIGHT)
#define CH_LAYOUT_5POINT1_BACK      (CH_LAYOUT_5POINT0_BACK|CH_LOW_FREQUENCY)
#define CH_LAYOUT_7POINT0           (CH_LAYOUT_5POINT0|CH_BACK_LEFT|CH_BACK_RIGHT)
#define CH_LAYOUT_7POINT1           (CH_LAYOUT_5POINT1|CH_BACK_LEFT|CH_BACK_RIGHT)
#define CH_LAYOUT_7POINT1_WIDE      (CH_LAYOUT_5POINT1_BACK|CH_FRONT_LEFT_OF_CENTER|CH_FRONT_RIGHT_OF_CENTER)
#define CH_LAYOUT_STEREO_DOWNMIX    (CH_STEREO_LEFT|CH_STEREO_RIGHT)

    lsmash_channel_layout_tag_code layout_tag = QT_CHANNEL_LAYOUT_UNKNOWN;
    lsmash_channel_bitmap_code bitmap = 0;

    /* Lavcodec always returns SMPTE/ITU-R channel order, but its copying doesn't do reordering. */
    if( !p_audio->b_copy )
    {
        layout_tag = QT_CHANNEL_LAYOUT_USE_CHANNEL_BITMAP;
        bitmap = p_audio->info->chanlayout;
        /* Avisynth input doesn't return channel order, so we guess it from the number of channels. */
        if( !p_audio->info->chanlayout && p_audio->info->channels <= 8 )
        {
            static const lsmash_channel_layout_tag_code channel_table[] = {
                QT_CHANNEL_LAYOUT_USE_CHANNEL_BITMAP,
                QT_CHANNEL_LAYOUT_ITU_1_0,
                QT_CHANNEL_LAYOUT_ITU_2_0,
                QT_CHANNEL_LAYOUT_ITU_3_0,
                QT_CHANNEL_LAYOUT_ITU_3_1,
                QT_CHANNEL_LAYOUT_ITU_3_2,
                QT_CHANNEL_LAYOUT_ITU_3_2_1,
                QT_CHANNEL_LAYOUT_USE_CHANNEL_BITMAP,
                QT_CHANNEL_LAYOUT_ITU_3_4_1
            };
            layout_tag = channel_table[p_audio->info->channels];
        }
    }
    else if( p_audio->codec_type == ISOM_CODEC_TYPE_MP4A_AUDIO )
    {
        /* Channel order is unknown, so we guess it from ffmpeg's channel layout flags. */
        typedef struct { lsmash_channel_bitmap_code bitmap; lsmash_channel_layout_tag_code layout_tag; } qt_channel_map;
        static const qt_channel_map channel_table[] = {
            { CH_LAYOUT_MONO,           QT_CHANNEL_LAYOUT_MONO },
            { CH_LAYOUT_STEREO,         QT_CHANNEL_LAYOUT_STEREO },
            { CH_LAYOUT_STEREO_DOWNMIX, QT_CHANNEL_LAYOUT_STEREO },
            { CH_LAYOUT_2_1,            QT_CHANNEL_LAYOUT_AAC_3_0 },
            { CH_LAYOUT_SURROUND,       QT_CHANNEL_LAYOUT_AAC_3_0 },
            { CH_LAYOUT_4POINT0,        QT_CHANNEL_LAYOUT_AAC_4_0 },
            { CH_LAYOUT_2_2,            QT_CHANNEL_LAYOUT_AAC_QUADRAPHONIC },
            { CH_LAYOUT_QUAD,           QT_CHANNEL_LAYOUT_AAC_QUADRAPHONIC },
            { CH_LAYOUT_5POINT0,        QT_CHANNEL_LAYOUT_AAC_5_0 },
            { CH_LAYOUT_5POINT0_BACK,   QT_CHANNEL_LAYOUT_AAC_5_0 },
            { CH_LAYOUT_5POINT1,        QT_CHANNEL_LAYOUT_AAC_5_1 },
            { CH_LAYOUT_5POINT1_BACK,   QT_CHANNEL_LAYOUT_AAC_5_1 },
            { CH_LAYOUT_7POINT0,        QT_CHANNEL_LAYOUT_AAC_7_0 },
            { CH_LAYOUT_7POINT1,        QT_CHANNEL_LAYOUT_AAC_7_1 },
            { CH_LAYOUT_7POINT1_WIDE,   QT_CHANNEL_LAYOUT_AAC_7_1 }
        };
        for( int i = 0; i < sizeof(channel_table)/sizeof(qt_channel_map); i++ )
            if( p_audio->info->chanlayout == channel_table[i].bitmap )
            {
                layout_tag = channel_table[i].layout_tag;
                break;
            }
    }

    p_audio->summary->layout_tag = layout_tag;
    p_audio->summary->bitmap = bitmap;
}
#endif

#if HAVE_ANY_AUDIO
static void remove_audio_hnd( mp4_audio_hnd_t *p_audio )
{
    if( p_audio->summary )
    {
        /* WARNING: You should not rely on this if you created summary in your own code. */
        lsmash_cleanup_audio_summary( p_audio->summary );
        p_audio->summary = NULL;
    }
#if HAVE_AUDIO
    if( p_audio->encoder )
    {
        x264_audio_encoder_close( p_audio->encoder );
        p_audio->encoder = NULL;
    }
#else
    if( p_audio->p_importer )
    {
        mp4sys_importer_close( p_audio->p_importer );
        p_audio->p_importer = NULL;
    }
#endif
    free( p_audio );
}
#endif

static void remove_mp4_hnd( hnd_t handle )
{
    mp4_hnd_t *p_mp4 = handle;
    if( !p_mp4 )
        return;
    if( p_mp4->p_sei_buffer )
    {
        free( p_mp4->p_sei_buffer );
        p_mp4->p_sei_buffer = NULL;
    }
    if( p_mp4->p_root )
    {
        lsmash_destroy_root( p_mp4->p_root );
        p_mp4->p_root = NULL;
    }
#if HAVE_ANY_AUDIO
    if( p_mp4->audio_hnd )
    {
        remove_audio_hnd( p_mp4->audio_hnd );
        p_mp4->audio_hnd = NULL;
    }
#endif
    free( p_mp4 );
}

#if HAVE_AUDIO
static int audio_init( hnd_t handle, hnd_t filters, char *audio_enc, char *audio_parameters )
{
    if( !strcmp( audio_enc, "none" ) || !filters )
        return 0;

    // TODO: support other audio format
    hnd_t henc;
    int copy = 0;

    if( !strcmp( audio_enc, "copy" ) )
    {
        henc = x264_audio_copy_open( filters );
        copy = 1;
    }
    else
    {
        char audio_params[MAX_ARGS];
        const char *used_enc;
        char *codec_list[] = { "aac", "mp3", "ac3", "alac", "raw", "amrnb", "amrwb",
                               "pcm_f32be", "pcm_f32le", "pcm_f64be", "pcm_f64le",
                               "pcm_s16be", "pcm_s16le", "pcm_s24be", "pcm_s24le",
                               "pcm_s32be", "pcm_s32le", "pcm_s8", "pcm_u8", NULL };
        const audio_encoder_t *encoder = x264_select_audio_encoder( audio_enc, codec_list, &used_enc );
        MP4_FAIL_IF_ERR( !encoder, "unable to select audio encoder.\n" );

        snprintf( audio_params, MAX_ARGS, "%s,codec=%s", audio_parameters, used_enc );
        henc = x264_audio_encoder_open( encoder, filters, audio_params );
    }

    MP4_FAIL_IF_ERR( !henc, "error opening audio encoder.\n" );
    mp4_hnd_t *p_mp4 = handle;
    mp4_audio_hnd_t *p_audio = p_mp4->audio_hnd = calloc( 1, sizeof( mp4_audio_hnd_t ) );
    audio_info_t *info = p_audio->info = x264_audio_encoder_info( henc );
    p_audio->b_copy = copy;

    p_audio->summary = lsmash_create_audio_summary();
    if( !p_audio->summary )
    {
        MP4_LOG_ERROR( "failed to allocate memory for summary information of audio.\n" );
        goto error;
    }

    switch( p_mp4->major_brand )
    {
        case ISOM_BRAND_TYPE_3GP6 :
        case ISOM_BRAND_TYPE_3G2A :
            if( !strcmp( info->codec_name, "amrnb" ) )
                p_audio->codec_type = ISOM_CODEC_TYPE_SAMR_AUDIO;
            else if( !strcmp( info->codec_name, "amrwb" ) )
                p_audio->codec_type = ISOM_CODEC_TYPE_SAWB_AUDIO;
        case ISOM_BRAND_TYPE_MP42 :
            if( !strcmp( info->codec_name, "aac" ) )
            {
                p_audio->codec_type = ISOM_CODEC_TYPE_MP4A_AUDIO;
                audio_aac_info_t *aacinfo = info->opaque;
                if( aacinfo )
                    p_audio->has_sbr = aacinfo->has_sbr;
                else
                    p_audio->has_sbr = 0; // SBR presence isn't specified, so assume implicit signaling
                p_mp4->b_brand_m4a = 1;
            }
            if( !strcmp( info->codec_name, "als" ) )
                p_audio->codec_type = ISOM_CODEC_TYPE_MP4A_AUDIO;
            else if( ( !strcmp( info->codec_name, "mp3" ) || !strcmp( info->codec_name, "mp2" ) || !strcmp( info->codec_name, "mp1" ) )
                     && info->samplerate >= 16000 ) /* freq <16khz is MPEG-2.5. */
                p_audio->codec_type = ISOM_CODEC_TYPE_MP4A_AUDIO;
            else if( !strcmp( info->codec_name, "ac3" ) )
                p_audio->codec_type = ISOM_CODEC_TYPE_AC_3_AUDIO;
            else if( !strcmp( info->codec_name, "alac" ) )
            {
                p_audio->codec_type = ISOM_CODEC_TYPE_ALAC_AUDIO;
                p_mp4->b_brand_m4a = 1;
            }
            break;
        case ISOM_BRAND_TYPE_QT :
            if( !strcmp( info->codec_name, "aac" ) )
            {
                p_audio->codec_type = ISOM_CODEC_TYPE_MP4A_AUDIO;
                audio_aac_info_t *aacinfo = info->opaque;
                if( aacinfo )
                    p_audio->has_sbr = aacinfo->has_sbr;
                else
                    p_audio->has_sbr = 0; // SBR presence isn't specified, so assume implicit signaling
            }
            else if( !strcmp( info->codec_name, "raw" ) )
            {
#ifdef WORDS_BIGENDIAN
                p_audio->codec_type = QT_CODEC_TYPE_TWOS_AUDIO;
                p_audio->summary->endianness    = 0;
#else
                p_audio->codec_type = QT_CODEC_TYPE_SOWT_AUDIO;
                p_audio->summary->endianness    = 1;
#endif
                p_audio->summary->sample_format = 0;
                p_audio->summary->signedness    = 1;
            }
            else if( p_audio->b_copy )
                break;      /* We haven't supported LPCM copying yet. */
            else if( !strcmp( info->codec_name, "pcm_f64be" ) )
            {
                p_audio->codec_type = QT_CODEC_TYPE_FL64_AUDIO;
                p_audio->summary->sample_format = 1;
                p_audio->summary->endianness    = 0;
            }
            else if( !strcmp( info->codec_name, "pcm_f64le" ) )
            {
                p_audio->codec_type = QT_CODEC_TYPE_FL64_AUDIO;
                p_audio->summary->sample_format = 1;
                p_audio->summary->endianness    = 1;
            }
            else if( !strcmp( info->codec_name, "pcm_f32be" ) )
            {
                p_audio->codec_type = QT_CODEC_TYPE_FL32_AUDIO;
                p_audio->summary->sample_format = 1;
                p_audio->summary->endianness    = 0;
            }
            else if( !strcmp( info->codec_name, "pcm_f32le" ) )
            {
                p_audio->codec_type = QT_CODEC_TYPE_FL32_AUDIO;
                p_audio->summary->sample_format = 1;
                p_audio->summary->endianness    = 1;
            }
            else if( !strcmp( info->codec_name, "pcm_s16be" ) || !strcmp( info->codec_name, "pcm_s8" ) )
            {
                p_audio->codec_type = QT_CODEC_TYPE_TWOS_AUDIO;
                p_audio->summary->sample_format = 0;
                p_audio->summary->endianness    = 0;
                p_audio->summary->signedness    = 1;
            }
            else if( !strcmp( info->codec_name, "pcm_s16le" ) )
            {
                p_audio->codec_type = QT_CODEC_TYPE_SOWT_AUDIO;
                p_audio->summary->sample_format = 0;
                p_audio->summary->endianness    = 1;
                p_audio->summary->signedness    = 1;
            }
            else if( !strcmp( info->codec_name, "pcm_s24be" ) )
            {
                p_audio->codec_type = QT_CODEC_TYPE_IN24_AUDIO;
                p_audio->summary->sample_format = 0;
                p_audio->summary->endianness    = 0;
                p_audio->summary->signedness    = 1;
            }
            else if( !strcmp( info->codec_name, "pcm_s24le" ) )
            {
                p_audio->codec_type = QT_CODEC_TYPE_IN24_AUDIO;
                p_audio->summary->sample_format = 0;
                p_audio->summary->endianness    = 1;
                p_audio->summary->signedness    = 1;
            }
            else if( !strcmp( info->codec_name, "pcm_s32be" ) )
            {
                p_audio->codec_type = QT_CODEC_TYPE_IN32_AUDIO;
                p_audio->summary->sample_format = 0;
                p_audio->summary->endianness    = 0;
                p_audio->summary->signedness    = 1;
            }
            else if( !strcmp( info->codec_name, "pcm_s32le" ) )
            {
                p_audio->codec_type = QT_CODEC_TYPE_IN32_AUDIO;
                p_audio->summary->sample_format = 0;
                p_audio->summary->endianness    = 1;
                p_audio->summary->signedness    = 1;
            }
            else if( !strcmp( info->codec_name, "pcm_u8" ) )
            {
                p_audio->codec_type = QT_CODEC_TYPE_RAW_AUDIO;
                p_audio->summary->sample_format = 0;
                p_audio->summary->signedness    = 0;
            }
            break;
        default :
            break;
    }

    if( !p_audio->codec_type )
    {
        MP4_LOG_ERROR( "unsupported audio codec '%s'.\n", info->codec_name );
        goto error;
    }

    p_audio->encoder = henc;

    return 1;

error:
    x264_audio_encoder_close( henc );
    free( p_mp4->audio_hnd );
    p_mp4->audio_hnd = NULL;

    return -1;
}
#endif /* #if HAVE_AUDIO */

#if HAVE_ANY_AUDIO
static int set_param_audio( mp4_hnd_t* p_mp4, uint64_t i_media_timescale, lsmash_track_mode_code track_mode )
{
    mp4_audio_hnd_t *p_audio = p_mp4->audio_hnd;

    /* Create a audio track. */
    p_audio->i_track = lsmash_create_track( p_mp4->p_root, ISOM_MEDIA_HANDLER_TYPE_AUDIO_TRACK );
    MP4_FAIL_IF_ERR( !p_audio->i_track, "failed to create a audio track.\n" );

#if HAVE_AUDIO
    if( p_mp4->major_brand == ISOM_BRAND_TYPE_QT )
        set_channel_layout( p_audio );
    p_audio->summary->stream_type      = MP4SYS_STREAM_TYPE_AudioStream;
    p_audio->summary->max_au_length    = ( 1 << 13 ) - 1;
    p_audio->summary->frequency        = p_audio->info->samplerate;
    p_audio->summary->channels         = p_audio->info->channels;
    p_audio->summary->bit_depth        = p_audio->info->depth;
    p_audio->summary->samples_in_frame = p_audio->info->framelen;
    p_audio->summary->packed           = 1;
    p_audio->summary->interleaved      = 1;
    switch( p_audio->codec_type )
    {
        case ISOM_CODEC_TYPE_MP4A_AUDIO :
            p_audio->summary->object_type_indication = MP4SYS_OBJECT_TYPE_Audio_ISO_14496_3;
            if( !strcmp( p_audio->info->codec_name, "mp3" ) || !strcmp( p_audio->info->codec_name, "mp2" ) || !strcmp( p_audio->info->codec_name, "mp1" ) )
            {
                if( p_audio->info->samplerate >= 32000 )
                    p_audio->summary->object_type_indication = MP4SYS_OBJECT_TYPE_Audio_ISO_11172_3; /* Legacy Interface */
                else
                    p_audio->summary->object_type_indication = MP4SYS_OBJECT_TYPE_Audio_ISO_13818_3; /* Legacy Interface */
            }
            if( !strcmp( p_audio->info->codec_name, "als" ) )
                p_audio->summary->aot = MP4A_AUDIO_OBJECT_TYPE_ALS;
            else
                p_audio->summary->aot = MP4A_AUDIO_OBJECT_TYPE_AAC_LC;
            p_audio->summary->sbr_mode      = p_audio->has_sbr ? MP4A_AAC_SBR_BACKWARD_COMPATIBLE : MP4A_AAC_SBR_NOT_SPECIFIED;
            MP4_FAIL_IF_ERR( lsmash_summary_add_exdata( p_audio->summary, p_audio->info->extradata, p_audio->info->extradata_size ),
                             "failed to create mp4a specific info.\n" );
            break;
        case ISOM_CODEC_TYPE_AC_3_AUDIO :
            p_audio->summary->object_type_indication = MP4SYS_OBJECT_TYPE_AC_3_AUDIO;
            MP4_FAIL_IF_ERR( mp4sys_create_dac3_from_syncframe( p_audio->summary, p_audio->info->extradata, p_audio->info->extradata_size ),
                             "failed to create AC-3 specific info.\n" );
            break;
        case ISOM_CODEC_TYPE_SAMR_AUDIO :
        case ISOM_CODEC_TYPE_SAWB_AUDIO :
            p_audio->summary->object_type_indication = MP4SYS_OBJECT_TYPE_NONE;
            MP4_FAIL_IF_ERR( mp4sys_amr_create_damr( p_audio->summary ),
                             "failed to create AMR specific info.\n" );
            break;
        default :
            p_audio->summary->object_type_indication = MP4SYS_OBJECT_TYPE_NONE;
            MP4_FAIL_IF_ERR( lsmash_summary_add_exdata( p_audio->summary, p_audio->info->extradata, p_audio->info->extradata_size ),
                             "failed to create unknown specific info.\n" );
            break;
    }
#else
    /*
     * NOTE: Retrieve audio summary, which will be used for lsmash_add_sample_entry() as audio parameters.
     * Currently, our ADTS importer never recognize SBR (HE-AAC).
     * Thus, in this sample, if the source contains SBR, it'll be coded as implicit signaling for SBR.
     * If you want to use explicit signaling of SBR, change the sbr_mode in summary and
     * call lsmash_setup_AudioSpecificConfig() to reconstruct ASC within the summary.
     */
    /*
     * WARNING: If you wish to allocate summary in your code, you have to allocate ASC too,
     * and never use lsmash_cleanup_audio_summary(), unless L-SMASH is compiled integrated with your code.
     * Because malloc() and free() have to be used as pair from EXACTLY SAME standard C library.
     * Otherwise you may cause bugs which you hardly call to mind.
     */
    p_audio->summary = mp4sys_duplicate_audio_summary( p_audio->p_importer, 1 );
    MP4_FAIL_IF_ERR( !p_audio->summary, "failed to duplicate summary information.\n" );
    p_audio->codec_type = p_audio->summary->sample_type;
#endif /* #if HAVE_AUDIO #else */

    /* Set sound track parameters. */
    lsmash_track_parameters_t track_param;
    lsmash_initialize_track_parameters( &track_param );
    track_param.mode = track_mode;
    MP4_FAIL_IF_ERR( lsmash_set_track_parameters( p_mp4->p_root, p_audio->i_track, &track_param ),
                     "failed to set track parameters for audio.\n" );
    p_audio->i_video_timescale = i_media_timescale;

    /* Set sound media parameters. */
    lsmash_media_parameters_t media_param;
    lsmash_initialize_media_parameters( &media_param );
    media_param.timescale = p_audio->summary->frequency;
    media_param.ISO_language = p_mp4->psz_language;
    media_param.media_handler_name = "X264 Sound Media Handler";
    if( p_mp4->b_brand_qt )
        media_param.data_handler_name = "X264 URL Data Handler";
    MP4_FAIL_IF_ERR( lsmash_set_media_parameters( p_mp4->p_root, p_audio->i_track, &media_param ),
                     "failed to set media parameters for audio.\n" );

    p_audio->i_sample_entry = lsmash_add_sample_entry( p_mp4->p_root, p_audio->i_track, p_audio->codec_type, p_audio->summary );
    MP4_FAIL_IF_ERR( !p_audio->i_sample_entry,
                     "failed to add sample_entry for audio.\n" );
    /* MP4AudioSampleEntry does not have btrt */
//    MP4_FAIL_IF_ERR( lsmash_add_btrt( p_mp4->p_root, p_audio->i_track, p_audio->i_sample_entry ),
//                     "failed to add btrt for audio.\n" );

    return 0;
}

static int write_audio_frames( mp4_hnd_t *p_mp4, double video_dts, int finish )
{
    mp4_audio_hnd_t *p_audio = p_mp4->audio_hnd;
    assert( p_audio );

#if HAVE_AUDIO
    audio_packet_t *frame;
#endif

    /* FIXME: This is just a sample implementation. */
    for(;;)
    {
        uint64_t audio_timestamp = (uint64_t)p_audio->i_numframe * p_audio->summary->samples_in_frame;
        /*
         * means while( audio_dts <= video_dts )
         * FIXME: I wonder if there's any way more effective.
         */
        if( !finish && ((audio_timestamp / (double)p_audio->summary->frequency > video_dts) || !video_dts) )
            break;

        /* read a audio frame */
#if HAVE_AUDIO
        if( finish )
            frame = x264_audio_encoder_finish( p_audio->encoder );
        else if( !(frame = x264_audio_encode_frame( p_audio->encoder )) )
        {
            finish = 1;
            continue;
        }

        if( !frame )
            break;

        lsmash_sample_t *p_sample = lsmash_create_sample( frame->size );
        MP4_FAIL_IF_ERR( !p_sample,
                         "failed to create a audio sample data.\n" );
        memcpy( p_sample->data, frame->data, frame->size );
        x264_audio_free_frame( p_audio->encoder, frame );
#else
        /* FIXME: mp4sys_importer_get_access_unit() returns 1 if there're any changes in stream's properties.
           If you want to support them, you have to retrieve summary again, and make some operation accordingly. */
        lsmash_sample_t *p_sample = lsmash_create_sample( p_audio->summary->max_au_length );
        MP4_FAIL_IF_ERR( !p_sample,
                         "failed to create a audio sample data.\n" );
        MP4_FAIL_IF_ERR( mp4sys_importer_get_access_unit( p_audio->p_importer, 1, p_sample->data, &p_sample->length ),
                         "failed to retrieve frame data from importer.\n" );
        if( p_sample->length == 0 )
            break; /* end of stream */
#endif
        p_sample->dts = p_sample->cts = audio_timestamp;
        p_sample->prop.random_access_type = ISOM_SAMPLE_RANDOM_ACCESS_TYPE_SYNC;
        p_sample->index = p_audio->i_sample_entry;
        MP4_FAIL_IF_ERR( lsmash_append_sample( p_mp4->p_root, p_audio->i_track, p_sample ),
                         "failed to append a audio sample.\n" );

        p_audio->i_numframe++;
    }
    return 0;
}

static int close_file_audio( mp4_hnd_t* p_mp4, double actual_duration, uint32_t movie_timescale )
{
    mp4_audio_hnd_t *p_audio = p_mp4->audio_hnd;
    MP4_LOG_IF_ERR( ( write_audio_frames( p_mp4, actual_duration / movie_timescale, 0 ) || // FIXME: I wonder why is this needed?
                      write_audio_frames( p_mp4, 0, 1 ) ),
                    "failed to flush audio frame(s).\n" );
    uint32_t last_delta;
    if( p_audio->codec_type == QT_CODEC_TYPE_SOWT_AUDIO || p_audio->codec_type == QT_CODEC_TYPE_TWOS_AUDIO )
        last_delta = 1;
    else
#if HAVE_AUDIO
        last_delta = p_audio->info->last_delta;
#else
        last_delta = p_audio->summary->samples_in_frame;
#endif
    MP4_LOG_IF_ERR( lsmash_flush_pooled_samples( p_mp4->p_root, p_audio->i_track, last_delta ),
                    "failed to set last sample's duration for audio.\n" );
    if( !p_mp4->b_fragments )
        MP4_LOG_IF_ERR( lsmash_create_explicit_timeline_map( p_mp4->p_root, p_audio->i_track, 0, 0, ISOM_EDIT_MODE_NORMAL ),
                        "failed to set timeline map for audio.\n" );
    return 0;
}
#endif /* #if HAVE_ANY_AUDIO */

int remux_callback( void* param, uint64_t done, uint64_t total )
{
    int64_t elapsed = x264_mdate() - *(int64_t*)param;
    double byterate = done / ( elapsed / 1000000. );
    fprintf( stderr, "remux [%5.2lf%%], %"PRIu64"/%"PRIu64" KiB, %u KiB/s, ",
        done*100./total, done/1024, total/1024, (unsigned)byterate/1024 );
    if( done == total )
    {
        unsigned sec = (unsigned)( elapsed / 1000000 );
        fprintf( stderr, "total elapsed %u:%02u:%02u\n\n", sec/3600, (sec/60)%60, sec%60 );
    }
    else
    {
        unsigned eta = (unsigned)( (total - done) / byterate );
        fprintf( stderr, "eta %u:%02u:%02u\r", eta/3600, (eta/60)%60, eta%60 );
    }
    fflush( stderr ); // needed in windows
    return 0;
}

/*******************/

static int close_file( hnd_t handle, int64_t largest_pts, int64_t second_largest_pts )
{
    mp4_hnd_t *p_mp4 = handle;

    if( !p_mp4 )
        return 0;

    if( p_mp4->p_root )
    {
        double actual_duration   = 0;   /* FIXME: This may be inside block of "if( p_mp4->i_track )" if audio does not use this. */
        uint32_t movie_timescale = 1;   /* FIXME: This may be inside block of "if( p_mp4->i_track )" if audio does not use this.
                                         *        And to avoid devision by zero, use 1 as default. */
        if( p_mp4->i_track )
        {
            /* Flush the rest of samples and add the last sample_delta. */
            uint32_t last_delta = largest_pts - second_largest_pts;
            MP4_LOG_IF_ERR( lsmash_flush_pooled_samples( p_mp4->p_root, p_mp4->i_track, (last_delta ? last_delta : 1) * p_mp4->i_time_inc ),
                            "failed to flush the rest of samples.\n" );

                     movie_timescale = lsmash_get_movie_timescale( p_mp4->p_root );
            uint32_t media_timescale = lsmash_get_media_timescale( p_mp4->p_root, p_mp4->i_track );
            if( media_timescale != 0 )  /* avoid zero division */
                actual_duration = (double)((largest_pts + last_delta) * p_mp4->i_time_inc) * movie_timescale / media_timescale;
            else
                MP4_LOG_ERROR( "media timescale is broken.\n" );

            if( !p_mp4->b_fragments )
            {
                /*
                 * Declare the explicit time-line mapping.
                 * A segment_duration is given by movie timescale, while a media_time that is the start time of this segment
                 * is given by not the movie timescale but rather the media timescale.
                 * The reason is that ISO media have two time-lines, presentation and media time-line,
                 * and an edit maps the presentation time-line to the media time-line.
                 * According to QuickTime file format specification and the actual playback in QuickTime Player,
                 * if the Edit Box doesn't exist in the track, the ratio of the summation of sample durations and track's duration becomes
                 * the track's media_rate so that the entire media can be used by the track.
                 * So, we add Edit Box here to avoid this implicit media_rate could distort track's presentation timestamps slightly.
                 * Note: Any demuxers should follow the Edit List Box if it exists.
                 */
                int64_t first_cts = p_mp4->b_dts_compress ? 0 : p_mp4->i_start_offset * p_mp4->i_time_inc;
                MP4_LOG_IF_ERR( lsmash_create_explicit_timeline_map( p_mp4->p_root, p_mp4->i_track, actual_duration, first_cts, ISOM_EDIT_MODE_NORMAL ),
                                "failed to set timeline map for video.\n" );
            }
        }

#if HAVE_ANY_AUDIO
        MP4_LOG_IF_ERR( p_mp4->audio_hnd && p_mp4->audio_hnd->i_track && close_file_audio( p_mp4, actual_duration, movie_timescale ),
                        "failed to close audio.\n" );
#endif

        if( p_mp4->psz_chapter && (p_mp4->major_brand != ISOM_BRAND_TYPE_QT) )
            MP4_LOG_IF_ERR( lsmash_set_tyrant_chapter( p_mp4->p_root, p_mp4->psz_chapter ), "failed to set chapter list.\n" );

        if( !p_mp4->b_no_remux )
        {
            int64_t start = x264_mdate();
            lsmash_adhoc_remux_t remux_info;
            remux_info.func = remux_callback;
            remux_info.buffer_size = 4*1024*1024; // 4MiB
            remux_info.param = &start;
            MP4_LOG_IF_ERR( lsmash_finish_movie( p_mp4->p_root, &remux_info ), "failed to finish movie.\n" );
        }
        else
            MP4_LOG_IF_ERR( lsmash_finish_movie( p_mp4->p_root, NULL ), "failed to finish movie.\n" );
    }

    remove_mp4_hnd( p_mp4 ); /* including lsmash_destroy_root( p_mp4->p_root ); */

    return 0;
}

static int open_file( char *psz_filename, hnd_t *p_handle, cli_output_opt_t *opt, hnd_t audio_filters, char *audio_enc, char *audio_params )
{
    mp4_hnd_t *p_mp4;

    *p_handle = NULL;
    FILE *fh = fopen( psz_filename, "wb" );
    MP4_FAIL_IF_ERR( !fh, "cannot open output file `%s'.\n", psz_filename );
    if( !x264_is_regular_file( fh ) )
    {
        fclose( fh );
        MP4_FAIL_IF_ERR( -1, "MP4 output is incompatible with non-regular file `%s'\n", psz_filename );
    }
    fclose( fh );

    p_mp4 = malloc( sizeof(mp4_hnd_t) );
    MP4_FAIL_IF_ERR( !p_mp4, "failed to allocate memory for muxer information.\n" );
    memset( p_mp4, 0, sizeof(mp4_hnd_t) );

    p_mp4->b_dts_compress = opt->use_dts_compress;

    if( opt->mux_mov )
    {
        p_mp4->major_brand = ISOM_BRAND_TYPE_QT;
        p_mp4->b_brand_qt = 1;
    }
    else if( opt->mux_3gp )
    {
        p_mp4->major_brand = ISOM_BRAND_TYPE_3GP6;
        p_mp4->i_brand_3gpp = 1;
    }
    else if( opt->mux_3g2 )
    {
        p_mp4->major_brand = ISOM_BRAND_TYPE_3G2A;
        p_mp4->i_brand_3gpp = 2;
    }
    else
        p_mp4->major_brand = ISOM_BRAND_TYPE_MP42;

    if( opt->chapter )
    {
        p_mp4->psz_chapter = opt->chapter;
        fh = fopen( p_mp4->psz_chapter, "rb" );
        MP4_FAIL_IF_ERR_EX( !fh, "can't open `%s'\n", p_mp4->psz_chapter );
        fclose( fh );
    }
    p_mp4->psz_language = opt->language;
    p_mp4->b_no_pasp = opt->no_sar;
    p_mp4->b_no_remux = opt->no_remux;
    p_mp4->i_display_width = opt->display_width * (1<<16);
    p_mp4->i_display_height = opt->display_height * (1<<16);
    p_mp4->b_force_display_size = p_mp4->i_display_height || p_mp4->i_display_height;
    p_mp4->scaling_method = p_mp4->b_force_display_size ? ISOM_SCALING_METHOD_FILL : ISOM_SCALING_METHOD_MEET;
    p_mp4->b_fragments = opt->fragments;

    p_mp4->p_root = lsmash_open_movie( psz_filename, p_mp4->b_fragments ? LSMASH_FILE_MODE_WRITE_FRAGMENTED : LSMASH_FILE_MODE_WRITE );
    MP4_FAIL_IF_ERR_EX( !p_mp4->p_root, "failed to create root.\n" );

    p_mp4->summary = calloc( 1, sizeof(lsmash_video_summary_t) );
    MP4_FAIL_IF_ERR_EX( !p_mp4->summary,
                        "failed to allocate memory for summary information of video.\n" );

#if HAVE_ANY_AUDIO
#if HAVE_AUDIO
    MP4_FAIL_IF_ERR_EX( audio_init( p_mp4, audio_filters, audio_enc, audio_params ) < 0, "unable to init audio output.\n" );
#else
    mp4_audio_hnd_t *p_audio = p_mp4->audio_hnd = (mp4_audio_hnd_t *)malloc( sizeof(mp4_audio_hnd_t) );
    MP4_FAIL_IF_ERR_EX( !p_audio, "failed to allocate memory for audio muxing information.\n" );
    memset( p_audio, 0, sizeof(mp4_audio_hnd_t) );
    p_audio->p_importer = mp4sys_importer_open( "x264_audio_test.adts", "auto" );
    if( !p_audio->p_importer )
    {
        free( p_audio );
        p_mp4->audio_hnd = NULL;
    }
#endif
    if( !p_mp4->audio_hnd )
        MP4_LOG_INFO( "audio muxing feature is disabled.\n" );
#endif

    *p_handle = p_mp4;

    return 0;
}

static int set_param( hnd_t handle, x264_param_t *p_param )
{
    mp4_hnd_t *p_mp4 = handle;
    uint64_t i_media_timescale;

    set_recovery_param( p_mp4, p_param );

    p_mp4->i_delay_frames = p_param->i_bframe ? (p_param->i_bframe_pyramid ? 2 : 1) : 0;
    p_mp4->i_dts_compress_multiplier = p_mp4->b_dts_compress * p_mp4->i_delay_frames + 1;

    i_media_timescale = p_param->i_timebase_den * p_mp4->i_dts_compress_multiplier;
    p_mp4->i_time_inc = p_param->i_timebase_num * p_mp4->i_dts_compress_multiplier;
    FAIL_IF_ERR( i_media_timescale > UINT32_MAX, "mp4", "MP4 media timescale %"PRIu64" exceeds maximum\n", i_media_timescale );

    /* Set brands. */
    lsmash_brand_type_code brands[10] = { 0 };
    uint32_t minor_version = 0;
    uint32_t brand_count = 0;
    if( p_mp4->b_brand_qt )
    {
        brands[brand_count++] = ISOM_BRAND_TYPE_QT;
        p_mp4->i_brand_3gpp = 0;
        p_mp4->b_brand_m4a = 0;
        p_mp4->b_use_recovery = 0;      /* Disable sample grouping. */
    }
    else
    {
        if( p_mp4->i_brand_3gpp >= 1 )
            brands[brand_count++] = ISOM_BRAND_TYPE_3GP6;
        if( p_mp4->i_brand_3gpp == 2 )
        {
            brands[brand_count++] = ISOM_BRAND_TYPE_3G2A;
            minor_version = 0x00010000;
        }
        brands[brand_count++] = ISOM_BRAND_TYPE_MP42;
        brands[brand_count++] = ISOM_BRAND_TYPE_MP41;
        if( p_mp4->b_brand_m4a )
        {
            brands[brand_count++] = ISOM_BRAND_TYPE_M4V;
            brands[brand_count++] = ISOM_BRAND_TYPE_M4A;
        }
        brands[brand_count++] = ISOM_BRAND_TYPE_ISOM;
        if( p_mp4->b_use_recovery )
        {
            brands[brand_count++] = ISOM_BRAND_TYPE_AVC1;   /* sdtp/sgpd/sbgp/random access recovery point grouping */
            if( p_param->b_open_gop )
            {
                brands[brand_count++] = ISOM_BRAND_TYPE_ISO6;   /* cslg/random access point grouping */
                brands[brand_count++] = ISOM_BRAND_TYPE_QT;     /* tapt/cslg/stps/sdtp */
                p_mp4->b_brand_qt = 1;
            }
        }
    }
    MP4_FAIL_IF_ERR( lsmash_set_brands( p_mp4->p_root, p_mp4->major_brand, minor_version, brands, brand_count ),
                     "failed to set brands / ftyp.\n" );

    /* Set movie parameters. */
    lsmash_movie_parameters_t movie_param;
    lsmash_initialize_movie_parameters( &movie_param );
    MP4_FAIL_IF_ERR( lsmash_set_movie_parameters( p_mp4->p_root, &movie_param ),
                     "failed to set movie parameters.\n" );

    /* Create a video track. */
    p_mp4->i_track = lsmash_create_track( p_mp4->p_root, ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK );
    MP4_FAIL_IF_ERR( !p_mp4->i_track, "failed to create a video track.\n" );

    p_mp4->summary->width = p_param->i_width;
    p_mp4->summary->height = p_param->i_height;
    if( !p_mp4->b_force_display_size )
    {
        p_mp4->i_display_width = p_param->i_width << 16;
        p_mp4->i_display_height = p_param->i_height << 16;
    }
    if( p_param->vui.i_sar_width && p_param->vui.i_sar_height )
    {
        if( !p_mp4->b_force_display_size )
        {
            double sar = (double)p_param->vui.i_sar_width / p_param->vui.i_sar_height;
            if( sar > 1.0 )
                p_mp4->i_display_width *= sar;
            else
                p_mp4->i_display_height /= sar;
        }
        if( !p_mp4->b_no_pasp )
        {
            p_mp4->summary->par_h = p_param->vui.i_sar_width;
            p_mp4->summary->par_v = p_param->vui.i_sar_height;
            if( p_mp4->major_brand != ISOM_BRAND_TYPE_QT )
                p_mp4->summary->scaling_method = p_mp4->scaling_method;
        }
    }
    if( p_mp4->b_brand_qt )
    {
        p_mp4->summary->primaries = p_param->vui.i_colorprim;
        p_mp4->summary->transfer = p_param->vui.i_transfer;
        p_mp4->summary->matrix = p_param->vui.i_colmatrix;
    }

    /* Set video track parameters. */
    lsmash_track_parameters_t track_param;
    lsmash_initialize_track_parameters( &track_param );
    lsmash_track_mode_code track_mode = ISOM_TRACK_ENABLED | ISOM_TRACK_IN_MOVIE | ISOM_TRACK_IN_PREVIEW;
    if( p_mp4->b_brand_qt )
        track_mode |= QT_TRACK_IN_POSTER;
    track_param.mode = track_mode;
    track_param.display_width = p_mp4->i_display_width;
    track_param.display_height = p_mp4->i_display_height;
    MP4_FAIL_IF_ERR( lsmash_set_track_parameters( p_mp4->p_root, p_mp4->i_track, &track_param ),
                     "failed to set track parameters for video.\n" );

    /* Set video media parameters. */
    lsmash_media_parameters_t media_param;
    lsmash_initialize_media_parameters( &media_param );
    media_param.timescale = i_media_timescale;
    media_param.ISO_language = p_mp4->psz_language;
    media_param.media_handler_name = "X264 Video Media Handler";
    if( p_mp4->b_brand_qt )
        media_param.data_handler_name = "X264 URL Data Handler";
    MP4_FAIL_IF_ERR( lsmash_set_media_parameters( p_mp4->p_root, p_mp4->i_track, &media_param ),
                     "failed to set media parameters for video.\n" );

    /* Add a sample entry. */
    p_mp4->i_sample_entry = lsmash_add_sample_entry( p_mp4->p_root, p_mp4->i_track, ISOM_CODEC_TYPE_AVC1_VIDEO, p_mp4->summary );
    MP4_FAIL_IF_ERR( !p_mp4->i_sample_entry,
                     "failed to add sample entry for video.\n" );

    if( p_mp4->major_brand != ISOM_BRAND_TYPE_QT )
        MP4_FAIL_IF_ERR( lsmash_add_btrt( p_mp4->p_root, p_mp4->i_track, p_mp4->i_sample_entry ),
                         "failed to add btrt.\n" );
    if( p_mp4->b_brand_qt && !p_mp4->b_no_pasp )
        MP4_FAIL_IF_ERR( lsmash_set_track_aperture_modes( p_mp4->p_root, p_mp4->i_track, p_mp4->i_sample_entry ),
                         "failed to set track aperture mode.\n" );
    if( p_mp4->major_brand != ISOM_BRAND_TYPE_QT )
    {
        if( p_param->b_intra_refresh )
            MP4_FAIL_IF_ERR( lsmash_create_grouping( p_mp4->p_root, p_mp4->i_track, ISOM_GROUP_TYPE_ROLL ),
                             "failed to create random access recovery point sample grouping\n" );
        if( p_param->b_open_gop )
            MP4_FAIL_IF_ERR( lsmash_create_grouping( p_mp4->p_root, p_mp4->i_track, ISOM_GROUP_TYPE_RAP ),
                             "failed to create random access point sample grouping\n" );
    }

#if HAVE_ANY_AUDIO
    MP4_FAIL_IF_ERR( p_mp4->audio_hnd && set_param_audio( p_mp4, i_media_timescale, track_mode ),
                     "failed to set audio param\n" );
#endif

    return 0;
}

static int write_headers( hnd_t handle, x264_nal_t *p_nal )
{
    mp4_hnd_t *p_mp4 = handle;

    uint32_t sps_size = p_nal[0].i_payload - 4;
    uint32_t pps_size = p_nal[1].i_payload - 4;
    uint32_t sei_size = p_nal[2].i_payload;

    uint8_t *sps = p_nal[0].p_payload + 4;
    uint8_t *pps = p_nal[1].p_payload + 4;
    uint8_t *sei = p_nal[2].p_payload;

    MP4_FAIL_IF_ERR( lsmash_set_avc_config( p_mp4->p_root, p_mp4->i_track, p_mp4->i_sample_entry, 1, sps[1], sps[2], sps[3], 3, 1, BIT_DEPTH-8, BIT_DEPTH-8 ),
                     "failed to set avc config.\n" );
    /* SPS */
    MP4_FAIL_IF_ERR( lsmash_add_sps_entry( p_mp4->p_root, p_mp4->i_track, p_mp4->i_sample_entry, sps, sps_size ),
                     "failed to add sps.\n" );
    /* PPS */
    MP4_FAIL_IF_ERR( lsmash_add_pps_entry( p_mp4->p_root, p_mp4->i_track, p_mp4->i_sample_entry, pps, pps_size ),
                     "failed to add pps.\n" );
    /* SEI */
    p_mp4->p_sei_buffer = malloc( sei_size );
    MP4_FAIL_IF_ERR( !p_mp4->p_sei_buffer,
                     "failed to allocate sei transition buffer.\n" );
    memcpy( p_mp4->p_sei_buffer, sei, sei_size );
    p_mp4->i_sei_size = sei_size;

    /* Write ftyp. */
    MP4_FAIL_IF_ERR( lsmash_write_ftyp( p_mp4->p_root ),
                     "failed to write brands / ftyp.\n" );

    return sei_size + sps_size + pps_size;
}

static int write_frame( hnd_t handle, uint8_t *p_nalu, int i_size, x264_picture_t *p_picture )
{
    mp4_hnd_t *p_mp4 = handle;
    uint64_t dts, cts;

    if( !p_mp4->i_numframe )
    {
        p_mp4->i_start_offset = p_picture->i_dts * -1;
        if( p_mp4->psz_chapter && (p_mp4->b_brand_qt || p_mp4->b_brand_m4a) )
            MP4_FAIL_IF_ERR( lsmash_create_reference_chapter_track( p_mp4->p_root, p_mp4->i_track, p_mp4->psz_chapter ),
                             "failed to create reference chapter track.\n" );
    }

    lsmash_sample_t *p_sample = lsmash_create_sample( i_size + p_mp4->i_sei_size );
    MP4_FAIL_IF_ERR( !p_sample,
                     "failed to create a video sample data.\n" );

    if( p_mp4->p_sei_buffer )
    {
        memcpy( p_sample->data, p_mp4->p_sei_buffer, p_mp4->i_sei_size );
        free( p_mp4->p_sei_buffer );
        p_mp4->p_sei_buffer = NULL;
    }

    memcpy( p_sample->data + p_mp4->i_sei_size, p_nalu, i_size );
    p_mp4->i_sei_size = 0;

    if( p_mp4->b_dts_compress )
    {
        if( p_mp4->i_numframe == 1 )
            p_mp4->i_init_delta = (p_picture->i_dts + p_mp4->i_start_offset) * p_mp4->i_time_inc;
        dts = p_mp4->i_numframe > p_mp4->i_delay_frames
            ? p_picture->i_dts * p_mp4->i_time_inc
            : p_mp4->i_numframe * (p_mp4->i_init_delta / p_mp4->i_dts_compress_multiplier);
        cts = p_picture->i_pts * p_mp4->i_time_inc;
    }
    else
    {
        dts = (p_picture->i_dts + p_mp4->i_start_offset) * p_mp4->i_time_inc;
        cts = (p_picture->i_pts + p_mp4->i_start_offset) * p_mp4->i_time_inc;
    }

    p_sample->dts = dts;
    p_sample->cts = cts;
    p_sample->index = p_mp4->i_sample_entry;
    p_sample->prop.random_access_type = p_picture->i_type == X264_TYPE_IDR ? ISOM_SAMPLE_RANDOM_ACCESS_TYPE_CLOSED_RAP : ISOM_SAMPLE_RANDOM_ACCESS_TYPE_NONE;
    if( p_mp4->b_use_recovery || p_mp4->b_brand_qt )
    {
        p_sample->prop.independent = IS_X264_TYPE_I( p_picture->i_type ) ? ISOM_SAMPLE_IS_INDEPENDENT : ISOM_SAMPLE_IS_NOT_INDEPENDENT;
        p_sample->prop.disposable = p_picture->i_type == X264_TYPE_B ? ISOM_SAMPLE_IS_DISPOSABLE : ISOM_SAMPLE_IS_NOT_DISPOSABLE;
        p_sample->prop.redundant = ISOM_SAMPLE_HAS_NO_REDUNDANCY;
        if( p_mp4->b_use_recovery )
        {
            p_sample->prop.leading = !IS_X264_TYPE_B( p_picture->i_type ) || p_sample->cts >= p_mp4->i_last_intra_cts
                                   ? ISOM_SAMPLE_IS_NOT_LEADING : ISOM_SAMPLE_IS_UNDECODABLE_LEADING;
            if( p_sample->prop.independent == ISOM_SAMPLE_IS_INDEPENDENT )
                p_mp4->i_last_intra_cts = p_sample->cts;
            p_sample->prop.recovery.identifier = p_picture->i_frame_num % p_mp4->i_max_frame_num;
            if( p_picture->b_keyframe && p_picture->i_type != X264_TYPE_IDR )
            {
                /* A picture with Recovery Point SEI */
                p_sample->prop.random_access_type = ISOM_SAMPLE_RANDOM_ACCESS_TYPE_RECOVERY;
                p_sample->prop.recovery.complete = (p_sample->prop.recovery.identifier + p_mp4->i_recovery_frame_cnt) % p_mp4->i_max_frame_num;
            }
        }
        else if( p_picture->i_type != X264_TYPE_IDR && p_picture->i_type != X264_TYPE_B )
            if( p_picture->i_type == X264_TYPE_I || p_picture->i_type == X264_TYPE_P || p_picture->i_type == X264_TYPE_BREF )
                p_sample->prop.allow_earlier = QT_SAMPLE_EARLIER_PTS_ALLOWED;
        if( p_picture->i_type == X264_TYPE_I && p_picture->b_keyframe && p_mp4->i_recovery_frame_cnt == 0 )
            p_sample->prop.random_access_type = ISOM_SAMPLE_RANDOM_ACCESS_TYPE_OPEN_RAP;
    }

    x264_cli_log( "mp4", X264_LOG_DEBUG, "coded: %d, frame_num: %d, key: %s, type: %s, independ: %s, dispose: %s, lead: %s\n",
                  p_mp4->i_numframe, p_picture->i_frame_num, p_picture->b_keyframe ? "yes" : "no",
                  p_picture->i_type == X264_TYPE_P ? "P" : p_picture->i_type == X264_TYPE_B ? "b" :
                  p_picture->i_type == X264_TYPE_BREF ? "B" : p_picture->i_type == X264_TYPE_IDR ? "I" :
                  p_picture->i_type == X264_TYPE_I ? "i" : p_picture->i_type == X264_TYPE_KEYFRAME ? "K" : "N",
                  p_sample->prop.independent == ISOM_SAMPLE_IS_INDEPENDENT ? "yes" : "no",
                  p_sample->prop.disposable == ISOM_SAMPLE_IS_DISPOSABLE ? "yes" : "no",
                  p_sample->prop.leading == ISOM_SAMPLE_IS_UNDECODABLE_LEADING || p_sample->prop.leading == ISOM_SAMPLE_IS_DECODABLE_LEADING ? "yes" : "no" );

#if HAVE_ANY_AUDIO
    mp4_audio_hnd_t *p_audio = p_mp4->audio_hnd;
    if( p_audio )
        if( write_audio_frames( p_mp4, p_sample->dts / (double)p_audio->i_video_timescale, 0 ) )
            return -1;
#endif

    if( p_mp4->b_fragments && p_mp4->i_numframe && p_sample->prop.random_access_type )
    {
        MP4_FAIL_IF_ERR( lsmash_flush_pooled_samples( p_mp4->p_root, p_mp4->i_track, p_sample->dts - p_mp4->prev_dts ),
                         "failed to flush the rest of samples.\n" );
#if HAVE_ANY_AUDIO
        if( p_audio )
            MP4_FAIL_IF_ERR( lsmash_flush_pooled_samples( p_mp4->p_root, p_audio->i_track, p_audio->summary->samples_in_frame ),
                             "failed to flush the rest of samples for audio.\n" );
#endif
        MP4_FAIL_IF_ERR( lsmash_create_fragment_movie( p_mp4->p_root ),
                         "failed to create a movie fragment.\n" );
    }

    /* Append data per sample. */
    MP4_FAIL_IF_ERR( lsmash_append_sample( p_mp4->p_root, p_mp4->i_track, p_sample ),
                     "failed to append a video frame.\n" );

    p_mp4->prev_dts = dts;
    p_mp4->i_numframe++;

    return i_size;
}

const cli_output_t mp4_output = { open_file, set_param, write_headers, write_frame, close_file };
