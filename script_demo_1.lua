print("Test starts")

--[[
This is to set up the number of threads and coroutines
First argument: number of OS threads
Second argument: total number of Lua coroutines

Within one thread, to the same host, only one connection exists
Obviously, there are different connections to different hosts even within each thread

Coroutine helps to increase concurrency
You can send out 2 concurrent requests and await for responses from two different coroutines
Every coroutine executes the same script (this script)

Coroutines are distributed to threads for execution, e.g., 10 threads, 100 coroutines, each thread executes 10 coroutines

This function returns an id of this coroutine, and this id is unique accross all coroutines running this same script 
You can use this id to customize the behavior of each coroutine
For example, you can use this id to decide which user(s) each coroutine should use to construct the request

If this function is not called, thread number is 1, and the same is coroutine number
--]]
my_id = setup_parallel_test(10, 1000)

--[[
Information purpose, not necessary
--]]
print ("my_id:", my_id)

--[[
optional, this is to show how to make a connection beforehand
--]]
client_id = make_connection("http://192.168.1.107:8081")
--[[
Information purpose, not necessary
--]]
print ("client_id:", client_id)

--[[
-- A connection established before will be reused and returned
client_id = make_connection("http://192.168.1.107:8081")
print ("client_id:", client_id)
--]]

--[[
-- for debug, to show a failure connection, failure connection will be retried until test finish
client_id = make_connection("http://192.168.1.124:8088")
print ("client_id:", client_id)

-- for debug, to show the re-use of a failure connection
client_id = make_connection("http://192.168.1.124:8088")
print ("client_id:", client_id)
--]]

--[[
this is to show how to prepare a request message, a table of headers and a payload
--]]
request_headers_to_send = {[":scheme"]="http", [":authority"]="192.168.1.107:8081", [":method"]="POST", [":path"]="/nudm-uecm/test"}
payload = "hello world"

--[[
This is to show how to send a request
This function will return immediately and will not block the coroutine
if no connection exists, a new connection will be made to send this request
--]]
client_id, stream_id = send_http_request(request_headers_to_send, payload)

--[[
Information purpose, not necessary
--]]
--print ("client_id:", client_id)
--print ("stream_id:", stream_id)

--[[
This is to show how to sleep for some milli seconds, this will only block this Lua coroutine, not the OS thread
--]]
--sleep_for_ms(1000)

--[[
This is to show how to retrieve the response of a request sent before
Pass client_id and stream_id to this function to retrieve the response
If the response is not yet available, it will block this coroutine until the response is available.
So obviously, there is no need to call sleep_for_ms before to wait for the response to be available
--]]
headers, body = await_response(client_id, stream_id)
--[[
Information purpose, not necessary
--]]
--print ("status code:", headers[":status"])
--print ("body:", body)

--sleep_for_ms(1000)

--[[
This is to show how to send requests repeatedly while with a pause in between
--]]
for i=1,100 do
    request_headers_to_send = {[":scheme"]="http", [":authority"]="192.168.1.107:8081", [":method"]="PATCH", [":path"]="/nudm-uecm/test"}
    payload = "hello world again"
    send_http_request_and_await_response(request_headers_to_send, payload)
    --sleep_for_ms(100)
    --print ("client_id:", client_id)
    --print ("stream_id:", stream_id)
end
--sleep_for_ms(1000)

--[[
This is to show how send a request and wait for the response with one function
This function will send out the request, and block this coroutine
One the response is received, this coroutine is resumed and the response is returned
--]]
headers, body = send_http_request_and_await_response(request_headers_to_send, payload)

--print ("status code:", headers[":status"])
--print ("body:", body)

--[[
This is to show another way to send a request and wait for the response
This will send out the request, and block this coroutine
One the response is received, this coroutine is resumed and the response is returned
--]]
request_headers_to_send = {[":scheme"]="http", [":authority"]="192.168.1.107:8081", [":method"]="POST", [":path"]="/nudm-uecm/test"}
payload = "hello world"
headers, body = await_response(send_http_request(request_headers_to_send, payload))
--print ("status code:", headers[":status"])
--print ("body:", body)

--sleep_for_ms(2000)


function this_is_a_function()
    print ("This is a demo to show sending request inside a function")
    local request_headers_to_send = {[":scheme"]="http", [":authority"]="192.168.1.107:8081", [":method"]="POST", [":path"]="/nudm-uecm/test"}
    local payload = "hello world"
    return send_http_request_and_await_response(request_headers_to_send, payload)
end

headers, body = this_is_a_function()
print ("status code:", headers[":status"])
print ("body:", body)