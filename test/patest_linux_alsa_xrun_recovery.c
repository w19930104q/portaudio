/*
 * patest_linux_alsa_xrun_recovery.c
 *
 * Reproduce a capture xrun (overrun) on a non-mmap ALSA PCM and verify that
 * the PortAudio ALSA host API recovers correctly: after the xrun has been
 * recovered the stream must stay active, the callback must keep firing and
 * the capture position must keep advancing.
 *
 * Background: for a non-mmap capture PCM, snd_pcm_recover() only moves the
 * device back to PREPARED state. Unlike playback (which restarts implicitly
 * once enough frames are written), a capture device has to be started
 * explicitly. PortAudio used to leave the device in PREPARED state after a
 * successful recovery, so poll() never reported new data and the callback
 * simply stopped firing while the stream still reported itself as running.
 *
 * The test requires an ALSA capture PCM that does NOT support mmap access
 * (the bug only exists on the readi/readn code path). A deterministic
 * virtual device for this purpose is provided by the "xrunmono" ALSA
 * ioplug, which emulates a capture device running on a monotonic clock and
 * latches into SND_PCM_STATE_XRUN when userspace stops consuming frames;
 * any other non-mmap capture PCM can be selected with -device.
 *
 * The xrun is induced by stalling inside the callback for longer than the
 * ALSA buffer can hold, which is exactly how an overloaded application
 * thread overruns a real capture device.
 *
 * Exit status: 0 on success (PASS), 1 on failed recovery checks,
 * 2 on usage error, otherwise a PortAudio error code.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <alsa/asoundlib.h>

#include "portaudio.h"
#include "pa_linux_alsa.h"

#define SAMPLE_RATE         44100
#define FRAMES_PER_BUFFER   256
#define NUM_XRUN_ROUNDS     3
#define STALL_MSEC          500     /* longer than the ALSA buffer duration, forces an overrun */
#define RECOVERY_WAIT_MSEC  3000

typedef struct
{
    unsigned long callbacks;
    unsigned long frames;
    unsigned long overflowFlags;
    long lastSample;
    volatile int stallMsec;
    volatile int stalled;
} TestData;

static int TestCallback( const void *input, void *output, unsigned long frameCount,
        const PaStreamCallbackTimeInfo *timeInfo, PaStreamCallbackFlags statusFlags,
        void *userData )
{
    TestData *data = (TestData *)userData;

    (void)output;
    (void)timeInfo;

    if( statusFlags & paInputOverflow )
        data->overflowFlags++;

    if( input )
    {
        const short *in = (const short *)input;
        /* Track the capture position: the last sample of every buffer. */
        data->lastSample = in[frameCount - 1];
    }

    /* Simulate an overloaded application: stall once, long enough to overrun
     * the device buffer. */
    if( data->stallMsec > 0 && !data->stalled )
    {
        int msec = data->stallMsec;
        data->stalled = 1;
        data->stallMsec = 0;
        Pa_Sleep( msec );
    }

    data->frames += frameCount;
    data->callbacks++;

    return paContinue;
}

int main( int argc, char **argv )
{
    const char *device = "xrunmono";
    PaStream *stream = NULL;
    PaError err;
    TestData data;
    PaAlsaStreamInfo alsaInfo;
    PaStreamParameters inputParams;
    int i, round;

    for( i = 1; i < argc; i++ )
    {
        if( !strcmp( argv[i], "-device" ) && i + 1 < argc )
            device = argv[++i];
        else
        {
            fprintf( stderr, "usage: %s [-device alsa-pcm-name]\n", argv[0] );
            return 2;
        }
    }

    memset( &data, 0, sizeof( data ) );

    err = Pa_Initialize();
    if( err != paNoError )
        goto error;

    /* The bug under test only exists on the non-mmap (readi/readn) path, so
     * refuse to run against a device that supports mmap access. */
    {
        snd_pcm_t *pcm = NULL;
        snd_pcm_hw_params_t *hwParams;
        int alsaErr;

        snd_pcm_hw_params_alloca( &hwParams );

        alsaErr = snd_pcm_open( &pcm, device, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK );
        if( alsaErr < 0 )
        {
            fprintf( stderr, "ALSA capture device '%s' cannot be opened: %s\n",
                    device, snd_strerror( alsaErr ) );
            err = paUnanticipatedHostError;
            goto error;
        }
        snd_pcm_hw_params_any( pcm, hwParams );
        if( snd_pcm_hw_params_test_access( pcm, hwParams, SND_PCM_ACCESS_MMAP_INTERLEAVED ) >= 0 ||
                snd_pcm_hw_params_test_access( pcm, hwParams, SND_PCM_ACCESS_MMAP_NONINTERLEAVED ) >= 0 )
        {
            fprintf( stderr, "ALSA device '%s' supports mmap; this test needs a non-mmap PCM\n",
                    device );
            snd_pcm_close( pcm );
            err = paUnanticipatedHostError;
            goto error;
        }
        snd_pcm_close( pcm );
        printf( "Device '%s' confirmed non-mmap capture PCM\n", device );
    }

    PaAlsa_InitializeStreamInfo( &alsaInfo );
    alsaInfo.deviceString = device;

    memset( &inputParams, 0, sizeof( inputParams ) );
    inputParams.device = paUseHostApiSpecificDeviceSpecification;
    inputParams.channelCount = 1;
    inputParams.sampleFormat = paInt16;
    inputParams.suggestedLatency = 0.1;
    inputParams.hostApiSpecificStreamInfo = &alsaInfo;

    err = Pa_OpenStream( &stream, &inputParams, NULL, SAMPLE_RATE, FRAMES_PER_BUFFER,
            paClipOff, TestCallback, &data );
    if( err != paNoError )
        goto error;

    err = Pa_StartStream( stream );
    if( err != paNoError )
        goto error;

    Pa_Sleep( 1500 );
    if( !Pa_IsStreamActive( stream ) )
    {
        fprintf( stderr, "Stream stopped before xrun was induced\n" );
        err = paInternalError;
        goto error;
    }

    {
        unsigned long callbacksBefore = data.callbacks;
        unsigned long framesBefore = data.frames;
        printf( "Before stall: %lu callbacks, %lu frames delivered\n",
                callbacksBefore, framesBefore );
        if( callbacksBefore == 0 )
        {
            fprintf( stderr, "Callback never fired before the stall\n" );
            err = paInternalError;
            goto error;
        }
    }

    /* Induce and recover from several xruns in a row. */
    for( round = 0; round < NUM_XRUN_ROUNDS; round++ )
    {
        unsigned long framesAtXrun = data.frames;
        unsigned long overflowsAtXrun = data.overflowFlags;

        /* Arm the in-callback stall; the next callback invocation blocks long
         * enough for the capture device to overrun. */
        data.stalled = 0;
        data.stallMsec = STALL_MSEC;
        Pa_Sleep( 1000 );

        if( !Pa_IsStreamActive( stream ) )
        {
            fprintf( stderr, "FAIL: stream became inactive after xrun round %d\n", round );
            err = paInternalError;
            goto error;
        }
        if( data.overflowFlags <= overflowsAtXrun )
        {
            fprintf( stderr, "FAIL: round %d stall did not produce a paInputOverflow flag\n", round );
            err = paInternalError;
            goto error;
        }

        /* Give the stream time to recover and deliver more frames. */
        Pa_Sleep( 3000 );

        if( data.frames <= framesAtXrun )
        {
            fprintf( stderr, "FAIL: capture position did not advance after xrun round %d\n", round );
            err = paInternalError;
            goto error;
        }
        printf( "Round %d recovered: %lu frames, %lu callbacks\n",
                round, data.frames, data.callbacks );
    }

    {
        unsigned long callbacksAfter = data.callbacks;
        unsigned long framesAfter = data.frames;
        unsigned long callbacksNow, framesNow, deltaFrames, expectedMin;
        int passed = 1;

        printf( "After stalls: %lu callbacks, %lu frames, %lu paInputOverflow flags\n",
                callbacksAfter, framesAfter, data.overflowFlags );

        Pa_Sleep( RECOVERY_WAIT_MSEC );

        callbacksNow = data.callbacks;
        framesNow = data.frames;
        printf( "After wait:   %lu callbacks, %lu frames delivered\n",
                callbacksNow, framesNow );

        if( !Pa_IsStreamActive( stream ) )
        {
            fprintf( stderr, "FAIL: stream is no longer active after recovery\n" );
            passed = 0;
        }
        if( callbacksNow <= callbacksAfter )
        {
            fprintf( stderr, "FAIL: callback did not resume after xrun recovery (%lu -> %lu invocations)\n",
                    callbacksAfter, callbacksNow );
            passed = 0;
        }
        if( framesNow <= framesAfter )
        {
            fprintf( stderr, "FAIL: capture position did not advance after xrun recovery (%lu -> %lu frames)\n",
                    framesAfter, framesNow );
            passed = 0;
        }

        /* The recovered stream should deliver frames at roughly the nominal rate. */
        deltaFrames = framesNow - framesAfter;
        expectedMin = (unsigned long)( SAMPLE_RATE * ( RECOVERY_WAIT_MSEC / 1000. ) / 2 );
        if( deltaFrames < expectedMin )
        {
            fprintf( stderr, "FAIL: post-recovery stream rate too low: %lu frames in %d ms (expected at least ~%lu)\n",
                    deltaFrames, RECOVERY_WAIT_MSEC, expectedMin );
            passed = 0;
        }

        err = Pa_StopStream( stream );
        if( err != paNoError )
            goto error;
        err = Pa_CloseStream( stream );
        stream = NULL;
        if( err != paNoError )
            goto error;
        Pa_Terminate();

        if( passed )
        {
            puts( "PASS: non-mmap capture xrun recovered; callbacks resumed and capture position advanced monotonically" );
            return 0;
        }
        return 1;
    }

error:
    fprintf( stderr, "Error number: %d\n", err );
    fprintf( stderr, "Error message: %s\n", Pa_GetErrorText( err ) );
    if( stream )
    {
        Pa_AbortStream( stream );
        Pa_CloseStream( stream );
    }
    Pa_Terminate();
    return err;
}
