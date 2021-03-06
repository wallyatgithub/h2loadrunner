#include <openssl/err.h>

#include "tls.h"
#include "http2.h"
#include "util.h"
#include "template.h"
#include "h2load_utils.h"
#include "h2load_Config.h"


namespace h2load
{

bool Config::is_rate_mode() const
{
    return (this->rate != 0);
}
bool Config::is_timing_based_mode() const
{
    return (this->duration > 0);
}
bool Config::has_base_uri() const
{
    return (!this->base_uri.empty());
}
bool Config::rps_enabled() const
{
    return this->rps > 0.0;
}

Config::Config()
    : ciphers(tls::DEFAULT_CIPHER_LIST),
      data_length(0),
      addrs(nullptr),
      nreqs(1),
      nclients(1),
      nthreads(1),
      max_concurrent_streams(1),
      window_bits(30),
      connection_window_bits(30),
      rate(0),
      rate_period(1.0),
      duration(0.0),
      warm_up_time(0.0),
      conn_active_timeout(0.),
      conn_inactivity_timeout(0.),
      no_tls_proto(PROTO_HTTP2),
      header_table_size(4_k),
      encoder_header_table_size(4_k),
      port(0),
      default_port(0),
      connect_to_port(0),
      verbose(false),
      timing_script(false),
      base_uri_unix(false),
#ifndef _WINDOWS
      unix_addr {},
#endif
      rps(0.),
      //      req_variable_start(0),
      //      req_variable_end(0),
      //      req_variable_name(""),
      //      crud_resource_header_name(""),
      //      crud_create_method(""),
      //      crud_update_method(""),
      //      crud_delete_method(""),
      //      crud_create_data_file_name(""),
      //      crud_update_data_file_name(""),
      stream_timeout_in_ms(5000) {}

Config::~Config()
{
    if (addrs)
    {
#ifndef _WINDOWS
        if (base_uri_unix)
        {
            delete addrs;
        }
        else
#endif
        {
            freeaddrinfo(addrs);
        }
    }
}

}
