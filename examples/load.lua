-- The following parameters can be changed for different test purpose

local total_target_tps = 12000

-- TPS each worker thread will take = (total_target_tps / number_of_worker_threads)
local number_of_worker_threads = 8

local duration_to_run_in_seconds = 60

local connections_per_host_per_thread = 20

-- each worker_thread will take (number_of_virtual_users / number_of_worker_threads) of virtual user
local number_of_virtual_users = 24000

local stats_output_interval_in_ms = 1000

local my_id = setup_parallel_test(number_of_worker_threads, connections_per_host_per_thread, (number_of_virtual_users + number_of_worker_threads))

local interval_in_ms_between_requests_for_every_virtual_user = ((number_of_virtual_users / total_target_tps) * 1000)

local number_of_request_to_send_in_one_loop_of_virtual_user = 1

local number_of_loops_for_each_virtual_user = ((duration_to_run_in_seconds * 1000) / interval_in_ms_between_requests_for_every_virtual_user)

-- the following global variables are shared among all number_of_virtual_users within one worker_thread
--
max_latency = 0

min_latency = 65536

total_requests_sent = 0

total_requests_sent_last_stats_interval = 0

total_response_time_is_ms_within_one_stats_interval = 0

total_number_of_connection_failures = 0

-- Do not let all virtual user start at the same time, to avoid load fluctuations
sleep_for_ms((my_id / number_of_virtual_users) * interval_in_ms_between_requests_for_every_virtual_user + 100)

local stats_interval_begin = time_since_epoch()

local function output_stats()
    for request_index = 1, (duration_to_run_in_seconds * 1000)/stats_output_interval_in_ms do
        sleep_for_ms(stats_output_interval_in_ms)
        local stats_interval_end = time_since_epoch()
        local stats_duration_in_ms = stats_interval_end - stats_interval_begin
        local delta_requests_sent = total_requests_sent - total_requests_sent_last_stats_interval
        local avg_latency = total_response_time_is_ms_within_one_stats_interval / delta_requests_sent
        output = string.format("worker: %d, total sent: %d, tps: %f, max_latency: %d, min_latency: %d, average_latency: %d", my_id, total_requests_sent, (delta_requests_sent*1000)/stats_duration_in_ms, max_latency, min_latency, avg_latency)
        print (output)

        max_latency = 0
        min_latency = 65536
        total_response_time_is_ms_within_one_stats_interval = 0
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

    total_response_time_is_ms_within_one_stats_interval = total_response_time_is_ms_within_one_stats_interval + latency
end

local function sleep_between_requests_if_necessary(latency)
    local time_to_sleep = interval_in_ms_between_requests_for_every_virtual_user - latency
    if (time_to_sleep > 1)
    then
        sleep_for_ms(time_to_sleep)
    end
end

function generate_load()
    local loop_count = 0;
    for request_index = 1,number_of_loops_for_each_virtual_user do

        if ( loop_count % number_of_request_to_send_in_one_loop_of_virtual_user == 0)
        then

            local x = time_since_epoch()

            local request_headers_to_send = {[":scheme"]="http", [":authority"]="192.168.1.107:8082", [":method"]="PATCH", [":path"]="/nudm-uecm/test"}
            local payload = "hello world again"
            local response_header, response_payload = send_http_request_and_await_response(request_headers_to_send, payload)
            if (next(response_header) ~= nil)
            -- validate the response further if you want
            then
                total_requests_sent = total_requests_sent + 1
            end
            local y = time_since_epoch()
            local latency = y - x
            update_latency(latency)
            sleep_between_requests_if_necessary(latency)

        --[[
         if ( loop_count % number_of_request_to_send_in_one_loop_of_virtual_user == 1)
         then
         bla bla bla
        --]]
        end

        loop_count = loop_count + 1;

    end
end

if (my_id < number_of_worker_threads)
then
    output_stats()
else
    generate_load()
end
