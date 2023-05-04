#include <openssl/err.h>

#include "tls.h"
#include "http2.h"
#include "util.h"
#include "template.h"
#include "h2load_utils.h"
#include "h2load_Config.h"
#ifdef ENABLE_HTTP3
#include <nghttp3/nghttp3.h>
#endif

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
      tls13_ciphers("TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:TLS_"
                    "CHACHA20_POLY1305_SHA256:TLS_AES_128_CCM_SHA256"),
      groups("X25519:P-256:P-384:P-521"),
      data_length(0),
      addrs(nullptr),
      nreqs(1),
      nclients(1),
      nthreads(1),
      max_concurrent_streams(1),
      max_frame_size(16_k),
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
      stream_timeout_in_ms(5000),
      no_udp_gso(false),
      max_udp_payload_size(0),
      ktls(false)
      {}

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
bool Config::is_quic() const
{
#ifdef ENABLE_HTTP3
    return !npn_list.empty() &&
           (npn_list[0] == NGHTTP3_ALPN_H3 || npn_list[0] == "\x5h3-29");
#else  // !ENABLE_HTTP3
    return false;
#endif // !ENABLE_HTTP3

}

}
