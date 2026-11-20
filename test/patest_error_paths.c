/** @file patest_error_paths.c
    @ingroup test_src

    @brief Tests that internal error paths which used to terminate the process
    with assert() now degrade gracefully: invalid parameters are rejected with
    error codes and defensive guards no-op instead of aborting.

    Covers the common-layer buffer processor (pa_process.c), the CPU load
    measurer (pa_cpuload.c) and a public API smoke test. Each check would have
    crashed (or hung) the process before the assertions were removed.

    @author Claude Code cleanup task
*/
/*
 * This program uses the PortAudio Portable Audio Library.
 * For more information see: http://www.portaudio.com/
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 * CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <stdio.h>
#include <math.h>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h> /* alarm() - guard against regression to infinite loop */
#define HAVE_ALARM 1
#endif

#include "portaudio.h"
#include "pa_process.h"
#include "pa_cpuload.h"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond) \
    do { \
        ++g_checks; \
        if( !(cond) ) { \
            ++g_failures; \
            printf( "FAIL line %d: %s\n", __LINE__, #cond ); \
        } \
    } while (0)

static int g_callbackCount = 0;

static int DummyCallback( const void *input, void *output,
        unsigned long frameCount, const PaStreamCallbackTimeInfo *timeInfo,
        PaStreamCallbackFlags statusFlags, void *userData )
{
    (void)input; (void)output; (void)frameCount;
    (void)timeInfo; (void)statusFlags; (void)userData;
    ++g_callbackCount;
    return paContinue;
}

/* -------------------------------------------------------------------------
 * PaUtil_InitializeBufferProcessor must reject invalid parameters with an
 * error code (previously assert() aborted the process, or worse, the invalid
 * values tripped assertions downstream).
 */
static void TestBufferProcessorInitValidation( void )
{
    PaUtilBufferProcessor bp;
    PaError result;

    printf( "%s\n", __FUNCTION__ );

    /* negative channel counts (would wrap to huge unsigned in the struct) */
    result = PaUtil_InitializeBufferProcessor( &bp,
            -1, paFloat32, paFloat32,
            1, paFloat32, paFloat32,
            44100., paNoFlag, 0, 64, paUtilFixedHostBufferSize,
            DummyCallback, 0 );
    CHECK( result == paInternalError );

    result = PaUtil_InitializeBufferProcessor( &bp,
            1, paFloat32, paFloat32,
            -1, paFloat32, paFloat32,
            44100., paNoFlag, 0, 64, paUtilFixedHostBufferSize,
            DummyCallback, 0 );
    CHECK( result == paInternalError );

    /* no channels at all */
    result = PaUtil_InitializeBufferProcessor( &bp,
            0, paFloat32, paFloat32,
            0, paFloat32, paFloat32,
            44100., paNoFlag, 0, 64, paUtilFixedHostBufferSize,
            DummyCallback, 0 );
    CHECK( result == paInternalError );

    /* non-positive sample rate (division by zero in bp->samplePeriod) */
    result = PaUtil_InitializeBufferProcessor( &bp,
            1, paFloat32, paFloat32,
            1, paFloat32, paFloat32,
            0., paNoFlag, 0, 64, paUtilFixedHostBufferSize,
            DummyCallback, 0 );
    CHECK( result == paInternalError );

    result = PaUtil_InitializeBufferProcessor( &bp,
            1, paFloat32, paFloat32,
            1, paFloat32, paFloat32,
            -48000., paNoFlag, 0, 64, paUtilFixedHostBufferSize,
            DummyCallback, 0 );
    CHECK( result == paInternalError );

    /* zero frames per host buffer with a fixed host buffer size */
    result = PaUtil_InitializeBufferProcessor( &bp,
            1, paFloat32, paFloat32,
            1, paFloat32, paFloat32,
            44100., paNoFlag, 0, 0, paUtilFixedHostBufferSize,
            DummyCallback, 0 );
    CHECK( result == paInternalError );

    /* pre-existing validation must be preserved: paNeverDropInput rules */
    result = PaUtil_InitializeBufferProcessor( &bp,
            1, paFloat32, paFloat32,
            1, paFloat32, paFloat32,
            44100., paNeverDropInput, 128, 64, paUtilFixedHostBufferSize,
            DummyCallback, 0 );
    CHECK( result == paInvalidFlag );

    /* valid configurations must still succeed (success path unchanged) */
    result = PaUtil_InitializeBufferProcessor( &bp,
            2, paFloat32, paFloat32,
            2, paFloat32, paFloat32,
            44100., paNoFlag, 0, 64, paUtilFixedHostBufferSize,
            DummyCallback, 0 );
    CHECK( result == paNoError );
    if( result == paNoError )
        PaUtil_TerminateBufferProcessor( &bp );

    /* half duplex variants */
    result = PaUtil_InitializeBufferProcessor( &bp,
            1, paFloat32, paFloat32,
            0, paFloat32, paFloat32,
            44100., paNoFlag, 0, 64, paUtilFixedHostBufferSize,
            DummyCallback, 0 );
    CHECK( result == paNoError );
    if( result == paNoError )
        PaUtil_TerminateBufferProcessor( &bp );

    result = PaUtil_InitializeBufferProcessor( &bp,
            0, paFloat32, paFloat32,
            1, paFloat32, paFloat32,
            44100., paNoFlag, 0, 64, paUtilFixedHostBufferSize,
            DummyCallback, 0 );
    CHECK( result == paNoError );
    if( result == paNoError )
        PaUtil_TerminateBufferProcessor( &bp );

    /* adapting (variable host buffer size) full duplex, exercises
       CalculateFrameShift() */
    result = PaUtil_InitializeBufferProcessor( &bp,
            1, paFloat32, paFloat32,
            1, paFloat32, paFloat32,
            44100., paNoFlag, 100, 64, paUtilFixedHostBufferSize,
            DummyCallback, 0 );
    CHECK( result == paNoError );
    if( result == paNoError )
        PaUtil_TerminateBufferProcessor( &bp );
}

/* -------------------------------------------------------------------------
 * The channel accessor functions must ignore invalid arguments instead of
 * asserting.
 */
static void TestBufferProcessorSetterGuards( void )
{
    PaUtilBufferProcessor bp;
    PaError result;
    float buffer[256] = {0};

    printf( "%s\n", __FUNCTION__ );

    result = PaUtil_InitializeBufferProcessor( &bp,
            2, paFloat32, paFloat32,
            2, paFloat32, paFloat32,
            44100., paNoFlag, 0, 64, paUtilFixedHostBufferSize,
            DummyCallback, 0 );
    CHECK( result == paNoError );
    if( result != paNoError )
        return;

    /* out-of-range channel indices: must be no-ops (previously asserted) */
    PaUtil_SetInputChannel( &bp, 5, buffer, 1 );
    CHECK( bp.hostInputChannels[0][0].data == 0 );
    CHECK( bp.hostInputChannels[0][1].data == 0 );

    PaUtil_Set2ndInputChannel( &bp, 2, buffer, 1 );
    CHECK( bp.hostInputChannels[1][0].data == 0 );

    PaUtil_SetOutputChannel( &bp, 99, buffer, 1 );
    CHECK( bp.hostOutputChannels[0][0].data == 0 );

    PaUtil_Set2ndOutputChannel( &bp, 7, buffer, 1 );
    CHECK( bp.hostOutputChannels[1][0].data == 0 );

    /* NULL data for an output channel: no-op */
    PaUtil_SetOutputChannel( &bp, 0, NULL, 1 );
    CHECK( bp.hostOutputChannels[0][0].data == 0 );

    /* channel range overrunning the channel count: no-op */
    PaUtil_SetInterleavedInputChannels( &bp, 1, buffer, 10 );
    CHECK( bp.hostInputChannels[0][0].data == 0 );
    CHECK( bp.hostInputChannels[0][1].data == 0 );

    PaUtil_SetInterleavedOutputChannels( &bp, 2, buffer, 1 );
    CHECK( bp.hostOutputChannels[0][0].data == 0 );

    PaUtil_Set2ndInterleavedInputChannels( &bp, 0, buffer, 3 );
    CHECK( bp.hostInputChannels[1][0].data == 0 );

    PaUtil_Set2ndInterleavedOutputChannels( &bp, 1, buffer, 0x7fffffff );
    CHECK( bp.hostOutputChannels[1][0].data == 0 );

    /* interleaving mismatch: no-op */
    PaUtil_SetNonInterleavedInputChannel( &bp, 0, buffer );
    CHECK( bp.hostInputChannels[0][0].data == 0 );
    PaUtil_SetNonInterleavedOutputChannel( &bp, 0, buffer );
    CHECK( bp.hostOutputChannels[0][0].data == 0 );
    PaUtil_Set2ndNonInterleavedInputChannel( &bp, 0, buffer );
    CHECK( bp.hostInputChannels[1][0].data == 0 );
    PaUtil_Set2ndNonInterleavedOutputChannel( &bp, 0, buffer );
    CHECK( bp.hostOutputChannels[1][0].data == 0 );

    /* valid calls must still work (success path unchanged) */
    PaUtil_SetInputChannel( &bp, 0, buffer, 1 );
    CHECK( bp.hostInputChannels[0][0].data == buffer );
    PaUtil_SetOutputChannel( &bp, 1, buffer, 2 );
    CHECK( bp.hostOutputChannels[0][1].data == buffer );
    CHECK( bp.hostOutputChannels[0][1].stride == 2 );

    PaUtil_TerminateBufferProcessor( &bp );

    /* PaUtil_SetNoInput / PaUtil_SetNoOutput on streams without the
       corresponding direction: must not crash (previously asserted) */
    result = PaUtil_InitializeBufferProcessor( &bp,
            0, paFloat32, paFloat32,
            1, paFloat32, paFloat32,
            44100., paNoFlag, 0, 64, paUtilFixedHostBufferSize,
            DummyCallback, 0 );
    CHECK( result == paNoError );
    if( result == paNoError )
    {
        PaUtil_SetNoInput( &bp ); /* no input channels: no-op */
        PaUtil_TerminateBufferProcessor( &bp );
    }

    result = PaUtil_InitializeBufferProcessor( &bp,
            1, paFloat32, paFloat32,
            0, paFloat32, paFloat32,
            44100., paNoFlag, 0, 64, paUtilFixedHostBufferSize,
            DummyCallback, 0 );
    CHECK( result == paNoError );
    if( result == paNoError )
    {
        PaUtil_SetNoOutput( &bp ); /* no output channels: no-op */
        PaUtil_TerminateBufferProcessor( &bp );
    }
}

/* -------------------------------------------------------------------------
 * PaUtil_EndBufferProcessing: the success path must be unchanged, and the
 * paths that used to assert must degrade gracefully.
 */
static void TestEndBufferProcessing( void )
{
    PaUtilBufferProcessor bp;
    PaError result;
    PaStreamCallbackTimeInfo timeInfo = { 0., 0., 0. };
    float inBuf[256] = {0};
    float outBuf[256] = {0};
    unsigned long frames;
    int callbackResult;

    printf( "%s\n", __FUNCTION__ );

    /* -- success path: full duplex, matching frame counts -- */
    result = PaUtil_InitializeBufferProcessor( &bp,
            1, paFloat32, paFloat32,
            1, paFloat32, paFloat32,
            44100., paNoFlag, 0, 64, paUtilFixedHostBufferSize,
            DummyCallback, 0 );
    CHECK( result == paNoError );
    if( result != paNoError )
        return;

    g_callbackCount = 0;
    PaUtil_SetInputFrameCount( &bp, 64 );
    PaUtil_SetInterleavedInputChannels( &bp, 0, inBuf, 1 );
    PaUtil_SetOutputFrameCount( &bp, 64 );
    PaUtil_SetInterleavedOutputChannels( &bp, 0, outBuf, 1 );
    PaUtil_BeginBufferProcessing( &bp, &timeInfo, 0 );
    callbackResult = paContinue;
    frames = PaUtil_EndBufferProcessing( &bp, &callbackResult );
    CHECK( frames == 64 );
    CHECK( callbackResult == paContinue );
    CHECK( g_callbackCount == 1 );

    /* -- mismatched input/output frame counts: previously asserted (and
       without the guard a naive fix would loop forever or overrun). Must
       process only as many frames as both directions can sustain. -- */
    PaUtil_SetInputFrameCount( &bp, 32 );
    PaUtil_SetInterleavedInputChannels( &bp, 0, inBuf, 1 );
    PaUtil_SetOutputFrameCount( &bp, 64 );
    PaUtil_SetInterleavedOutputChannels( &bp, 0, outBuf, 1 );
    PaUtil_BeginBufferProcessing( &bp, &timeInfo, 0 );
    callbackResult = paContinue;
#if HAVE_ALARM
    alarm( 30 ); /* fail noisily instead of hanging forever on regression */
#endif
    frames = PaUtil_EndBufferProcessing( &bp, &callbackResult );
#if HAVE_ALARM
    alarm( 0 );
#endif
    CHECK( frames == 32 );

    /* -- zero frames of input while output is available: previously asserted
       framesToProcess != 0 inside the loop; must return, not spin. -- */
    PaUtil_SetOutputFrameCount( &bp, 64 );
    PaUtil_SetInterleavedOutputChannels( &bp, 0, outBuf, 1 );
    /* note: input frame counts are 0 now (consumed above), but the input
       channel data pointer is still set, i.e. "input was supplied" */
    PaUtil_BeginBufferProcessing( &bp, &timeInfo, 0 );
    callbackResult = paContinue;
#if HAVE_ALARM
    alarm( 30 );
#endif
    frames = PaUtil_EndBufferProcessing( &bp, &callbackResult );
#if HAVE_ALARM
    alarm( 0 );
#endif
    CHECK( frames == 0 );

    /* -- invalid callback result value: previously asserted, now coerced to
       paAbort so no further callbacks are made -- */
    PaUtil_SetInputFrameCount( &bp, 64 );
    PaUtil_SetInterleavedInputChannels( &bp, 0, inBuf, 1 );
    PaUtil_SetOutputFrameCount( &bp, 64 );
    PaUtil_SetInterleavedOutputChannels( &bp, 0, outBuf, 1 );
    PaUtil_BeginBufferProcessing( &bp, &timeInfo, 0 );
    callbackResult = 42; /* not paContinue/paComplete/paAbort */
    g_callbackCount = 0;
    frames = PaUtil_EndBufferProcessing( &bp, &callbackResult );
    CHECK( callbackResult == paAbort );
    CHECK( g_callbackCount == 0 );
    CHECK( frames == 64 );

    PaUtil_TerminateBufferProcessor( &bp );
}

/* -------------------------------------------------------------------------
 * PaUtil_InitializeCpuLoadMeasurer with a non-positive sample rate used to
 * assert; it must now degrade to a harmless zero load reading.
 */
static void TestCpuLoadMeasurerGuards( void )
{
    PaUtilCpuLoadMeasurer measurer;

    printf( "%s\n", __FUNCTION__ );

    PaUtil_InitializeCpuLoadMeasurer( &measurer, 0. ); /* was assert */
    PaUtil_BeginCpuLoadMeasurement( &measurer );
    PaUtil_EndCpuLoadMeasurement( &measurer, 128 );
    CHECK( PaUtil_GetCpuLoad( &measurer ) == 0. );

    PaUtil_InitializeCpuLoadMeasurer( &measurer, -44100. ); /* was assert */
    PaUtil_BeginCpuLoadMeasurement( &measurer );
    PaUtil_EndCpuLoadMeasurement( &measurer, 128 );
    CHECK( PaUtil_GetCpuLoad( &measurer ) == 0. );

    /* valid sample rate still produces a finite, non-negative load */
    PaUtil_InitializeCpuLoadMeasurer( &measurer, 48000. );
    PaUtil_BeginCpuLoadMeasurement( &measurer );
    PaUtil_EndCpuLoadMeasurement( &measurer, 128 );
    {
        double load = PaUtil_GetCpuLoad( &measurer );
        CHECK( load >= 0. && load < 1e6 );
    }

    /* zero frames processed must remain a no-op */
    PaUtil_ResetCpuLoadMeasurer( &measurer );
    PaUtil_BeginCpuLoadMeasurement( &measurer );
    PaUtil_EndCpuLoadMeasurement( &measurer, 0 );
    CHECK( PaUtil_GetCpuLoad( &measurer ) == 0. );
}

/* -------------------------------------------------------------------------
 * Public API smoke test: Pa_Initialize() runs the (previously asserting)
 * PA_VALIDATE_TYPE_SIZES / PA_VALIDATE_ENDIANNESS checks, and out-of-range
 * devices must be reported as errors, not assertions.
 */
static void TestPublicApiSmoke( void )
{
    PaError result;
    PaStreamParameters params;
    PaStream *stream = 0;

    printf( "%s\n", __FUNCTION__ );

    result = Pa_Initialize();
    CHECK( result == paNoError );
    if( result != paNoError )
        return;

    /* out-of-range device index must return paInvalidDevice */
    params.device = Pa_GetDeviceCount(); /* one past the last valid device */
    if( params.device < 0 )
        params.device = 0; /* no devices at all: 0 is out of range too */
    params.channelCount = 1;
    params.sampleFormat = paFloat32;
    params.suggestedLatency = 0.1;
    params.hostApiSpecificStreamInfo = 0;

    result = Pa_IsFormatSupported( &params, 0, 44100. );
    CHECK( result == paInvalidDevice );

    result = Pa_OpenStream( &stream, &params, 0, 44100., 64, paNoFlag, 0, 0 );
    CHECK( result == paInvalidDevice );
    CHECK( stream == 0 );

    /* negative device index (other than paNoDevice) */
    params.device = -5;
    result = Pa_OpenStream( &stream, &params, 0, 44100., 64, paNoFlag, 0, 0 );
    CHECK( result == paInvalidDevice );

    CHECK( Pa_Terminate() == paNoError );
}

int main( void )
{
    TestBufferProcessorInitValidation();
    TestBufferProcessorSetterGuards();
    TestEndBufferProcessing();
    TestCpuLoadMeasurerGuards();
    TestPublicApiSmoke();

    printf( "%d checks, %d failures\n", g_checks, g_failures );
    if( g_failures > 0 )
    {
        printf( "FAILED\n" );
        return 1;
    }
    printf( "PASSED\n" );
    return 0;
}
