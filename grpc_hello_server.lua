local server_id = start_server("maock_grpc_hello_server.json")

 -- load pb file
pb.loadfile "hello.pb"
local protobuf_start_offset = 6
local response_header = {[":status"]="200", ["content-type"]="application/grpc", ["grpc-accept-encoding"]="identity"}
local trailer = {["grpc-status"]="0"}

print("server id: ", server_id)

register_service_handler(server_id, "helloworld", "handle_grpc_request", 20)

math.randomseed(os.time())

function handle_grpc_request(response_addr, headers, payload)
    --print ("path:", headers[":path"])
    --index = math.random(table.getn(clusters))
    
    local msg = {"Hello, ", "user", "; let me return a random number between 1 to 65535 to you: ", 0}

    local request = pb.decode("helloworld.HelloRequest", string.sub(payload, protobuf_start_offset))
    msg[2] = request.name
    
    msg[4] = math.random(65535)
    
    local resp_msg = {
        message = table.concat(msg)
    }

    local response_payload = pb.encode("helloworld.HelloReply", resp_msg)

    send_response(response_addr, response_header, response_payload, trailer)
end

