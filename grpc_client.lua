
pb.loadfile "hello.pb"

local hello = {
   name = "Hello"
}

local request_payload = pb.encode("helloworld.HelloRequest", hello)

local request_payload_size = string.len(request_payload)

local actual_payload = string.char(0)

actual_payload = actual_payload .. string.char(request_payload_size, 0, 0, 0) .. request_payload

print("encoded grpc body:", actual_payload)

local request_headers_to_send = {[":scheme"]="http", [":authority"]="192.168.1.125:50051", [":path"]="/helloworld.Greeter/SayHello"}

local resp_headers, resp_body, trailers = send_grpc_request_and_await_response(request_headers_to_send, actual_payload)

print("resp body: ", resp_body)
