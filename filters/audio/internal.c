#include "filters/audio/internal.h"
#include <stdint.h>
#include <math.h>

float **af_get_buffer( unsigned channels, unsigned samplecount )
{
    float **samples = malloc( sizeof( float* ) * channels );
    for( int i = 0; i < channels; i++ ) {
        samples[i] = malloc( sizeof( float ) * samplecount );
    }
    return samples;
}

int af_resize_buffer( float **buffer, unsigned channels, unsigned samplecount )
{
    for( int c = 0; c < channels; c++ )
    {
        if( !(buffer[c] = realloc( buffer[c], sizeof( float ) * samplecount )) )
            return -1;
    }
    return 0;
}

float **af_dup_buffer( float **buffer, unsigned channels, unsigned samplecount )
{
    float **buf = af_get_buffer( channels, samplecount );
    for( int c = 0; c < channels; c++ )
        memcpy( buf[c], buffer[c], samplecount );
    return buf;
}

void af_free_buffer( float **buffer, unsigned channels )
{
    if( !buffer )
        return;
    for( int c = 0; c < channels; c++ )
        free( buffer[c] );
    free( buffer );
}

int af_cat_buffer( float **buf, unsigned bufsamples, float **in, unsigned insamples, unsigned channels )
{
    if( af_resize_buffer( buf, channels, bufsamples + insamples ) < 0 )
        return -1;
    for( int c = 0; c < channels; c++ )
        for( int s = 0; s < insamples; s++ )
            buf[c][bufsamples+s] = in[c][s];
    return 0;
}

float **af_deinterleave ( float *input, unsigned channels, unsigned samplecount )
{
    float **deint = af_get_buffer( channels, samplecount );
    for( int s = 0; s < samplecount; s++ )
        for( int c = 0; c < channels; c++ )
            deint[c][s] = input[s*channels + c];
    return deint;
}

float *af_interleave ( float **input, unsigned channels, unsigned samplecount )
{
    float *inter = malloc( sizeof( float ) * channels * samplecount );
    for( int c = 0; c < channels; c++ )
        for( int s = 0; s < samplecount; s++ )
            inter[s*channels + c] = input[c][s];
    return inter;
}

float **af_deinterleave2( uint8_t *input, enum SampleFmt fmt, unsigned channels, unsigned samplecount )
{
    float  *in  = (float*) af_convert( SMPFMT_FLT, input, fmt, channels, samplecount );
    float **out = af_deinterleave( in, channels, samplecount );
    free( in );
    return out;
}

uint8_t *af_interleave2( enum SampleFmt outfmt, float **input, unsigned channels, unsigned samplecount )
{
    float   *tmp = af_interleave( input, channels, samplecount );
    uint8_t *out = af_convert( outfmt, (uint8_t*) tmp, SMPFMT_FLT, channels, samplecount );
    free( tmp );
    return out;
}

static inline int samplesize( enum SampleFmt fmt )
{
    switch( fmt )
    {
    case SMPFMT_U8:
        return 1;
    case SMPFMT_S16:
        return 2;
    case SMPFMT_S32:
    case SMPFMT_FLT:
        return 4;
    case SMPFMT_DBL:
        return 8;
    default:
        return 0;
    }
}

#define CLIPFUN( num, type, min, max )                                  \
    static inline type clip##num( int64_t i ) {                         \
        return (type)( ( i > max ) ? max : ( ( i < min ) ? min : i ) ); \
    }
CLIPFUN( 8,  uint8_t, 0,         UINT8_MAX )
CLIPFUN( 16, int16_t, INT16_MIN, INT16_MAX )
CLIPFUN( 32, int32_t, INT32_MIN, INT32_MAX )
#undef CLIPFUN

uint8_t *af_convert( enum SampleFmt outfmt, uint8_t *input, enum SampleFmt fmt, unsigned channels, unsigned samplecount )
{
    int totalsamples = channels * samplecount;
    int sz = samplesize( outfmt ) * totalsamples;
    uint8_t *out = malloc( sz );
    if( !out )
        return NULL;

    if( fmt == outfmt )
    {
        memcpy( out, input, sz );
        return out;
    }

#define CONVERT( ifmt, ofmt, otype, expr )                  \
    if( ifmt == fmt && ofmt == outfmt ) {                   \
        for( int i = 0; i < totalsamples; i++ )             \
        {                                                   \
            ((otype*)out)[i] = (otype)expr;                 \
        }                                                   \
        return out;                                         \
    }
#define IN( itype ) (((itype*)input)[i])

    CONVERT( SMPFMT_U8,  SMPFMT_S16, int16_t, (IN( uint8_t ) - 0x80) << 8 );
    CONVERT( SMPFMT_U8,  SMPFMT_S32, int32_t, (IN( uint8_t ) - 0x80) << 24 );
    CONVERT( SMPFMT_U8,  SMPFMT_FLT, float,   (IN( uint8_t ) - 0x80) * (1.0 / (1<<7)) );
    CONVERT( SMPFMT_U8,  SMPFMT_DBL, double,  (IN( uint8_t ) - 0x80) * (1.0 / (1<<7)) );
    CONVERT( SMPFMT_S16, SMPFMT_U8,  uint8_t, (IN( int16_t ) >> 8) + 0x80 );
    CONVERT( SMPFMT_S16, SMPFMT_S32, int32_t,  IN( int16_t ) << 16 );
    CONVERT( SMPFMT_S16, SMPFMT_FLT, float,    IN( int16_t ) * (1.0 / (1<<15)) );
    CONVERT( SMPFMT_S16, SMPFMT_DBL, double,   IN( int16_t ) * (1.0 / (1<<15)) );
    CONVERT( SMPFMT_S32, SMPFMT_U8,  uint8_t, (IN( int32_t ) >> 24) + 0x80 );
    CONVERT( SMPFMT_S32, SMPFMT_S16, int16_t,  IN( int32_t ) >> 16 );
    CONVERT( SMPFMT_S32, SMPFMT_FLT, float,    IN( int32_t ) * (1.0 / (1<<31)) );
    CONVERT( SMPFMT_S32, SMPFMT_DBL, double,   IN( int32_t ) * (1.0 / (1<<31)) );
    CONVERT( SMPFMT_FLT, SMPFMT_U8,  uint8_t, clip8(  lrintf(  IN( float ) * (1<<7) ) + 0x80 ) );
    CONVERT( SMPFMT_FLT, SMPFMT_S16, int16_t, clip16( lrintf(  IN( float ) * (1<<15) ) ) );
    CONVERT( SMPFMT_FLT, SMPFMT_S32, int32_t, clip32( llrintf( IN( float ) * (1U<<31) ) ) );
    CONVERT( SMPFMT_FLT, SMPFMT_DBL, double,   IN( float ) );
    CONVERT( SMPFMT_FLT, SMPFMT_U8,  uint8_t, clip8(  lrintf(  IN( double ) * (1<<7) ) + 0x80 ) );
    CONVERT( SMPFMT_FLT, SMPFMT_S16, int16_t, clip16( lrintf(  IN( double ) * (1<<15) ) ) );
    CONVERT( SMPFMT_FLT, SMPFMT_S32, int32_t, clip32( llrintf( IN( double ) * (1U<<31) ) ) );
    CONVERT( SMPFMT_FLT, SMPFMT_DBL, double,   IN( double ) );
#undef IN
#undef CONVERT
    free( out );
    return NULL;
}
