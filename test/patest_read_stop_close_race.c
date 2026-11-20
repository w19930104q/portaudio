/** @file patest_read_stop_close_race.c
    @ingroup test_src

    @brief Regression test for a race between a blocking Pa_ReadStream()/Pa_WriteStream()
    and Pa_StopStream()/Pa_CloseStream() issued from another thread.

    A blocking read or write polls the audio device while waiting for frames. If another
    thread stops and closes the stream at that moment, the stream must not be freed (nor
    the device handles closed) while the blocked call is still using them. This test
    repeatedly starts a stream, lets a worker thread block in Pa_ReadStream()/Pa_WriteStream(),
    and concurrently stops and closes the stream from the main thread. Before the fix this
    could segfault in the poll loop (use-after-free); now the blocked call must return
    cleanly (paStreamIsStopped/paBadStreamPtr) and the process must survive all iterations.

    The test passes trivially (reports "skipped") when no suitable device is available.

    @author PortAudio contributors
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

/*
 * The text above constitutes the entire PortAudio license; however,
 * the PortAudio community also makes the following non-binding requests:
 *
 * Any person wishing to distribute modifications to the Software is
 * requested to send the modifications to the original developer so that
 * they can be incorporated into the canonical version. It is also
 * requested that these non-binding requests be included along with the
 * license above.
 */

#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include "portaudio.h"

#define NUM_LOOPS          (50)
#define SAMPLE_RATE        (44100)
#define FRAMES_PER_BUFFER  (256)
/* Read/write more frames than one host buffer so the call blocks in the poll loop */
#define IO_FRAMES          (4096)

typedef struct IoThreadData
{
    PaStream *stream;
    int isInput;             /* 1: Pa_ReadStream, 0: Pa_WriteStream */
    volatile int quit;       /* set by the main thread once the stream is closed */
    PaError lastError;       /* last error returned by Pa_ReadStream/Pa_WriteStream */
    unsigned long ioCount;   /* number of successful blocking I/O calls */
}
IoThreadData;

static void *IoThreadProc( void *arg )
{
    IoThreadData *data = (IoThreadData *)arg;
    static short buffer[IO_FRAMES];

    while( !data->quit )
    {
        PaError err = data->isInput
                ? Pa_ReadStream( data->stream, buffer, IO_FRAMES )
                : Pa_WriteStream( data->stream, buffer, IO_FRAMES );

        if( err == paNoError || err == paInputOverflowed || err == paOutputUnderflowed )
        {
            ++data->ioCount;
            continue;
        }

        /* Expected once the main thread stops/closes the stream underneath us */
        data->lastError = err;
        break;
    }

    return NULL;
}

/* Run NUM_LOOPS iterations of: open + start stream, block a worker thread in
 * Pa_ReadStream/Pa_WriteStream, then stop and close the stream from this thread.
 * Returns 0 on success. */
static int RunRaceLoop( int isInput )
{
    int i, result = 0;

    for( i = 0; i < NUM_LOOPS; ++i )
    {
        PaStream *stream;
        PaStreamParameters params;
        IoThreadData data;
        pthread_t thread;
        PaError err;

        memset( &params, 0, sizeof( params ) );
        params.device = isInput ? Pa_GetDefaultInputDevice() : Pa_GetDefaultOutputDevice();
        params.channelCount = 1;
        params.sampleFormat = paInt16;
        params.suggestedLatency = isInput
                ? Pa_GetDeviceInfo( params.device )->defaultLowInputLatency
                : Pa_GetDeviceInfo( params.device )->defaultLowOutputLatency;
        params.hostApiSpecificStreamInfo = NULL;

        err = Pa_OpenStream( &stream,
                isInput ? &params : NULL,
                isInput ? NULL : &params,
                SAMPLE_RATE, FRAMES_PER_BUFFER, paClipOff, NULL, NULL );
        if( err != paNoError )
        {
            printf( "ERROR: Pa_OpenStream failed: %s\n", Pa_GetErrorText( err ) );
            return 1;
        }

        err = Pa_StartStream( stream );
        if( err != paNoError )
        {
            printf( "ERROR: Pa_StartStream failed: %s\n", Pa_GetErrorText( err ) );
            Pa_CloseStream( stream );
            return 1;
        }

        data.stream = stream;
        data.isInput = isInput;
        data.quit = 0;
        data.lastError = paNoError;
        data.ioCount = 0;

        if( pthread_create( &thread, NULL, IoThreadProc, &data ) != 0 )
        {
            printf( "ERROR: pthread_create failed\n" );
            Pa_CloseStream( stream );
            return 1;
        }

        /* Give the worker a varying amount of time to block inside the I/O call,
         * then stop and close the stream while it is blocked there. */
        Pa_Sleep( i % 11 );

        err = Pa_StopStream( stream );
        if( err != paNoError )
        {
            printf( "ERROR: Pa_StopStream failed: %s\n", Pa_GetErrorText( err ) );
            result = 1;
        }

        err = Pa_CloseStream( stream );
        if( err != paNoError )
        {
            printf( "ERROR: Pa_CloseStream failed: %s\n", Pa_GetErrorText( err ) );
            result = 1;
        }

        /* The stream is closed now; tell the worker to get out and wait for it. */
        data.quit = 1;
        pthread_join( thread, NULL );

        /* The blocked I/O call must have returned cleanly, either successfully or with
         * one of the expected "stream went away" errors. */
        if( data.lastError != paNoError
                && data.lastError != paStreamIsStopped
                && data.lastError != paBadStreamPtr )
        {
            printf( "ERROR: blocking %s returned unexpected error: %s\n",
                    isInput ? "Pa_ReadStream" : "Pa_WriteStream",
                    Pa_GetErrorText( data.lastError ) );
            result = 1;
        }

        if( result )
            break;
    }

    return result;
}

int main( int argc, char **argv )
{
    PaError err;
    int result = 0;
    int ranSomething = 0;

    (void)argc;
    (void)argv;

    err = Pa_Initialize();
    if( err != paNoError )
    {
        printf( "ERROR: Pa_Initialize failed: %s\n", Pa_GetErrorText( err ) );
        return 1;
    }

    if( Pa_GetDefaultInputDevice() != paNoDevice )
    {
        printf( "Testing blocking Pa_ReadStream vs concurrent Pa_StopStream/Pa_CloseStream...\n" );
        if( RunRaceLoop( 1 ) != 0 )
            result = 1;
        ranSomething = 1;
    }
    else
    {
        printf( "No default input device, read test skipped.\n" );
    }

    if( Pa_GetDefaultOutputDevice() != paNoDevice )
    {
        printf( "Testing blocking Pa_WriteStream vs concurrent Pa_StopStream/Pa_CloseStream...\n" );
        if( RunRaceLoop( 0 ) != 0 )
            result = 1;
        ranSomething = 1;
    }
    else
    {
        printf( "No default output device, write test skipped.\n" );
    }

    Pa_Terminate();

    if( !ranSomething )
        printf( "No audio devices available, test skipped.\n" );

    if( result == 0 )
        printf( "Test finished: no crash, no unexpected errors.\n" );

    return result;
}
