print("Test starts")


function verify_response(response_header, response_payload)
    if (response_header[":status"] == "404")
    then
        print ("response validation pass")
    else
        print ("response validation failed")
        print ("status code:", headers[":status"])
        print ("body:", body)
    end
end

--[[
This is to set up the number of threads and coroutines
First argument: number of OS threads
Second argument: number of conections to the same host within one thread
Third argument: total number of Lua coroutines

Coroutine helps to increase concurrency
You can send out 2 concurrent requests and await for responses from two different coroutines
Every coroutine executes the same script (this script)

Coroutines are distributed to threads for execution, e.g., 10 threads, 100 coroutines, each thread executes 10 coroutines

This function returns an id of this coroutine
This id, starting from zero, is unique accross all coroutines running this same script 
You can use this id to customize the behavior of each coroutine
For example, you can use this id to decide which user(s) each coroutine should use to construct the request

If this function is not called, thread number is 1, and the same is coroutine number
--]]
local my_id = setup_parallel_test(1, 1, 1)

--[[
Information purpose, not necessary
--]]
print ("my_id:", my_id)

--[[
optional, this is to show how to make a connection beforehand
--]]
local client_id = make_connection("http://192.168.1.107:8081")
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
local request_headers_to_send = {[":scheme"]="http", [":authority"]="192.168.1.107:8081", [":method"]="POST", [":path"]="/nudm-uecm/test"}
local payload = "hello world"

--[[
This is to show how to send a request
This function will return immediately and will not block the coroutine
if no connection exists, a new connection will be made to send this request
--]]
local client_id, stream_id = send_http_request(request_headers_to_send, payload)

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
This is to show how to get the time in milliseconds since epoch
Note: this epoch is not that 1970 one, it is just a certain timestamp that is in the past
This function is not used to get the current date and time, instead, it is used to measure the duration.
Call it before some operation, and call it again after that operation, then the duration is known
--]]
--time_since_epoch()

--[[
This is to show how to retrieve the response of a request sent before
Pass client_id and stream_id to this function to retrieve the response
If the response is not yet available, it will block this coroutine until the response is available.
So obviously, there is no need to call sleep_for_ms before to wait for the response to be available
--]]
local headers, body = await_response(client_id, stream_id)

verify_response(headers, body)
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
    local payload = "hello world again"
    local headers, body = send_http_request_and_await_response(request_headers_to_send, payload)
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
local headers, body = send_http_request_and_await_response(request_headers_to_send, payload)

verify_response(headers, body)
--print ("status code:", headers[":status"])
--print ("body:", body)

--[[
This is to show another way to send a request and wait for the response
This will send out the request, and block this coroutine
One the response is received, this coroutine is resumed and the response is returned
--]]
local request_headers_to_send = {[":scheme"]="http", [":authority"]="192.168.1.107:8081", [":method"]="POST", [":path"]="/nudm-uecm/test"}
local payload = "hello world"
local headers, body = await_response(send_http_request(request_headers_to_send, payload))

verify_response(headers, body)

--print ("status code:", headers[":status"])
--print ("body:", body)

--sleep_for_ms(2000)


function this_is_a_function()
    print ("This is a demo to show sending request inside a function")
    local request_headers_to_send = {[":scheme"]="http", [":authority"]="192.168.1.107:8081", [":method"]="POST", [":path"]="/nudm-uecm/test"}
    local payload = "hello world"
    return send_http_request_and_await_response(request_headers_to_send, payload)
end

local headers, body = this_is_a_function()
verify_response(headers, body)

print ("test await_response with incorrect argument")
local headers, body = await_response(-1, -1)
if (body == "")
then
    print "test pass"
else
    print "test failed"
end

print ("test await_response with non-existed client id")
local headers, body = await_response(99, 1)
if (body == "")
then
    print "test pass"
else
    print "test failed"
end

print ("test await_response with stream id that is already gone")
local headers, body = await_response(0, 1)
if (body == "")
then
    print "test pass"
else
    print "test failed"
end

print "test make connection with incorrect argument"
local client_id = make_connection(1)
if (client_id == -1)
then
    print "test pass"
else
    print "test failed"
end

print "test make connection without schema"
local client_id = make_connection("://192.168.1.107:8081")
if (client_id == -1)
then
    print "test pass"
else
    print "test failed"
end

print "test make_connection to unreachable address"
local client_id = make_connection("http://192.168.1.124:18088")
if (client_id == -1)
then
    print "test pass"
else
    print "test failed"
end

print "test send_http_request with incorrect argument"
local client_id, stream_id = send_http_request(-1, -1)
if (client_id == -1)
then
    print "test pass"
else
    print "test failed"
    print ("client_id", client_id)
end

print "test send_http_request without schema"
local request_headers_to_send = {[":authority"]="192.168.1.107:8081", [":method"]="POST", [":path"]="/nudm-uecm/test"}
local payload = "hello world"
local client_id, stream_id = send_http_request(request_headers_to_send, payload)
if (client_id == -1)
then
    print "test pass"
else
    print "test failed"
    print ("client_id", client_id)
end

print "test send_http_request to unreachable address"
local request_headers_to_send = {[":scheme"]="http", [":authority"]="192.168.1.124:28080", [":method"]="POST", [":path"]="/nudm-uecm/test"}
local payload = "hello world"
local client_id, stream_id = send_http_request(request_headers_to_send, payload)
if (client_id == -1)
then
    print "test pass"
else
    print "test failed"
    print ("client_id", client_id)
end

print "test send_http_request_and_await_response with incorrect argument"
local headers, body = send_http_request_and_await_response(-1, -1)
if (body == "")
then
    print "test pass"
else
    print "test failed"
end

print "test send_http_request_and_await_response without schema"
local request_headers_to_send = {[":authority"]="192.168.1.107:8081", [":method"]="POST", [":path"]="/nudm-uecm/test"}
local payload = "hello world"
local headers, body = send_http_request_and_await_response(request_headers_to_send, payload)
if (body == "")
then
    print "test pass"
else
    print "test failed"
end

print "test send_http_request_and_await_response with unreachable address"
local request_headers_to_send = {[":scheme"]="http", [":authority"]="192.168.1.124:8088", [":method"]="POST", [":path"]="/nudm-uecm/test"}
local payload = "hello world"
local headers, body = send_http_request_and_await_response(request_headers_to_send, payload)
if (body == "")
then
    print "test pass"
else
    print "test failed"
end

print "test sleep_for_ms"
local before = os.time()
sleep_for_ms(1000)
local after = os.time()
if (after - before >= 1)
then
    print "test pass"
    print("before:", before)
    print("after:", after)
else
    print ("test failed", before, after);
end

print "test time_since_epoch"
local time_since_epoch_before = time_since_epoch()
print("now before sleep_for_ms:", os.time())
sleep_for_ms(1100)
print("now after sleep_for_ms:", os.time())
local time_since_epoch_after = time_since_epoch()
if (time_since_epoch_after - time_since_epoch_before >= 1000)
then
    print ("test pass", time_since_epoch_before, time_since_epoch_after)
else
    print ("test failed", time_since_epoch_before, time_since_epoch_after);
end