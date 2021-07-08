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
#include <fstream>
#include <streambuf>

#include "h2load.h"

#include <getopt.h>
#include <signal.h>
#ifdef HAVE_NETINET_IN_H
#  include <netinet/in.h>
#endif // HAVE_NETINET_IN_H
#include <netinet/tcp.h>
#include <sys/stat.h>
#ifdef HAVE_FCNTL_H
#  include <fcntl.h>
#endif // HAVE_FCNTL_H

#include <sys/types.h>
#ifdef HAVE_SYS_SOCKET_H
#  include <sys/socket.h>
#endif // HAVE_SYS_SOCKET_H
#ifdef HAVE_NETDB_H
#  include <netdb.h>
#endif // HAVE_NETDB_H
#include <sys/un.h>

#include <cstdio>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <future>
#include <random>
#include <vector>


#include <openssl/err.h>
#include <openssl/ssl.h>
extern "C" {
#include <ares.h>
}

#include <nghttp2/nghttp2.h>

#include "memchunk.h"
#include "template.h"
#include "url-parser/url_parser.h"

#include "h2load_http1_session.h"
#include "h2load_http2_session.h"
#include "tls.h"
#include "http2.h"
#include "util.h"
#include "template.h"
#include "h2load_utils.h"
#include "h2load_Config.h"
#include "h2load_Client.h"
#include "h2load_Worker.h"
#include "h2load_stats.h"
#include "staticjson/document.hpp"
#include "staticjson/staticjson.hpp"
#include "rapidjson/schema.h"
#include "rapidjson/prettywriter.h"
#include "config_schema.h"



#ifndef O_BINARY
#  define O_BINARY (0)
#endif // O_BINARY

using namespace nghttp2;

namespace h2load
{

Config config;

namespace
{
constexpr size_t MAX_SAMPLES = 1000000;
} // namespace

Stats::Stats(size_t req_todo, size_t nclients)
    : req_todo(req_todo),
      req_started(0),
      req_done(0),
      req_success(0),
      req_status_success(0),
      req_failed(0),
      req_error(0),
      req_timedout(0),
      bytes_total(0),
      bytes_head(0),
      bytes_head_decomp(0),
      bytes_body(0),
      status(),
      max_resp_time_us(0),
      min_resp_time_us(0xFFFFFFFFFFFFFFFE) {}

Stream::Stream() : req_stat {}, status_success(-1) {}

namespace
{
void print_version(std::ostream& out)
{
    out << "h2load nghttp2/" NGHTTP2_VERSION << std::endl;
}
} // namespace

namespace
{
void print_usage(std::ostream& out)
{
    out << R"(Usage: h2loadrunner [OPTIONS]... [URI]...
benchmarking tool for HTTP1.x / HTTP/2 server)"
        << std::endl;
}
} // namespace

namespace
{
constexpr char DEFAULT_NPN_LIST[] = "h2,h2-16,h2-14,http/1.1";
} // namespace

namespace
{
constexpr char UNIX_PATH_PREFIX[] = "unix:";
} // namespace

namespace
{
void print_help(std::ostream &out) {
  print_usage(out);

  Config config;

  out << R"(
  <URI>       Specify URI to access.   Multiple URIs can be specified.
              URIs are used  in this order for each  client.  All URIs
              are used, then  first URI is used and then  2nd URI, and
              so  on.  The  scheme, host  and port  in the  subsequent
              URIs, if present,  are ignored.  Those in  the first URI
              are used solely.  Definition of a base URI overrides all
              scheme, host or port values.
Options:
  -n, --requests=<N>
              Number of  requests across all  clients.  If it  is used
              with --timing-script-file option,  this option specifies
              the number of requests  each client performs rather than
              the number of requests  across all clients.  This option
              is ignored if timing-based  benchmarking is enabled (see
              --duration option).
              Default: )"
      << config.nreqs << R"(
  -c, --clients=<N>
              Number  of concurrent  clients.   With  -r option,  this
              specifies the maximum number of connections to be made.
              Default: )"
      << config.nclients << R"(
  -t, --threads=<N>
              Number of native threads.
              Default: )"
      << config.nthreads << R"(
  -i, --input-file=<PATH>
              Path of a file with multiple URIs are separated by EOLs.
              This option will disable URIs getting from command-line.
              If '-' is given as <PATH>, URIs will be read from stdin.
              URIs are used  in this order for each  client.  All URIs
              are used, then  first URI is used and then  2nd URI, and
              so  on.  The  scheme, host  and port  in the  subsequent
              URIs, if present,  are ignored.  Those in  the first URI
              are used solely.  Definition of a base URI overrides all
              scheme, host or port values.
  -m, --max-concurrent-streams=<N>
              Max  concurrent  streams  to issue  per  session.   When
              http/1.1  is used,  this  specifies the  number of  HTTP
              pipelining requests in-flight.
              Default: 1
  -w, --window-bits=<N>
              Sets the stream level initial window size to (2**<N>)-1.
              Default: )"
      << config.window_bits << R"(
  -W, --connection-window-bits=<N>
              Sets  the  connection  level   initial  window  size  to
              (2**<N>)-1.
              Default: )"
      << config.connection_window_bits << R"(
  -H, --header=<HEADER>
              Add/Override a header to the requests.
  --ciphers=<SUITE>
              Set allowed  cipher list.  The  format of the  string is
              described in OpenSSL ciphers(1).
              Default: )"
      << config.ciphers << R"(
  -p, --no-tls-proto=<PROTOID>
              Specify ALPN identifier of the  protocol to be used when
              accessing http URI without SSL/TLS.
              Available protocols: )"
      << NGHTTP2_CLEARTEXT_PROTO_VERSION_ID << R"( and )" << NGHTTP2_H1_1 << R"(
              Default: )"
      << NGHTTP2_CLEARTEXT_PROTO_VERSION_ID << R"(
  -d, --data=<PATH>
              Post FILE to  server.  The request method  is changed to
              POST.   For  http/1.1 connection,  if  -d  is used,  the
              maximum number of in-flight pipelined requests is set to
              1.
  -r, --rate=<N>
              Specifies  the  fixed  rate  at  which  connections  are
              created.   The   rate  must   be  a   positive  integer,
              representing the  number of  connections to be  made per
              rate period.   The maximum  number of connections  to be
              made  is  given  in  -c   option.   This  rate  will  be
              distributed among  threads as  evenly as  possible.  For
              example,  with   -t2  and   -r4,  each  thread   gets  2
              connections per period.  When the rate is 0, the program
              will run  as it  normally does, creating  connections at
              whatever variable rate it  wants.  The default value for
              this option is 0.  -r and -D are mutually exclusive.
  --rate-period=<DURATION>
              Specifies the time  period between creating connections.
              The period  must be a positive  number, representing the
              length of the period in time.  This option is ignored if
              the rate option is not used.  The default value for this
              option is 1s.
  -D, --duration=<DURATION>
              Specifies the main duration for the measurements in case
              of timing-based  benchmarking.  -D  and -r  are mutually
              exclusive.
  --warm-up-time=<DURATION>
              Specifies the  time  period  before  starting the actual
              measurements, in  case  of  timing-based benchmarking.
              Needs to provided along with -D option.
  -T, --connection-active-timeout=<DURATION>
              Specifies  the maximum  time that  h2load is  willing to
              keep a  connection open,  regardless of the  activity on
              said connection.  <DURATION> must be a positive integer,
              specifying the amount of time  to wait.  When no timeout
              value is  set (either  active or inactive),  h2load will
              keep  a  connection  open indefinitely,  waiting  for  a
              response.
  -N, --connection-inactivity-timeout=<DURATION>
              Specifies the amount  of time that h2load  is willing to
              wait to see activity  on a given connection.  <DURATION>
              must  be a  positive integer,  specifying the  amount of
              time  to wait.   When no  timeout value  is set  (either
              active or inactive), h2load  will keep a connection open
              indefinitely, waiting for a response.
  --timing-script-file=<PATH>
              Path of a file containing one or more lines separated by
              EOLs.  Each script line is composed of two tab-separated
              fields.  The first field represents the time offset from
              the start of execution, expressed as a positive value of
              milliseconds  with microsecond  resolution.  The  second
              field represents the URI.  This option will disable URIs
              getting from  command-line.  If '-' is  given as <PATH>,
              script lines will be read  from stdin.  Script lines are
              used in order for each client.   If -n is given, it must
              be less  than or  equal to the  number of  script lines,
              larger values are clamped to the number of script lines.
              If -n is not given,  the number of requests will default
              to the  number of  script lines.   The scheme,  host and
              port defined in  the first URI are  used solely.  Values
              contained  in  other  URIs,  if  present,  are  ignored.
              Definition of a  base URI overrides all  scheme, host or
              port   values.   --timing-script-file   and  --rps   are
              mutually exclusive.
  -B, --base-uri=(<URI>|unix:<PATH>)
              Specify URI from which the scheme, host and port will be
              used  for  all requests.   The  base  URI overrides  all
              values  defined either  at  the command  line or  inside
              input files.  If argument  starts with "unix:", then the
              rest  of the  argument will  be treated  as UNIX  domain
              socket path.   The connection is made  through that path
              instead of TCP.   In this case, scheme  is inferred from
              the first  URI appeared  in the  command line  or inside
              input files as usual.
  --npn-list=<LIST>
              Comma delimited list of  ALPN protocol identifier sorted
              in the  order of preference.  That  means most desirable
              protocol comes  first.  This  is used  in both  ALPN and
              NPN.  The parameter must be  delimited by a single comma
              only  and any  white spaces  are  treated as  a part  of
              protocol string.
              Default: )"
      << DEFAULT_NPN_LIST << R"(
  --h1        Short        hand         for        --npn-list=http/1.1
              --no-tls-proto=http/1.1,    which   effectively    force
              http/1.1 for both http and https URI.
  --header-table-size=<SIZE>
              Specify decoder header table size.
              Default: )"
      << util::utos_unit(config.header_table_size) << R"(
  --encoder-header-table-size=<SIZE>
              Specify encoder header table size.  The decoder (server)
              specifies  the maximum  dynamic table  size it  accepts.
              Then the negotiated dynamic table size is the minimum of
              this option value and the value which server specified.
              Default: )"
      << util::utos_unit(config.encoder_header_table_size) << R"(
  --log-file=<PATH>
              Write per-request information to a file as tab-separated
              columns: start  time as  microseconds since  epoch; HTTP
              status code;  microseconds until end of  response.  More
              columns may be added later.  Rows are ordered by end-of-
              response  time when  using  one worker  thread, but  may
              appear slightly  out of order with  multiple threads due
              to buffering.  Status code is -1 for failed streams.
  --connect-to=<HOST>[:<PORT>]
              Host and port to connect  instead of using the authority
              in <URI>.
  --rps=<N>   Specify request  per second for each  client.  --rps and
              --timing-script-file are mutually exclusive.
  --crud-request-variable-name=<VARIABLE-NAME>
              Specify the name of the variable to be  replaced in  the
              request-URI and the data file. When h2load runs, it will
              start  from  request-variable-value-start  to  request-\
              variable-value-end, every  time a value is  picked up to
              replace  the  VARIABLE-NAME  in request-URI and the data
              file.  This is  repeated  until  the  load test is done.
              This feature is  useful for the case  where a user ID is
              part of the URI and data, e.g., CURD based on user ID.
  --crud-request-variable-value-start=<start-value>
              An integer to specify the start of the range.
  --crud-request-variable-value-end=<end-value>
              An integer to specify the end of the range.
  --crud-request-variable-range-slicing
              Slice the  variable range,  each  client get a sub range
              Otherwise, each client runs with full variable range
  --crud-create-method=<METHOD>
              HTTP METHOD for Create operationto override the  default
              method (GET/POST)
  --crud-read-method=<METHOD>
              HTTP METHOD for CRUD Read operation
  --crud-update-method=<METHOD>
              HTTP METHOD for CRUD Update operation
  --crud-delete-method=<METHOD>
              HTTP METHOD for CRUD Delete operation
  --crud-resource-header-name=<header name>
              name of the  header  which contains the resource created
  --crud-create-data-file=<file name>
              name of the data file for  Create operation. If present,
              this overrides the file name provided in 'data' option
  --crud-update-data-file=<file name>
              name of the data file for Update operation.
  --stream-timeout-interval-ms=<timeout value in ms>
              request time out  value.  After  timeout,  RST_STREAM is
              sent by h2load. Default 5000.
  --rps-input-file=<PATH>
              A file specifying rps number.  It is useful when dynamic
              change of rps is needed.
  --config-file=<PATH>
              A JSON file specifying the configurations needed.
  -v, --verbose
              Output debug information.
  --version   Display version information and exit.
  -h, --help  Display this help and exit.

--

  The <SIZE> argument is an integer and an optional unit (e.g., 10K is
  10 * 1024).  Units are K, M and G (powers of 1024).

  The <DURATION> argument is an integer and an optional unit (e.g., 1s
  is 1 second and 500ms is 500 milliseconds).  Units are h, m, s or ms
  (hours, minutes, seconds and milliseconds, respectively).  If a unit
  is omitted, a second is used as unit.)"
      << std::endl;
}

} // namespace


int main(int argc, char** argv)
{
    tls::libssl_init();
    auto status = ares_library_init(ARES_LIB_INIT_ALL);
    if (status != ARES_SUCCESS)
    {
        std::cout<<"ares_library_init failed"<<std::endl;
        exit(EXIT_FAILURE);
        return 1;
    }
#ifndef NOTHREADS
    tls::LibsslGlobalLock lock;
#endif // NOTHREADS

    std::string datafile;
    std::string logfile;
    bool nreqs_set_manually = false;
    while (1)
    {
        static int flag = 0;
        constexpr static option long_options[] =
        {
            {"requests", required_argument, nullptr, 'n'},
            {"clients", required_argument, nullptr, 'c'},
            {"data", required_argument, nullptr, 'd'},
            {"threads", required_argument, nullptr, 't'},
            {"max-concurrent-streams", required_argument, nullptr, 'm'},
            {"window-bits", required_argument, nullptr, 'w'},
            {"connection-window-bits", required_argument, nullptr, 'W'},
            {"input-file", required_argument, nullptr, 'i'},
            {"header", required_argument, nullptr, 'H'},
            {"no-tls-proto", required_argument, nullptr, 'p'},
            {"verbose", no_argument, nullptr, 'v'},
            {"help", no_argument, nullptr, 'h'},
            {"version", no_argument, &flag, 1},
            {"ciphers", required_argument, &flag, 2},
            {"rate", required_argument, nullptr, 'r'},
            {"connection-active-timeout", required_argument, nullptr, 'T'},
            {"connection-inactivity-timeout", required_argument, nullptr, 'N'},
            {"duration", required_argument, nullptr, 'D'},
            {"timing-script-file", required_argument, &flag, 3},
            {"base-uri", required_argument, nullptr, 'B'},
            {"npn-list", required_argument, &flag, 4},
            {"rate-period", required_argument, &flag, 5},
            {"h1", no_argument, &flag, 6},
            {"header-table-size", required_argument, &flag, 7},
            {"encoder-header-table-size", required_argument, &flag, 8},
            {"warm-up-time", required_argument, &flag, 9},
            {"log-file", required_argument, &flag, 10},
            {"connect-to", required_argument, &flag, 11},
            {"rps", required_argument, &flag, 12},
            {"crud-create-method", required_argument, &flag, 13},
            {"crud-read-method", required_argument, &flag, 14},
            {"crud-update-method", required_argument, &flag, 15},
            {"crud-delete-method", required_argument, &flag, 16},
            {"crud-resource-header-name", required_argument, &flag, 17},
            {"crud-create-data-file", required_argument, &flag, 18},
            {"crud-update-data-file", required_argument, &flag, 19},
            {"crud-request-variable-name", required_argument, &flag, 20},
            {"crud-request-variable-value-start", required_argument, &flag, 21},
            {"crud-request-variable-value-end", required_argument, &flag, 22},
            {"stream-timeout-interval-ms", required_argument, &flag, 23},
            {"rps-input-file", required_argument, &flag, 24},
            {"config-file", required_argument, &flag, 25},
            {"crud-request-variable-range-slicing", required_argument, &flag, 26},
            {nullptr, 0, nullptr, 0}
        };
        int option_index = 0;
        auto c = getopt_long(argc, argv,
                             "hvW:c:d:m:n:p:t:w:H:i:r:T:N:D:B:", long_options,
                             &option_index);
        if (c == -1)
        {
            break;
        }
        switch (c)
        {
            case 'n':
                config.nreqs = strtoul(optarg, nullptr, 10);
                nreqs_set_manually = true;
                break;
            case 'c':
                config.nclients = strtoul(optarg, nullptr, 10);
                break;
            case 'd':
                datafile = optarg;
                break;
            case 't':
#ifdef NOTHREADS
                std::cerr << "-t: WARNING: Threading disabled at build time, "
                          << "no threads created." << std::endl;
#else
                config.nthreads = strtoul(optarg, nullptr, 10);
#endif // NOTHREADS
                break;
            case 'm':
                config.max_concurrent_streams = strtoul(optarg, nullptr, 10);
                break;
            case 'w':
            case 'W':
            {
                errno = 0;
                char* endptr = nullptr;
                auto n = strtoul(optarg, &endptr, 10);
                if (errno == 0 && *endptr == '\0' && n < 31)
                {
                    if (c == 'w')
                    {
                        config.window_bits = n;
                    }
                    else
                    {
                        config.connection_window_bits = n;
                    }
                }
                else
                {
                    std::cerr << "-" << static_cast<char>(c)
                              << ": specify the integer in the range [0, 30], inclusive"
                              << std::endl;
                    exit(EXIT_FAILURE);
                }
                break;
            }
            case 'H':
            {
                char* header = optarg;
                // Skip first possible ':' in the header name
                char* value = strchr(optarg + 1, ':');
                if (!value || (header[0] == ':' && header + 1 == value))
                {
                    std::cerr << "-H: invalid header: " << optarg << std::endl;
                    exit(EXIT_FAILURE);
                }
                *value = 0;
                value++;
                //while (isspace(*value))
                //{
                //    value++;
                //}
                if (*value == 0)
                {
                    // This could also be a valid case for suppressing a header
                    // similar to curl
                    std::cerr << "-H: invalid header - value missing: " << optarg
                              << std::endl;
                    exit(EXIT_FAILURE);
                }
                // Note that there is no processing currently to handle multiple
                // message-header fields with the same field name
                config.custom_headers.emplace_back(header, value);
                util::inp_strlower(config.custom_headers.back().name);
                break;
            }
            case 'i':
                config.ifile = optarg;
                break;
            case 'p':
            {
                auto proto = StringRef {optarg};
                if (util::strieq(StringRef::from_lit(NGHTTP2_CLEARTEXT_PROTO_VERSION_ID),
                                 proto))
                {
                    config.no_tls_proto = Config::PROTO_HTTP2;
                }
                else if (util::strieq(NGHTTP2_H1_1, proto))
                {
                    config.no_tls_proto = Config::PROTO_HTTP1_1;
                }
                else
                {
                    std::cerr << "-p: unsupported protocol " << proto << std::endl;
                    exit(EXIT_FAILURE);
                }
                break;
            }
            case 'r':
                config.rate = strtoul(optarg, nullptr, 10);
                if (config.rate == 0)
                {
                    std::cerr << "-r: the rate at which connections are made "
                              << "must be positive." << std::endl;
                    exit(EXIT_FAILURE);
                }
                break;
            case 'T':
                config.conn_active_timeout = util::parse_duration_with_unit(optarg);
                if (!std::isfinite(config.conn_active_timeout))
                {
                    std::cerr << "-T: bad value for the conn_active_timeout wait time: "
                              << optarg << std::endl;
                    exit(EXIT_FAILURE);
                }
                break;
            case 'N':
                config.conn_inactivity_timeout = util::parse_duration_with_unit(optarg);
                if (!std::isfinite(config.conn_inactivity_timeout))
                {
                    std::cerr << "-N: bad value for the conn_inactivity_timeout wait time: "
                              << optarg << std::endl;
                    exit(EXIT_FAILURE);
                }
                break;
            case 'B':
            {
                auto arg = StringRef {optarg};
                config.base_uri = "";
                config.base_uri_unix = false;

                if (util::istarts_with_l(arg, UNIX_PATH_PREFIX))
                {
                    // UNIX domain socket path
                    sockaddr_un un;

                    auto path = StringRef {std::begin(arg) + str_size(UNIX_PATH_PREFIX),
                                           std::end(arg)
                                          };

                    if (path.size() == 0 || path.size() + 1 > sizeof(un.sun_path))
                    {
                        std::cerr << "--base-uri: invalid UNIX domain socket path: " << arg
                                  << std::endl;
                        exit(EXIT_FAILURE);
                    }

                    config.base_uri_unix = true;

                    auto& unix_addr = config.unix_addr;
                    std::copy(std::begin(path), std::end(path), unix_addr.sun_path);
                    unix_addr.sun_path[path.size()] = '\0';
                    unix_addr.sun_family = AF_UNIX;

                    break;
                }

                if (!parse_base_uri(arg, config))
                {
                    std::cerr << "--base-uri: invalid base URI: " << arg << std::endl;
                    exit(EXIT_FAILURE);
                }

                config.base_uri = arg.str();
                break;
            }
            case 'D':
                config.duration = util::parse_duration_with_unit(optarg);
                if (!std::isfinite(config.duration))
                {
                    std::cerr << "-D: value error " << optarg << std::endl;
                    exit(EXIT_FAILURE);
                }
                break;
            case 'v':
                config.verbose = true;
                break;
            case 'h':
                print_help(std::cout);
                exit(EXIT_SUCCESS);
            case '?':
                util::show_candidates(argv[optind - 1], long_options);
                exit(EXIT_FAILURE);
            case 0:
                switch (flag)
                {
                    case 1:
                        // version option
                        print_version(std::cout);
                        exit(EXIT_SUCCESS);
                    case 2:
                        // ciphers option
                        config.ciphers = optarg;
                        break;
                    case 3:
                        // timing-script option
                        config.ifile = optarg;
                        config.timing_script = true;
                        break;
                    case 4:
                        // npn-list option
                        config.npn_list = util::parse_config_str_list(StringRef {optarg});
                        break;
                    case 5:
                        // rate-period
                        config.rate_period = util::parse_duration_with_unit(optarg);
                        if (!std::isfinite(config.rate_period))
                        {
                            std::cerr << "--rate-period: value error " << optarg << std::endl;
                            exit(EXIT_FAILURE);
                        }
                        break;
                    case 6:
                        // --h1
                        config.npn_list =
                            util::parse_config_str_list(StringRef::from_lit("http/1.1"));
                        config.no_tls_proto = Config::PROTO_HTTP1_1;
                        break;
                    case 7:
                        // --header-table-size
                        if (parse_header_table_size(config.header_table_size,
                                                    "header-table-size", optarg) != 0)
                        {
                            exit(EXIT_FAILURE);
                        }
                        break;
                    case 8:
                        // --encoder-header-table-size
                        if (parse_header_table_size(config.encoder_header_table_size,
                                                    "encoder-header-table-size", optarg) != 0)
                        {
                            exit(EXIT_FAILURE);
                        }
                        break;
                    case 9:
                        // --warm-up-time
                        config.warm_up_time = util::parse_duration_with_unit(optarg);
                        if (!std::isfinite(config.warm_up_time))
                        {
                            std::cerr << "--warm-up-time: value error " << optarg << std::endl;
                            exit(EXIT_FAILURE);
                        }
                        break;
                    case 10:
                        // --log-file
                        logfile = optarg;
                        break;
                    case 11:
                    {
                        // --connect-to
                        auto p = util::split_hostport(StringRef {optarg});
                        int64_t port = 0;
                        if (p.first.empty() ||
                            (!p.second.empty() && (port = util::parse_uint(p.second)) == -1))
                        {
                            std::cerr << "--connect-to: Invalid value " << optarg << std::endl;
                            exit(EXIT_FAILURE);
                        }
                        config.connect_to_host = p.first.str();
                        config.connect_to_port = port;
                        break;
                    }
                    case 12:
                    {
                        char* end;
                        auto v = std::strtod(optarg, &end);
                        if (end == optarg || *end != '\0' || !std::isfinite(v) ||
                            1. / v < 1e-6)
                        {
                            std::cerr << "--rps: Invalid value " << optarg << std::endl;
                            exit(EXIT_FAILURE);
                        }
                        config.rps = v;
                        break;
                    }
                    case 13:
                    {
                        // create-method
                        config.crud_create_method = optarg;
                        break;
                    }
                    case 14:
                    {
                        // read-method
                        config.crud_read_method = optarg;
                        break;
                    }
                    case 15:
                    {
                        // update-method
                        config.crud_update_method = optarg;
                        break;
                    }
                    case 16:
                    {
                        // delete-method
                        config.crud_delete_method = optarg;
                        break;
                    }
                    case 17:
                    {
                        // crud_resource_header_name
                        config.crud_resource_header_name = optarg;
                        break;
                    }
                    case 18:
                    {
                        // crud_create_data_file_name
                        config.crud_create_data_file_name = optarg;
                        break;
                    }
                    case 19:
                    {
                        // crud_update_data_file_name
                        config.crud_update_data_file_name = optarg;
                        break;
                    }
                    case 20:
                    {
                        config.req_variable_name = optarg;
                    }
                    break;
                    case 21:
                    {
                        config.req_variable_start = strtoul(optarg, nullptr, 10);
                    }
                    break;
                    case 22:
                    {
                        config.req_variable_end = strtoul(optarg, nullptr, 10);
                    }
                    break;
                    case 23:
                    {
                        config.stream_timeout_in_ms = (uint16_t)strtoul(optarg, nullptr, 10);
                    }
                    break;
                    case 24:
                    {
                        config.rps_file = optarg;
                    }
                    break;
                    case 25:
                    {
                        std::string config_file_name = optarg;
                        std::ifstream buffer(config_file_name);
                        std::string jsonStr((std::istreambuf_iterator<char>(buffer)),
                                            std::istreambuf_iterator<char>());
                        staticjson::ParseStatus result;
                        if (!staticjson::from_json_string(jsonStr.c_str(), &config.json_config_schema, &result))
                        {
                            std::cout << "error reading config file:" << result.description() << std::endl;
                            exit(EXIT_FAILURE);
                        }
                        util::inp_strlower(config.json_config_schema.host);
                        util::inp_strlower(config.json_config_schema.schema);
                        std::cout << "Use configuration from JSON:" << std::endl << staticjson::to_pretty_json_string(
                                      config.json_config_schema) << std::endl;
                        assert(config.json_config_schema.scenario[0].uri.typeOfAction == "input");

                        for (auto& request : config.json_config_schema.scenario)
                        {
                            for (auto& header_with_value : request.additonalHeaders)
                            {
                                size_t t = header_with_value.find(":", 1);
                                if ((t == std::string::npos) ||
                                    (header_with_value[0] == ':' && 1 == t))
                                {
                                    std::cerr << "invalid header, no name: " << header_with_value << std::endl;
                                    continue;
                                }
                                std::string header_name = header_with_value.substr(0, t);
                                std::string header_value = header_with_value.substr(t + 1);
                                /*
                                header_value.erase(header_value.begin(), std::find_if(header_value.begin(), header_value.end(),
                                                                                      [](unsigned char ch)
                                {
                                    return !std::isspace(ch);
                                }));
                                */

                                if (header_value.empty())
                                {
                                    std::cerr << "invalid header - no value: " << header_with_value
                                              << std::endl;
                                    continue;
                                }
                                request.headers_in_map[header_name] = header_value;
                            }
                            if (request.payload.size())
                            {
                                std::ifstream f(request.payload);
                                if (f.good())
                                {
                                    std::string payloadStr((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                                    request.payload = payloadStr;
                                }
                            }
                            if (request.luaScript.size())
                            {
                                std::ifstream f(request.luaScript);
                                if (f.good())
                                {
                                    std::string luaScriptStr((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                                    request.luaScript = luaScriptStr;
                                }
                            }
                            if (request.uri.typeOfAction == "input")
                            {
                                http_parser_url u {};
                                if (http_parser_parse_url(request.uri.input.c_str(), request.uri.input.size(), 0, &u) != 0)
                                {
                                    std::cerr << "invalid URI given: " << request.uri.input << std::endl;
                                    exit(EXIT_FAILURE);
                                }
                                request.path = get_reqline(request.uri.input.c_str(), u);
                                if (util::has_uri_field(u, UF_SCHEMA) && util::has_uri_field(u, UF_HOST))
                                {
                                    request.schema = util::get_uri_field(request.uri.input.c_str(), u, UF_SCHEMA).str();
                                    util::inp_strlower(request.schema);
                                    request.authority = util::get_uri_field(request.uri.input.c_str(), u, UF_HOST).str();
                                    util::inp_strlower(request.authority);
                                    if (util::has_uri_field(u, UF_PORT))
                                    {
                                        request.authority.append(":").append(util::utos(u.port));
                                    }
                                }
                            }
                        }
                        populate_config_from_json(config);
                    }
                    break;
                    case 26:
                    {
                        config.variable_range_slicing = true;
                    }
                    break;
                }
                break;
            default:
                break;
        }
    }

    if (argc == optind)
    {
        if (config.ifile.empty() && (config.host.empty() || config.scheme.empty()))
        {
            std::cerr << "no URI or input file given" << std::endl;
            exit(EXIT_FAILURE);
        }
    }

    if (config.nclients == 0)
    {
        std::cerr << "-c: the number of clients must be strictly greater than 0."
                  << std::endl;
        exit(EXIT_FAILURE);
    }

    if (config.npn_list.empty())
    {
        config.npn_list =
            util::parse_config_str_list(StringRef::from_lit(DEFAULT_NPN_LIST));
    }

    // serialize the APLN tokens
    for (auto& proto : config.npn_list)
    {
        proto.insert(proto.begin(), static_cast<unsigned char>(proto.size()));
    }

    if (config.ifile.empty())
    {
        std::vector<std::string> uris;
        std::copy(&argv[optind], &argv[argc], std::back_inserter(uris));
        if (uris.empty() && config.host.size() && config.scheme.size())
        {
            // no exit
        }
        else
        {
            config.reqlines = parse_uris(std::begin(uris), std::end(uris), config);
        }
    }
    else
    {
        std::vector<std::string> uris;
        if (!config.timing_script)
        {
            if (config.ifile == "-")
            {
                uris = read_uri_from_file(std::cin);
            }
            else
            {
                std::ifstream infile(config.ifile);
                if (!infile)
                {
                    std::cerr << "cannot read input file: " << config.ifile << std::endl;
                    exit(EXIT_FAILURE);
                }

                uris = read_uri_from_file(infile);
            }
        }
        else
        {
            if (config.ifile == "-")
            {
                read_script_from_file(std::cin, config.timings, uris);
            }
            else
            {
                std::ifstream infile(config.ifile);
                if (!infile)
                {
                    std::cerr << "cannot read input file: " << config.ifile << std::endl;
                    exit(EXIT_FAILURE);
                }

                read_script_from_file(infile, config.timings, uris);
            }

            if (nreqs_set_manually)
            {
                if (config.nreqs > uris.size())
                {
                    std::cerr << "-n: the number of requests must be less than or equal "
                              "to the number of timing script entries. Setting number "
                              "of requests to "
                              << uris.size() << std::endl;

                    config.nreqs = uris.size();
                }
            }
            else
            {
                config.nreqs = uris.size();
            }
        }

        config.reqlines = parse_uris(std::begin(uris), std::end(uris), config);
    }

    if (config.reqlines.empty() && (config.host.empty() || config.scheme.empty()))
    {
        std::cerr << "No URI given" << std::endl;
        exit(EXIT_FAILURE);
    }

    if (config.is_timing_based_mode() && config.is_rate_mode())
    {
        std::cerr << "-r, -D: they are mutually exclusive." << std::endl;
        exit(EXIT_FAILURE);
    }

    if (config.timing_script && config.rps_enabled())
    {
        std::cerr << "--timing-script-file, --rps: they are mutually exclusive."
                  << std::endl;
        exit(EXIT_FAILURE);
    }

    if (config.nreqs == 0 && !config.is_timing_based_mode())
    {
        std::cerr << "-n: the number of requests must be strictly greater than 0 "
                  "if timing-based test is not being run."
                  << std::endl;
        exit(EXIT_FAILURE);
    }

    if (config.max_concurrent_streams == 0)
    {
        std::cerr << "-m: the max concurrent streams must be strictly greater "
                  << "than 0." << std::endl;
        exit(EXIT_FAILURE);
    }

    if (config.nthreads == 0)
    {
        std::cerr << "-t: the number of threads must be strictly greater than 0."
                  << std::endl;
        exit(EXIT_FAILURE);
    }

    if (config.nthreads > std::thread::hardware_concurrency())
    {
        std::cerr << "-t: warning: the number of threads is greater than hardware "
                  << "cores." << std::endl;
    }

    // With timing script, we don't distribute config.nreqs to each
    // client or thread.
    if (!config.timing_script && config.nreqs < config.nclients &&
        !config.is_timing_based_mode())
    {
        std::cerr << "-n, -c: the number of requests must be greater than or "
                  << "equal to the clients." << std::endl;
        exit(EXIT_FAILURE);
    }

    if (config.nclients < config.nthreads)
    {
        std::cerr << "-c, -t: the number of clients must be greater than or equal "
                  << "to the number of threads." << std::endl;
        exit(EXIT_FAILURE);
    }

    if (config.is_timing_based_mode())
    {
        config.nreqs = 0;
    }

    if (config.is_rate_mode())
    {
        if (config.rate < config.nthreads)
        {
            std::cerr << "-r, -t: the connection rate must be greater than or equal "
                      << "to the number of threads." << std::endl;
            exit(EXIT_FAILURE);
        }

        if (config.rate > config.nclients)
        {
            std::cerr << "-r, -c: the connection rate must be smaller than or equal "
                      "to the number of clients."
                      << std::endl;
            exit(EXIT_FAILURE);
        }
    }

    if (!datafile.empty())
    {
        config.data_fd = open(datafile.c_str(), O_RDONLY | O_BINARY);
        if (config.data_fd == -1)
        {
            std::cerr << "-d: Could not open file " << datafile << std::endl;
            exit(EXIT_FAILURE);
        }
        struct stat data_stat;
        if (fstat(config.data_fd, &data_stat) == -1)
        {
            std::cerr << "-d: Could not stat file " << datafile << std::endl;
            exit(EXIT_FAILURE);
        }
        config.data_length = data_stat.st_size;
    }

    if (!logfile.empty())
    {
        close(config.log_fd);
        config.log_fd = open(logfile.c_str(), O_WRONLY | O_CREAT | O_APPEND,
                             S_IRUSR | S_IWUSR | S_IRGRP);
        if (config.log_fd == -1)
        {
            std::cerr << "--log-file: Could not open file " << logfile << std::endl;
            exit(EXIT_FAILURE);
        }
    }

    struct sigaction act {};
    act.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &act, nullptr);

    auto ssl_ctx = SSL_CTX_new(SSLv23_client_method());
    if (!ssl_ctx)
    {
        std::cerr << "Failed to create SSL_CTX: "
                  << ERR_error_string(ERR_get_error(), nullptr) << std::endl;
        exit(EXIT_FAILURE);
    }

    auto ssl_opts = (SSL_OP_ALL & ~SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS) |
                    SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION |
                    SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION;

    SSL_CTX_set_options(ssl_ctx, ssl_opts);
    SSL_CTX_set_mode(ssl_ctx, SSL_MODE_AUTO_RETRY);
    SSL_CTX_set_mode(ssl_ctx, SSL_MODE_RELEASE_BUFFERS);

    if (nghttp2::tls::ssl_ctx_set_proto_versions(
            ssl_ctx, nghttp2::tls::NGHTTP2_TLS_MIN_VERSION,
            nghttp2::tls::NGHTTP2_TLS_MAX_VERSION) != 0)
    {
        std::cerr << "Could not set TLS versions" << std::endl;
        exit(EXIT_FAILURE);
    }

    if (SSL_CTX_set_cipher_list(ssl_ctx, config.ciphers.c_str()) == 0)
    {
        std::cerr << "SSL_CTX_set_cipher_list with " << config.ciphers
                  << " failed: " << ERR_error_string(ERR_get_error(), nullptr)
                  << std::endl;
        exit(EXIT_FAILURE);
    }

#ifndef OPENSSL_NO_NEXTPROTONEG
    SSL_CTX_set_next_proto_select_cb(ssl_ctx, client_select_next_proto_cb,
                                     &config);
#endif // !OPENSSL_NO_NEXTPROTONEG

#if OPENSSL_VERSION_NUMBER >= 0x10002000L
    std::vector<unsigned char> proto_list;
    for (const auto& proto : config.npn_list)
    {
        std::copy_n(proto.c_str(), proto.size(), std::back_inserter(proto_list));
    }

    SSL_CTX_set_alpn_protos(ssl_ctx, proto_list.data(), proto_list.size());
#endif // OPENSSL_VERSION_NUMBER >= 0x10002000L

    std::string user_agent = "h2load nghttp2/" NGHTTP2_VERSION;
    Headers shared_nva;
    shared_nva.emplace_back(":scheme", config.scheme);
    if (config.port != config.default_port)
    {
        shared_nva.emplace_back(":authority",
                                config.host + ":" + util::utos(config.port));
    }
    else
    {
        shared_nva.emplace_back(":authority", config.host);
    }
    shared_nva.emplace_back(":method", config.data_fd == -1 ? "GET" : "POST");
    shared_nva.emplace_back("user-agent", user_agent);

    // list header fields that can be overridden.
    auto override_hdrs = make_array<std::string>(":authority", ":host", ":method",
                                                 ":scheme", "user-agent");

    for (auto& kv : config.custom_headers)
    {
        if (std::find(std::begin(override_hdrs), std::end(override_hdrs),
                      kv.name) != std::end(override_hdrs))
        {
            // override header
            for (auto& nv : shared_nva)
            {
                if ((nv.name == ":authority" && kv.name == ":host") ||
                    (nv.name == kv.name))
                {
                    nv.value = kv.value;
                }
            }
        }
        else
        {
            // add additional headers
            shared_nva.push_back(kv);
        }
    }

    std::string content_length_str;
    if (config.data_fd != -1)
    {
        content_length_str = util::utos(config.data_length);
    }

    auto method_it =
        std::find_if(std::begin(shared_nva), std::end(shared_nva),
                     [](const Header & nv)
    {
        return nv.name == ":method";
    });
    assert(method_it != std::end(shared_nva));

    config.h1reqs.reserve(config.reqlines.size());
    config.nva.reserve(config.reqlines.size());

    for (auto& req : config.reqlines)
    {
        // For HTTP/1.1
        auto h1req = (*method_it).value;
        h1req += ' ';
        h1req += req;
        h1req += " HTTP/1.1\r\n";
        for (auto& nv : shared_nva)
        {
            if (nv.name == ":authority")
            {
                h1req += "Host: ";
                h1req += nv.value;
                h1req += "\r\n";
                continue;
            }
            if (nv.name[0] == ':')
            {
                continue;
            }
            h1req += nv.name;
            h1req += ": ";
            h1req += nv.value;
            h1req += "\r\n";
        }

        if (!content_length_str.empty())
        {
            h1req += "Content-Length: ";
            h1req += content_length_str;
            h1req += "\r\n";
        }
        h1req += "\r\n";

        config.h1reqs.push_back(std::move(h1req));

        // For nghttp2
        std::vector<nghttp2_nv> nva;
        // 2 for :path, and possible content-length
        nva.reserve(2 + shared_nva.size());

        nva.push_back(http2::make_nv_ls(":path", req));

        for (auto& nv : shared_nva)
        {
            nva.push_back(http2::make_nv(nv.name, nv.value, false));
        }

        if (!content_length_str.empty())
        {
            nva.push_back(http2::make_nv(StringRef::from_lit("content-length"),
                                         StringRef {content_length_str}));
        }

        config.nva.push_back(std::move(nva));
    }


    // Don't DOS our server!
    if (config.host == "nghttp2.org")
    {
        std::cerr << "Using h2load against public server " << config.host
                  << " should be prohibited." << std::endl;
        exit(EXIT_FAILURE);
    }

    if (!config.json_config_schema.scenario.size())
    {
        convert_CRUD_operation_to_Json_scenarios(config);
    }

    if (config.json_config_schema.scenario.size() && config.custom_headers.size())
    {
        insert_customized_headers_to_Json_scenarios(config);
    }

    normalize_request_templates(&config);

    std::cout << "Scenario to run:" << std::endl << staticjson::to_pretty_json_string(config.json_config_schema)
              <<std::endl;

    tokenize_path_and_payload_for_fast_var_replace(config);

    resolve_host(config);

    std::cout << "starting benchmark..." << std::endl;

    std::vector<std::unique_ptr<Worker>> workers;
    workers.reserve(config.nthreads);

#ifndef NOTHREADS
    size_t nreqs_per_thread = 0;
    ssize_t nreqs_rem = 0;

    if (!config.timing_script)
    {
        nreqs_per_thread = config.nreqs / config.nthreads;
        nreqs_rem = config.nreqs % config.nthreads;
    }

    size_t nclients_per_thread = config.nclients / config.nthreads;
    ssize_t nclients_rem = config.nclients % config.nthreads;

    size_t rate_per_thread = config.rate / config.nthreads;
    ssize_t rate_per_thread_rem = config.rate % config.nthreads;

    size_t max_samples_per_thread =
        std::max(static_cast<size_t>(256), MAX_SAMPLES / config.nthreads);

    std::mutex mu;
    std::condition_variable cv;
    auto ready = false;

    std::vector<std::future<void>> futures;
    for (size_t i = 0; i < config.nthreads; ++i)
    {
        auto rate = rate_per_thread;
        if (rate_per_thread_rem > 0)
        {
            --rate_per_thread_rem;
            ++rate;
        }
        auto nclients = nclients_per_thread;
        if (nclients_rem > 0)
        {
            --nclients_rem;
            ++nclients;
        }

        size_t nreqs;
        if (config.timing_script)
        {
            // With timing script, each client issues config.nreqs requests.
            // We divide nreqs by number of clients in Worker ctor to
            // distribute requests to those clients evenly, so multiply
            // config.nreqs here by config.nclients.
            nreqs = config.nreqs * nclients;
        }
        else
        {
            nreqs = nreqs_per_thread;
            if (nreqs_rem > 0)
            {
                --nreqs_rem;
                ++nreqs;
            }
        }

        workers.push_back(create_worker(i, ssl_ctx, nreqs, nclients, rate,
                                        max_samples_per_thread, config));
        auto& worker = workers.back();
        futures.push_back(
            std::async(std::launch::async, [&worker, &mu, &cv, &ready]()
        {
            {
                std::unique_lock<std::mutex> ulk(mu);
                cv.wait(ulk, [&ready] { return ready; });
            }
            worker->run();
        }));
    }

    {
        std::lock_guard<std::mutex> lg(mu);
        ready = true;
        cv.notify_all();
    }

    auto start = std::chrono::steady_clock::now();
    std::atomic<bool> workers_stopped;
    workers_stopped = false;

    std::future<void> fu_tps =
        std::async(std::launch::async, [&workers, &workers_stopped]()
    {
        static uint32_t counter = 0;
        size_t totalReq_till_now = 0;
        size_t totalReq_success_till_now = 0;
        size_t total3xx_till_now = 0;
        size_t total4xx_till_now = 0;
        size_t total5xx_till_now = 0;
        while (!workers_stopped)
        {
            size_t total_req_till_last_interval = totalReq_till_now;
            size_t totalReq_success_till_last_interval = totalReq_success_till_now;
            size_t total3xx_till_last_interval = total3xx_till_now;
            size_t total4xx_till_last_interval = total4xx_till_now;
            size_t total5xx_till_last_interval = total5xx_till_now;
            std::this_thread::sleep_for(std::chrono::seconds(1));
            totalReq_till_now = 0;
            totalReq_success_till_now = 0;
            total3xx_till_now = 0;
            total4xx_till_now = 0;
            total5xx_till_now = 0;
            uint64_t max_resp_time_us = 0;
            uint64_t min_resp_time_us = 0xFFFFFFFFFFFFFFFE;
            for (auto& w : workers)
            {
                auto& s = w->stats;
                totalReq_till_now += s.req_done;
                totalReq_success_till_now += s.req_status_success;
                total3xx_till_now += s.status[3];
                total4xx_till_now += s.status[4];
                total5xx_till_now += s.status[5];
                max_resp_time_us = std::max(max_resp_time_us, s.max_resp_time_us.exchange(0));
                min_resp_time_us = std::min(min_resp_time_us, s.min_resp_time_us.exchange(0xFFFFFFFFFFFFFFFE));
            }
            size_t delta_TPS = totalReq_till_now - total_req_till_last_interval;
            size_t delta_TPS_success = totalReq_success_till_now - totalReq_success_till_last_interval;
            size_t delta_TPS_3xx = total3xx_till_now - total3xx_till_last_interval;
            size_t delta_TPS_4xx = total4xx_till_now - total4xx_till_last_interval;
            size_t delta_TPS_5xx = total5xx_till_now - total5xx_till_last_interval;
            auto now = std::chrono::system_clock::now();
            auto now_c = std::chrono::system_clock::to_time_t(now);
            std::cout << std::put_time(std::localtime(&now_c), "%c")
                      << ", send: " << delta_TPS
                      << ", successful: " << delta_TPS_success
                      << ", 3xx: " << delta_TPS_3xx
                      << ", 4xx: " << delta_TPS_4xx
                      << ", 5xx: " << delta_TPS_5xx
                      << ", max resp time (us): " << max_resp_time_us
                      << ", min resp time (us): " << min_resp_time_us
                      << ", successful/send: "
                      << (((double)delta_TPS_success / delta_TPS) * 100) << "%" << std::endl;
            counter++;

            if (counter == 30)
            {
                counter = 0;
                std::cout << std::put_time(std::localtime(&now_c), "%c")
                          << ", total requests sent: " << totalReq_till_now
                          << ", total successful responses: " << totalReq_success_till_now
                          << ", total 3xx: " << total3xx_till_now
                          << ", total 4xx: " << total4xx_till_now
                          << ", total 5xx: " << total5xx_till_now
                          << ", ovewrall successful rate: "
                          << (((double)totalReq_success_till_now / totalReq_till_now) * 100) << "%" << std::endl;
            }
        }
    });

    std::future<void> fu_update_rps =
        std::async(std::launch::async, [&workers_stopped]()
    {
        while (!config.rps_file.empty() && !workers_stopped)
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            std::ifstream file;
            file.open(config.rps_file);
            std::string line;
            if (!file)
            {
                std::cerr << "Error: Could not find:" << config.rps_file;
            }
            else
            {
                std::getline(file, line);
                file.close();
                char* end;
                auto v = std::strtod(line.c_str(), &end);
                if (end == line.c_str() || *end != '\0' || !std::isfinite(v) ||
                    1. / v < 1e-6)
                {
                    std::cerr << "--rps: Invalid value, skip: " << line << std::endl;
                }
                else if (v != config.rps)
                {
                    config.rps = v;
                }
            }
        }
    });


    for (auto& fut : futures)
    {
        fut.get();
    }
    workers_stopped = true;
    fu_tps.get();
    fu_update_rps.get();

#else  // NOTHREADS
    auto rate = config.rate;
    auto nclients = config.nclients;
    auto nreqs =
        config.timing_script ? config.nreqs * config.nclients : config.nreqs;

    workers.push_back(
        create_worker(0, ssl_ctx, nreqs, nclients, rate, MAX_SAMPLES, config));

    auto start = std::chrono::steady_clock::now();

    workers.back()->run();
#endif // NOTHREADS

    auto end = std::chrono::steady_clock::now();
    auto duration =
        std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    Stats stats(0, 0);
    for (const auto& w : workers)
    {
        const auto& s = w->stats;

        stats.req_todo += s.req_todo;
        stats.req_started += s.req_started;
        stats.req_done += s.req_done;
        stats.req_timedout += s.req_timedout;
        stats.req_success += s.req_success;
        stats.req_status_success += s.req_status_success;
        stats.req_failed += s.req_failed;
        stats.req_error += s.req_error;
        stats.bytes_total += s.bytes_total;
        stats.bytes_head += s.bytes_head;
        stats.bytes_head_decomp += s.bytes_head_decomp;
        stats.bytes_body += s.bytes_body;

        for (size_t i = 0; i < stats.status.size(); ++i)
        {
            stats.status[i] += s.status[i];
        }
    }

    auto ts = process_time_stats(workers);

    // Requests which have not been issued due to connection errors, are
    // counted towards req_failed and req_error.
    auto req_not_issued =
        (stats.req_todo - stats.req_status_success - stats.req_failed);
    stats.req_failed += req_not_issued;
    stats.req_error += req_not_issued;

    // UI is heavily inspired by weighttp[1] and wrk[2]
    //
    // [1] https://github.com/lighttpd/weighttp
    // [2] https://github.com/wg/wrk
    double rps = 0;
    int64_t bps = 0;
    if (duration.count() > 0)
    {
        if (config.is_timing_based_mode())
        {
            // we only want to consider the main duration if warm-up is given
            rps = stats.req_success / config.duration;
            bps = stats.bytes_total / config.duration;
        }
        else
        {
            auto secd = std::chrono::duration_cast <
                        std::chrono::duration<double, std::chrono::seconds::period >> (
                            duration);
            rps = stats.req_success / secd.count();
            bps = stats.bytes_total / secd.count();
        }
    }

    double header_space_savings = 0.;
    if (stats.bytes_head_decomp > 0)
    {
        header_space_savings =
            1. - static_cast<double>(stats.bytes_head) / stats.bytes_head_decomp;
    }

    std::cout << std::fixed << std::setprecision(2) << R"(
finished in )"
              << util::format_duration(duration) << ", " << rps << " req/s, "
              << util::utos_funit(bps) << R"(B/s
requests: )" << stats.req_todo
              << " total, " << stats.req_started << " started, " << stats.req_done
              << " done, " << stats.req_status_success << " succeeded, "
              << stats.req_failed << " failed, " << stats.req_error
              << " errored, " << stats.req_timedout << R"( timeout
status codes: )"
              << stats.status[2] << " 2xx, " << stats.status[3] << " 3xx, "
              << stats.status[4] << " 4xx, " << stats.status[5] << R"( 5xx
traffic: )" << util::utos_funit(stats.bytes_total)
              << "B (" << stats.bytes_total << ") total, "
              << util::utos_funit(stats.bytes_head) << "B (" << stats.bytes_head
              << ") headers (space savings " << header_space_savings * 100
              << "%), " << util::utos_funit(stats.bytes_body) << "B ("
              << stats.bytes_body << R"() data
min         max         mean         sd        +/- sd
time for request: )"
              << std::setw(10) << util::format_duration(ts.request.min) << "  "
              << std::setw(10) << util::format_duration(ts.request.max) << "  "
              << std::setw(10) << util::format_duration(ts.request.mean) << "  "
              << std::setw(10) << util::format_duration(ts.request.sd)
              << std::setw(9) << util::dtos(ts.request.within_sd) << "%"
              << "\ntime for connect: " << std::setw(10)
              << util::format_duration(ts.connect.min) << "  " << std::setw(10)
              << util::format_duration(ts.connect.max) << "  " << std::setw(10)
              << util::format_duration(ts.connect.mean) << "  " << std::setw(10)
              << util::format_duration(ts.connect.sd) << std::setw(9)
              << util::dtos(ts.connect.within_sd) << "%"
              << "\ntime to 1st byte: " << std::setw(10)
              << util::format_duration(ts.ttfb.min) << "  " << std::setw(10)
              << util::format_duration(ts.ttfb.max) << "  " << std::setw(10)
              << util::format_duration(ts.ttfb.mean) << "  " << std::setw(10)
              << util::format_duration(ts.ttfb.sd) << std::setw(9)
              << util::dtos(ts.ttfb.within_sd) << "%"
              << "\nreq/s           : " << std::setw(10) << ts.rps.min << "  "
              << std::setw(10) << ts.rps.max << "  " << std::setw(10)
              << ts.rps.mean << "  " << std::setw(10) << ts.rps.sd << std::setw(9)
              << util::dtos(ts.rps.within_sd) << "%" << std::endl;

    SSL_CTX_free(ssl_ctx);

    if (config.log_fd != -1)
    {
        close(config.log_fd);
    }

    ares_library_cleanup();

    return 0;
}

} // namespace h2load

int main(int argc, char** argv)
{
    return h2load::main(argc, argv);
}
