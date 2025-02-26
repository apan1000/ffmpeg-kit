/*
 * Copyright (C) 2008 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <errno.h>
#include <malloc.h>
#include <math.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

// posix_memalign() and memalign() are not available in the NDK
// __INTRODUCED_IN(17)

#if __ANDROID_API__ < 17

int posix_memalign(void **memptr, size_t alignment, size_t size) {
    if ((alignment & (alignment - 1)) != 0 || alignment == 0) {
        return EINVAL;
    }

    if (alignment % sizeof(void *) != 0) {
        return EINVAL;
    }

    *memptr = memalign(alignment, size);
    if (*memptr == NULL) {
        return errno;
    }

    return 0;
}

#endif /* __ANDROID_API__ < 17 */

// log2() and log2f() are C99 functions, but they're not available in the NDK
// __INTRODUCED_IN(18)

#if __ANDROID_API__ < 18

double log2(double x) { return (log(x) / M_LN2); }

float log2f(float x) { return (float)log2((double)x); }

#endif /* __ANDROID_API__ < 18 */

#ifdef __cplusplus
}; /* end of extern "C" */
#endif