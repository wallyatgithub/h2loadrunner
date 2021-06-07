class H2LoadConfig_Schema: public MsgPacker::CSB_JsonParser<H2LoadConfig_Schema>
{
public:

struct Path {
    std::string source;
    std::string input;
    std::string headerToExtract;
    std::string luaScript;
};

struct scenarioes {
    Path path;
    std::string method;
    std::string payload;
    std::vector<std::string> additonalHeaders;
};

    std::string schema;
    std::string host;
    uint32_t port;
    uint64_t requests;
    uint32_t threads;
    uint32_t clients;
    uint32_t max_concurrent_streams;
    uint64_t window_bits;
    uint64_t connection_window_bits;
    std::string ciphers;
    std::string no_tls_proto;
    uint32_t rate;
    uint32_t rate_period;
    uint64_t duration;
    uint64_t warm_up_time;
    uint64_t connection_active_timeout;
    uint64_t connection_inactivity_timeout;
    std::string npn_list;
    uint64_t header_table_size;
    uint64_t encoder_header_table_size;
    std::string log_file;
    uint64_t request_per_second;
    std::string variable_name_in_path_and_data;
    uint64_t variable_range_start;
    uint64_t variable_range_end;
    std::vector<scenarioes>;



    explicit H2LoadConfig_Schema():
        schema("htt"),
        port(80),
        mCallbackUri(""),
        mContextName(""),
        mContextExpiry(0),
        mHttpHeaders(),
        mApplicationData(""),
        mIsConnFailureHandled(false),
        mPeerIpEndpoint(""),
        mPeerIpEndpointFlags(staticjson::Flags::Optional | staticjson::Flags::IgnoreWrite)
    {  }

    void clear()
    {
        mPeerIndex = 0;
        mStreamId = 0;
        mCallbackUri.clear();
        mPeerIpEndpoint.clear();
        mContextName.clear();
        mContextExpiry = 0L;
        mHttpHeaders.clear();
        mApplicationData.clear();
        mIsConnFailureHandled = false;
        mPeerIpEndpointFlags = staticjson::Flags::Optional | staticjson::Flags::IgnoreWrite;
    }

    void staticjson_init(staticjson::ObjectHandler* h)
    {
        h->add_property("PeerIndex", &this->mPeerIndex, staticjson::Flags::Optional);
        h->add_property("StreamId", &this->mStreamId, staticjson::Flags::Optional);
        h->add_property("CallbackUri", &this->mCallbackUri, staticjson::Flags::Optional);
        h->add_property("PeerIpEndpoint", &this->mPeerIpEndpoint, mPeerIpEndpointFlags);
        h->add_property("ContextName", &this->mContextName, staticjson::Flags::Optional);
        h->add_property("ContextExpiry", &this->mContextExpiry, staticjson::Flags::Optional);
        h->add_property("HttpHeaders", &this->mHttpHeaders, staticjson::Flags::Default);
        h->add_property("ApplicationData", &this->mApplicationData, staticjson::Flags::Default);
        h->add_property("IsConnFailureHandled", &this->mIsConnFailureHandled, staticjson::Flags::Optional);

        h->set_flags(staticjson::Flags::Default | staticjson::Flags::DisallowUnknownKey);
    }


    friend std::ostream& operator<<(std::ostream& o, const CSB_IpcMsgHolder& obj)
    {
        o << "PeerIndex:" << obj.mPeerIndex << std::endl;
        o << "StreamId:" << obj.mStreamId << std::endl;
        o << "CallbackUri:" << obj.mCallbackUri << std::endl;
        o << "PeerIpEndpoint:" << obj.mPeerIpEndpoint << std::endl;
        o << "ContextName:" << obj.mContextName << std::endl;
        o << "ContextExpiry:" << obj.mContextExpiry << std::endl;
        for (auto& id : obj.mHttpHeaders)
        {
            o << "HttpHeaders:" << id.first << " => " << id.second << std::endl;
        }
        o << "ApplicationData:" << obj.mApplicationData << std::endl;
        o << "IsConnFailureHandled:" << obj.mIsConnFailureHandled << std::endl;

        return o;

    }
};