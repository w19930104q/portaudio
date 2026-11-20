#ifndef PA_TYPES_H
#define PA_TYPES_H

/*
 * Portable Audio I/O Library
 * integer type definitions
 *
 * Based on the Open Source API proposed by Ross Bencina
 * Copyright (c) 1999-2006 Ross Bencina, Phil Burk
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

/** @file
 @ingroup common_src

 @brief Definition of 16 and 32 bit integer types (PaInt16, PaInt32 etc)

 SIZEOF_SHORT, SIZEOF_INT and SIZEOF_LONG are set by the configure script
 when it is used. Otherwise we default to the common 32 bit values, if your
 platform doesn't use configure, and doesn't use the default values below
 you will need to explicitly define these symbols in your make file.

 A PA_VALIDATE_TYPE_SIZES macro is provided to verify at compile time that
 the values set in this file are correct.
*/

#ifndef SIZEOF_SHORT
#define SIZEOF_SHORT 2
#endif

#ifndef SIZEOF_INT
#define SIZEOF_INT 4
#endif

#ifndef SIZEOF_LONG
#define SIZEOF_LONG 4
#endif


#if SIZEOF_SHORT == 2
typedef signed short PaInt16;
typedef unsigned short PaUint16;
#elif SIZEOF_INT == 2
typedef signed int PaInt16;
typedef unsigned int PaUint16;
#else
#error pa_types.h was unable to determine which type to use for 16bit integers on the target platform
#endif

#if SIZEOF_SHORT == 4
typedef signed short PaInt32;
typedef unsigned short PaUint32;
#elif SIZEOF_INT == 4
typedef signed int PaInt32;
typedef unsigned int PaUint32;
#elif SIZEOF_LONG == 4
typedef signed long PaInt32;
typedef unsigned long PaUint32;
#else
#error pa_types.h was unable to determine which type to use for 32bit integers on the target platform
#endif


/* PA_VALIDATE_TYPE_SIZES verifies at compile time that the size of the integer
 types matches the expected values, ensuring that PortAudio was configured
 correctly. A mismatch causes a compile error (a typedef with negative array
 size) rather than a runtime assertion that would terminate the process.
 The trailing enum references the typedef via sizeof so that compilers don't
 warn about it being unused.
*/
#define PA_VALIDATE_TYPE_SIZES_CONCAT_INNER_( a, b ) a ## b
#define PA_VALIDATE_TYPE_SIZES_CONCAT_( a, b ) PA_VALIDATE_TYPE_SIZES_CONCAT_INNER_( a, b )
#define PA_VALIDATE_TYPE_SIZES \
    typedef char PA_VALIDATE_TYPE_SIZES_CONCAT_( pa_type_sizes_are_invalid_, __LINE__ ) \
        [ ( sizeof( PaUint16 ) == 2 && sizeof( PaInt16 ) == 2 && \
            sizeof( PaUint32 ) == 4 && sizeof( PaInt32 ) == 4 ) ? 1 : -1 ]; \
    enum { PA_VALIDATE_TYPE_SIZES_CONCAT_( pa_type_sizes_check_, __LINE__ ) = \
        (int)sizeof( PA_VALIDATE_TYPE_SIZES_CONCAT_( pa_type_sizes_are_invalid_, __LINE__ ) ) }


#endif /* PA_TYPES_H */
