local protobuf_start_offset = 6


pb.loadfile "hello.pb"

local hello = {
   name = "H2loadrunner GRPC Client"
}

local request_payload = pb.encode("helloworld.HelloRequest", hello)

local request_headers_to_send = {[":scheme"]="http", [":authority"]="192.168.1.125:50051", [":path"]="/helloworld.Greeter/SayHello"}

local resp_headers, resp_body, trailers = send_grpc_request_and_await_response(request_headers_to_send, request_payload)

local reply = pb.decode("helloworld.HelloReply", string.sub(resp_body, protobuf_start_offset))

print("server replied msg: ", reply.message)
