#include <udsf_util.h>
#include <h2load_utils.h>


namespace udsf
{

h2load::asio_worker* get_worker()
{
    thread_local static h2load::Config conf;
    auto init_config = []()
    {
        Request request;
        Scenario scenario;
        scenario.requests.push_back(request);
        conf.json_config_schema.scenarios.push_back(scenario);
        return true;
    };
    thread_local static auto init_config_ret_code = init_config();

    thread_local static std::shared_ptr<h2load::asio_worker> worker = std::make_shared<h2load::asio_worker>(0, 0xFFFFFFFF,
                                                                                                            1, 0, 1000, &conf);

    thread_local static auto work = boost::asio::io_service::work(worker->get_io_context());

    auto start_worker = []()
    {
        auto thread_func = []()
        {
            worker->run_event_loop();
        };

        std::thread worker_thread(thread_func);
        worker_thread.detach();
        return true;
    };
    thread_local static auto dummy = start_worker();
    return worker.get();
}

bool send_http2_request(const std::string& method, const std::string& uri,
                        const std::map<std::string, std::string, ci_less>& headers,
                        const std::string& message_body)
{
    static std::map<std::string, std::string, ci_less> dummyHeaders;

    http_parser_url u {};
    if (http_parser_parse_url(uri.c_str(), uri.size(), 0, &u) != 0 ||
        !util::has_uri_field(u, UF_SCHEMA) || !util::has_uri_field(u, UF_HOST))
    {
        std::cerr << "invalid uri:" << uri << std::endl;
        return false;
    }

    auto worker = get_worker();

    auto run_inside_worker = [method, uri, headers, message_body, worker]()
    {
        http_parser_url u {};
        if (http_parser_parse_url(uri.c_str(), uri.size(), 0, &u) != 0 ||
            !util::has_uri_field(u, UF_SCHEMA) || !util::has_uri_field(u, UF_HOST))
        {
            std::cerr << "invalid uri:" << uri << std::endl;
            return;
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
        auto& clients = worker->get_client_pool();
        PROTO_TYPE proto_type = PROTO_HTTP2;

        h2load::base_client* dest_client = nullptr;

        for (auto& client : clients[proto_type][base_uri])
        {
            if (client->get_total_pending_streams() < client->get_max_concurrent_stream())
            {
                dest_client = client;
                break;
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
        request_to_send->string_collection.emplace_back(message_body);
        request_to_send->req_payload = &(request_to_send->string_collection.back());
        request_to_send->string_collection.emplace_back(std::move(method));
        request_to_send->method = &(request_to_send->string_collection.back());
        request_to_send->string_collection.emplace_back(std::move(path));
        request_to_send->path = &(request_to_send->string_collection.back());
        request_to_send->string_collection.emplace_back(std::move(authority));
        request_to_send->authority = &(request_to_send->string_collection.back());
        request_to_send->string_collection.emplace_back(std::move(schema));
        request_to_send->schema = &(request_to_send->string_collection.back());
        request_to_send->req_headers_of_individual = headers;
        request_to_send->req_headers_from_config = &dummyHeaders;
        dest_client->requests_to_submit.emplace_back(std::move(request_to_send));

        if (h2load::CLIENT_IDLE == dest_client->state)
        {
            dest_client->connect_to_host(schema, authority);
        }
        else if (h2load::CLIENT_CONNECTED == dest_client->state)
        {
            dest_client->submit_request();
        }
    };
    worker->get_io_context().post(run_inside_worker);

    return true;
}

}
