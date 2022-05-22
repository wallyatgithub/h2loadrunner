
pb.loadfile "hello.pb"

local hello = {
   name = "Hello"
}

local request_payload = pb.encode("helloworld.HelloRequest", hello)

print("encoded grpc body:", actual_payload)

local request_headers_to_send = {[":scheme"]="http", [":authority"]="192.168.1.125:50051", [":path"]="/helloworld.Greeter/SayHello"}

local resp_headers, resp_body, trailers = send_grpc_request_and_await_response(request_headers_to_send, request_payload)

print("resp body: ", resp_body)
