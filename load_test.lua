my_id = setup_parallel_test(2, 10, 5000)

-- optional, better to have
for i=1,10 do
    client_id = make_connection("http://192.168.1.124:8080")
end

-- Do not let all coroutines start at the same time, to avoid load fluctuations
sleep_for_ms((my_id%10)*100+100)

for i=1,1000 do
    local x = time_since_epoch()
    request_headers_to_send = {[":scheme"]="http", [":authority"]="192.168.1.124:8080", [":method"]="PATCH", [":path"]="/nudm-uecm/test"}
    payload = "hello world again"
    send_http_request_and_await_response(request_headers_to_send, payload)
    -- validate the response if you want
    local y = time_since_epoch()
    if (y - x < 1000)
    then
        sleep_for_ms(1000 - (y - x))
    end
end