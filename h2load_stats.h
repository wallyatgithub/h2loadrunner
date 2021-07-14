#ifndef H2LOAD_STAT_H
#define H2LOAD_STAT_H

#include <chrono>
#include <atomic>
#include <vector>


namespace h2load
{

struct RequestStat
{
    // time point when request was sent
    std::chrono::steady_clock::time_point request_time;
    // same, but in wall clock reference frame
    std::chrono::system_clock::time_point request_wall_time;
    // time point when stream was closed
    std::chrono::steady_clock::time_point stream_close_time;
    // upload data length sent so far
    int64_t data_offset;
    // HTTP status code
    int status;
    // true if stream was successfully closed.  This means stream was
    // not reset, but it does not mean HTTP level error (e.g., 404).
    bool completed;
};

struct TransactionStat
{
    std::chrono::steady_clock::time_point start_time;
    bool successful;
    explicit TransactionStat()
    {
        successful = false;
        start_time = std::chrono::steady_clock::now();
    };
};

struct ClientStat
{
    // time client started (i.e., first connect starts)
    std::chrono::steady_clock::time_point client_start_time;
    // time client end (i.e., client somehow processed all requests it
    // is responsible for, and disconnected)
    std::chrono::steady_clock::time_point client_end_time;
    // The number of requests completed successful, but not necessarily
    // means successful HTTP status code.
    size_t req_success;
    size_t leading_req_done;
    size_t trans_done;

    // The following 3 numbers are overwritten each time when connection
    // is made.

    // time connect starts
    std::chrono::steady_clock::time_point connect_start_time;
    // time to connect
    std::chrono::steady_clock::time_point connect_time;
    // time to first byte (TTFB)
    std::chrono::steady_clock::time_point ttfb;

    ClientStat()
    {
        req_success = 0;
        leading_req_done = 0;
        trans_done = 0;
    }
};

struct SDStat
{
    // min, max, mean and sd (standard deviation)
    double min, max, mean, sd;
    // percentage of samples inside mean -/+ sd
    double within_sd;
};

struct SDStats
{
    // time for request
    SDStat request;
    // time for connect
    SDStat connect;
    // time to first byte (TTFB)
    SDStat ttfb;
    // request per second for each client
    SDStat rps;
};

struct Stats
{
    Stats(size_t req_todo, size_t nclients);
    // The total number of requests
    size_t req_todo;
    // The number of requests issued so far
    size_t req_started;
    // The number of requests finished
    std::atomic<size_t> req_done;
    // The number of requests completed successful, but not necessarily
    // means successful HTTP status code.
    size_t req_success;
    // The number of requests marked as success.  HTTP status code is
    // also considered as success. This is subset of req_done.
    std::atomic<size_t> req_status_success;
    // The number of requests failed. This is subset of req_done.
    size_t req_failed;
    // The number of requests failed due to network errors. This is
    // subset of req_failed.
    size_t req_error;
    // The number of requests that failed due to timeout.
    size_t req_timedout;
    // The number of bytes received on the "wire". If SSL/TLS is used,
    // this is the number of decrypted bytes the application received.
    int64_t bytes_total;
    // The number of bytes received for header fields.  This is
    // compressed version.
    int64_t bytes_head;
    // The number of bytes received for header fields after they are
    // decompressed.
    int64_t bytes_head_decomp;
    // The number of bytes received in DATA frame.
    int64_t bytes_body;
    // The number of each HTTP status category, status[i] is status code
    // in the range [i*100, (i+1)*100).
    std::array<size_t, 6> status;
    // The statistics per request
    std::vector<RequestStat> req_stats;
    // The statistics per client
    std::vector<ClientStat> client_stats;
    std::atomic<uint64_t> max_resp_time_ms;
    std::atomic<uint64_t> min_resp_time_ms;
    std::atomic<uint64_t> trans_max_resp_time_ms;
    std::atomic<uint64_t> trans_min_resp_time_ms;
    size_t transaction_done;
    size_t transaction_successful;
};


struct Stream
{
    RequestStat req_stat;
    int status_success;
    Stream();
};


}
#endif
