#ifndef H2LOAD_STAT_H
#define H2LOAD_STAT_H

#include <chrono>
#include <atomic>
#include <vector>


namespace h2load
{

struct Request_Response_Data;


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
    size_t scenario_index;
    size_t request_index;

    explicit RequestStat(size_t scenario_id, size_t request_id):
        scenario_index(scenario_id),
        request_index(request_id),
        status(0x1FFF)
    {};
};

struct ClientStat
{
    // time client started (i.e., first connect starts)
    std::chrono::steady_clock::time_point client_start_time = std::chrono::steady_clock::time_point();
    // time client end (i.e., client somehow processed all requests it
    // is responsible for, and disconnected)
    std::chrono::steady_clock::time_point client_end_time = std::chrono::steady_clock::time_point();
    // The number of requests completed successful, but not necessarily
    // means successful HTTP status code.
    size_t req_success;

    // The following 3 numbers are overwritten each time when connection
    // is made.

    // time connect starts
    std::chrono::steady_clock::time_point connect_start_time = std::chrono::steady_clock::time_point();
    // time to connect
    std::chrono::steady_clock::time_point connect_time = std::chrono::steady_clock::time_point();
    // time to first byte (TTFB)
    std::chrono::steady_clock::time_point ttfb = std::chrono::steady_clock::time_point();

    ClientStat()
    {
        req_success = 0;
    }

    friend std::ostream& operator<<(std::ostream& o, const ClientStat& stat)
    {
        auto now = std::chrono::steady_clock::now();
        o << "ClientStats: "<<std::endl
          <<"{ " << std::endl
          << "  client_start_time: " << std::chrono::duration_cast<std::chrono::milliseconds>(stat.client_start_time - now).count() << std::endl
          << "  client_end_time: " << std::chrono::duration_cast<std::chrono::milliseconds>(stat.client_end_time - now).count() << std::endl
          << "  connect_start_time: " << std::chrono::duration_cast<std::chrono::milliseconds>(stat.connect_start_time - now).count() << std::endl
          << "  connect_time: " << std::chrono::duration_cast<std::chrono::milliseconds>(stat.connect_time - now).count() << std::endl
          << "  time to first byte: " << std::chrono::duration_cast<std::chrono::milliseconds>(stat.ttfb - now).count() << std::endl
          << "  req_success: " << stat.req_success << std::endl
          << "}" << std::endl;
        return o;
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
    size_t req_todo = 0;
    // The number of requests issued so far
    size_t req_started = 0;
    // The number of requests finished
    size_t req_done = 0;
    // The number of requests completed successful, but not necessarily
    // means successful HTTP status code.
    size_t req_success = 0;
    // The number of requests marked as success.  HTTP status code is
    // also considered as success. This is subset of req_done.
    size_t req_status_success = 0;
    // The number of requests failed. This is subset of req_done.
    size_t req_failed = 0;
    // The number of requests failed due to network errors. This is
    // subset of req_failed.
    size_t req_error = 0;
    // The number of requests that failed due to timeout.
    size_t req_timedout = 0;
    // The number of bytes received on the "wire". If SSL/TLS is used,
    // this is the number of decrypted bytes the application received.
    int64_t bytes_total = 0;
    // The number of bytes received for header fields.  This is
    // compressed version.
    int64_t bytes_head = 0;
    // The number of bytes received for header fields after they are
    // decompressed.
    int64_t bytes_head_decomp = 0;
    // The number of bytes received in DATA frame.
    int64_t bytes_body = 0;
    // The number of each HTTP status category, status[i] is status code
    // in the range [i*100, (i+1)*100).
    std::array<size_t, 6> status;
    // The statistics per request
    std::vector<RequestStat> req_stats;
    // The statistics per client
    std::vector<ClientStat> client_stats;
    // The number of UDP datagrams received.
    size_t udp_dgram_recv = 0;
    // The number of UDP datagrams sent.
    size_t udp_dgram_sent = 0;

    friend std::ostream& operator<<(std::ostream& o, const Stats& stat)
    {
        o << "Stats: "<<std::endl
          <<"{ " << std::endl
          << "  req_todo: " << stat.req_todo << std::endl
          << "  req_started: " << stat.req_started << std::endl
          << "  req_done: " << stat.req_done << std::endl
          << "  req_success: " << stat.req_success << std::endl
          << "  req_status_success: " << stat.req_status_success << std::endl
          << "  req_failed: " << stat.req_failed << std::endl
          << "  req_error: " << stat.req_error << std::endl
          << "  req_timedout: " << stat.req_timedout << std::endl
          << "  bytes_total: " << stat.bytes_total << std::endl
          << "  bytes_head: " << stat.bytes_head << std::endl
          << "  bytes_head_decomp: " << stat.bytes_head_decomp << std::endl
          << "  bytes_body: " << stat.bytes_body << std::endl
          << "  udp_dgram_recv: " << stat.udp_dgram_recv << std::endl
          << "  udp_dgram_sent: " << stat.udp_dgram_sent << std::endl;
        for (size_t i = 1; i < stat.status.size(); i++)
        {
            o << "  " << i <<"xx status code: " << stat.status[i] <<std::endl;
        }
        for (size_t i = 0; i < stat.client_stats.size(); i++)
        {
            o << "  client: " << i <<", req_success: " << stat.client_stats[i].req_success <<std::endl;
        }
        o <<"}" << std::endl;
        return o;
    }
};


struct Stream
{
    RequestStat req_stat;
    int status_success;
    bool statistics_eligible;
    std::unique_ptr<Request_Response_Data> request_response;
    Stream(bool stat_eligible, std::unique_ptr<Request_Response_Data>& rr_data);
};


}
#endif
