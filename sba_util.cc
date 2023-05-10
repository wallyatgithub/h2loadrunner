#include <sba_util.h>
#include <h2load_utils.h>

extern bool debug_mode;
extern thread_local size_t g_current_thread_id;
extern size_t number_of_worker_thread;
extern std::vector<boost::asio::io_service* > g_io_services;
extern std::vector<boost::asio::io_service::strand> g_strands;
extern size_t min_concurrent_clients;

// TODO: pass config schema in
h2load::asio_worker* get_egress_worker()
{
    auto create_worker = []()
    {
        thread_local static h2load::Config conf;
        if (debug_mode)
        {
            conf.verbose = true;
        }
        Request request;
        Scenario scenario;
        scenario.requests.push_back(request);
        conf.json_config_schema.scenarios.push_back(scenario);
        auto worker = std::make_shared<h2load::asio_worker>(0, 0xFFFFFFFF, 1, 0, 1000, &conf, g_io_services[g_current_thread_id]);
        return worker;
    };

    static thread_local auto worker = create_worker();
    return worker.get();
}

void dummy_callback(const std::vector<std::map<std::string, std::string, ci_less>>& resp_headers, const std::string& resp_payload)
{
}

bool send_http2_request(const std::string& method, const std::string& uri,
                        h2load::Stream_Close_CallBack callback,
                        std::map<std::string, std::string, ci_less> headers,
                        std::string message_body)
{
    static std::map<std::string, std::string, ci_less> dummyHeaders;

    http_parser_url u {};
    if (http_parser_parse_url(uri.c_str(), uri.size(), 0, &u) != 0 ||
        !util::has_uri_field(u, UF_SCHEMA) || !util::has_uri_field(u, UF_HOST))
    {
        std::cerr << "invalid uri:" << uri << std::endl;
        return false;
    }

    std::string schema = util::get_uri_field(uri.c_str(), u, UF_SCHEMA).str();
    std::string authority = util::get_uri_field(uri.c_str(), u, UF_HOST).str();
    auto path = get_reqline(uri.c_str(), u);
    uint32_t port;
    if (util::has_uri_field(u, UF_PORT))
    {
        port = u.port;
    }
    else
    {
        port = util::get_default_port(uri.c_str(), u);
    }
    authority.append(":").append(std::to_string(port));
    std::string base_uri = schema;
    base_uri.append("://").append(authority);

    thread_local static auto worker = get_egress_worker();

    auto& clients = worker->get_client_pool();
    PROTO_TYPE proto_type = PROTO_HTTP2;

    h2load::base_client* dest_client = nullptr;

    if (clients[proto_type][base_uri].size() < min_concurrent_clients)
    {
        auto client = worker->create_new_client(0xFFFFFFFF, proto_type, schema, authority);
        worker->check_in_client(client);
        client->set_prefered_authority(authority);
        dest_client = client.get();
    }
    if (!(clients[proto_type][base_uri].size() < min_concurrent_clients))
    {
        for (size_t count = 0; count < clients[proto_type][base_uri].size(); count++)
        {
            thread_local static std::random_device rand_dev;
            thread_local static std::mt19937 generator(rand_dev());
            thread_local static std::uniform_int_distribution<uint64_t>  distr(0, min_concurrent_clients - 1);
            auto client_index = distr(generator);
            auto iter = clients[proto_type][base_uri].begin();
            std::advance(iter, client_index);
            dest_client = *iter;
            if (dest_client->get_total_pending_streams() < dest_client->get_max_concurrent_stream())
            {
                break;
            }
        }
    }

    if (!dest_client)
    {
        auto client = worker->create_new_client(0xFFFFFFFF, proto_type, schema, authority);
        worker->check_in_client(client);
        client->set_prefered_authority(authority);
        dest_client = client.get();
    }

    auto request_to_send = std::make_unique<h2load::Request_Response_Data>(std::vector<uint64_t>(0));
    request_to_send->string_collection.emplace_back(std::move(message_body));
    request_to_send->req_payload = &(request_to_send->string_collection.back());
    request_to_send->string_collection.emplace_back(method);
    request_to_send->method = &(request_to_send->string_collection.back());
    request_to_send->string_collection.emplace_back(std::move(path));
    request_to_send->path = &(request_to_send->string_collection.back());
    request_to_send->string_collection.emplace_back(std::move(authority));
    request_to_send->authority = &(request_to_send->string_collection.back());
    request_to_send->string_collection.emplace_back(std::move(schema));
    request_to_send->schema = &(request_to_send->string_collection.back());
    request_to_send->req_headers_of_individual = std::move(headers);
    request_to_send->req_headers_from_config = &dummyHeaders;
    request_to_send->stream_close_callback = callback;
    dest_client->requests_to_submit.emplace_back(std::move(request_to_send));

    if (h2load::CLIENT_IDLE == dest_client->state)
    {
        dest_client->connect_to_host(*dest_client->requests_to_submit.back()->schema,
                                     *dest_client->requests_to_submit.back()->authority);
    }
    else if (h2load::CLIENT_CONNECTED == dest_client->state)
    {
        dest_client->submit_request();
    }

    return true;
}

std::string get_boundary(const std::string& content_type)
{
    std::string boundary;
    const std::string BOUNDARY = "boundary=";
    auto boundary_start = content_type.find(BOUNDARY);
    if (boundary_start != std::string::npos)
    {
        boundary = content_type.substr(boundary_start + BOUNDARY.size(), std::string::npos);
        boundary = boundary.substr(0, boundary.find(";"));
        std::string tmp;
        tmp.reserve(TWO_LEADING_DASH.size() + boundary.size());
        tmp.append(TWO_LEADING_DASH).append(boundary);
        boundary = std::move(tmp);
    }
    return boundary;
}

