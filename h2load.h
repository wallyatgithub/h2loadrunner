/*
 * nghttp2 - HTTP/2 C Library
 *
 * Copyright (c) 2014 Tatsuhiro Tsujikawa
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#ifndef H2LOAD_H
#define H2LOAD_H

#include "http2.h"
#include "memchunk.h"
#include "template.h"


using namespace nghttp2;

namespace h2load
{

constexpr auto BACKOFF_WRITE_BUFFER_THRES = 16_k;
constexpr int MAX_STREAM_TO_BE_EXHAUSTED = -2;

enum ClientState { CLIENT_IDLE, CLIENT_CONNECTING, CLIENT_CONNECTED };

// This type tells whether the client is in warmup phase or not or is over
enum class Phase
{
    INITIAL_IDLE,  // Initial idle state before warm-up phase
    WARM_UP,       // Warm up phase when no measurements are done
    MAIN_DURATION, // Main measurement phase; if timing-based
    // test is not run, this is the default phase
    DURATION_OVER  // This phase occurs after the measurements are over
};


// We use reservoir sampling method
struct Sampling
{
    // maximum number of samples
    size_t max_samples;
    // number of samples seen, including discarded samples.
    size_t n;
};

} // namespace h2load

#endif // H2LOAD_H
