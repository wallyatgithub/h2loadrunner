-- The following parameters can be changed for different test purpose
local target_tps = 6000
-- TPS each worker thread will take = target_tps/worker_threads
local worker_threads = 2
local duration_to_run_in_seconds = 600
local coroutines = 3000
local connections_per_host_per_thread = 10
local number_of_requests_per_worker_thread_to_trigger_one_stats_print = 1 * target_tps

local my_id = setup_parallel_test(worker_threads, connections_per_host_per_thread, coroutines)

-- optional, better to have
for thread_index = 1,connections_per_host_per_thread do
    client_id = make_connection("http://192.168.1.124:8080")
end

local interval_in_ms_between_requests_for_every_coroutine = ((coroutines / target_tps) * 1000)

local number_of_request_for_each_coroutine = ((duration_to_run_in_seconds * 1000) / interval_in_ms_between_requests_for_every_coroutine)

-- Do not let all coroutines start at the same time, to avoid load fluctuations
sleep_for_ms((my_id / coroutines) * interval_in_ms_between_requests_for_every_coroutine + 100)

-- this is global
max_latency = 0

-- this is global
min_latency = 65536

-- this is global
total_requests_sent = 0

-- this is global, do not modify
total_requests_sent_last_stats_interval = 0

local stats_interval_begin = time_since_epoch()

local function output_stats()
    if ((my_id < worker_threads) and (total_requests_sent - total_requests_sent_last_stats_interval) >= number_of_requests_per_worker_thread_to_trigger_one_stats_print)
    then
        local stats_interval_end = time_since_epoch()
        local stats_duration_in_ms = stats_interval_end - stats_interval_begin
        local delta_requests_sent = total_requests_sent - total_requests_sent_last_stats_interval
        print ("worker: ", my_id, ", total sent: ", total_requests_sent, ", tps: ", (delta_requests_sent*1000)/stats_duration_in_ms, ", max_latency: ", max_latency, ", min_latency: ", min_latency)
        
        max_latency = 0
        min_latency = 65536
        total_requests_sent_last_stats_interval = total_requests_sent
        stats_interval_begin = stats_interval_end
    end
end

local function update_latency(latency)
    if (latency > max_latency)
    then
        max_latency = latency
    end
    
    if (latency < min_latency)
    then
        min_latency = latency
    end
end

local function sleep_between_requests_if_necessary(latency)
    if (latency < interval_in_ms_between_requests_for_every_coroutine)
    then
        sleep_for_ms(interval_in_ms_between_requests_for_every_coroutine - latency)
    end
end

for request_index = 1,number_of_request_for_each_coroutine do
    local x = time_since_epoch()

    local request_headers_to_send = {[":scheme"]="http", [":authority"]="192.168.1.124:8080", [":method"]="PATCH", [":path"]="/nudm-uecm/test"}
    local payload = "hello world again"
    send_http_request_and_await_response(request_headers_to_send, payload)

    total_requests_sent = total_requests_sent + 1

    -- validate the response if you want

    local y = time_since_epoch()
    
    local latency = y - x
    
    update_latency(latency)

    output_stats()
    
    sleep_between_requests_if_necessary(latency)
    
    -- you can send more request here based on your scenario, make sure to update latency and output stats and to sleep to keep the tps
end

