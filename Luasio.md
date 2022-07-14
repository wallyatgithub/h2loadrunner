
# Luasio: A cross-platform high-performance web platform


## What is Luasio？
**Luasio** is the program mode of h2loadrunner, and it is a web platform built with ASIO; it is written in C++, and is **truly** cross-platform, with best-in-class performance on every popular OS, including Linux, Windows, Mac OS, FreeBSD, etc.

Luasio has a built-in web server engine that supports both http1.1 and http2 with very advanced routing pattern, which can match an incoming http message based on its path, method, authority (host part), any other arbitrary header, and the message body content, as well as their combinations.

Luasio is able to originate outgoing request with different protocols, including http1, http2, grpc, and so on. It is quite simple to extend it with new protocols, like Redis.

To make it simple, Luasio is an equivalent of *OpenResty®*, while going a bit far beyond that. 

For example:

 - *OpenResty®*, for many year, does not support originating outgoing http2 request from application logic; while originating http2 request  is quite natural and very efficient in Luasio.
   
- *OpenResty®* is based on *Nginx*, while *Nginx* on windows uses poll/select instead of IOCP, so "high performance and scalability should not be expected" from *Nginx*/*OpenResty®* on windows; while Luasio is built with ASIO, it uses IOCP on windows, so naturally it  would provide best performance on Windows.
   Luasio natively uses epoll on Linux, kqueue on Mac OS and FreeBSD for best performance.

In short, *OpenResty®* is good, but with limitations; these limitations are becoming more critical in the world where http2 is growing dominating;

*Luasio* is like *OpenResty®*, but never with these limitations.

The programing interface currently supported by Luasio is Lua, this is also like *OpenResty®*, while Luasio does not borrow any code from *OpenResty®*; Luasio is partially inspired by the idea of *OpenResty®*, but not taking any code from the latter.

Luasio uses Lua, because Lua provides a very simple programing interface to manage coroutines with high performance. This is very important to provide simplicity together with high performance.

## How to use Luasio
Write a piece of Lua script, to start a web server listening at some port; meanwhile, provide a configuration file in Json to define the route pattern to match, as well as a function for Luasio to call when a match of the incoming request is found.

Luasio would start a web server at the given port, and inspect every incoming http1 or http2 request from the client, to find a match with the pattern defined; once a match is found, Luasio would call the function above.

The function is able to see every header and the message body of the matched request; it can choose to generate the response and send back the response immediately.

Or alternatively, it can originate a chain of asynchronous outgoing requests,  in either http1, http2, or grpc, or any other protocol that is supported, towards other hosts, a.k.a, the upstream hosts or the so-called back-end servers, to fetch the required information that is needed to compose the response message. 

Once all the required information is available, the response message can be generated and sent back to the client.

Some clarification on the asynchronous outgoing requests: 

User does not need to do anything special to make an outgoing request asynchronous, just program in simple sync way; neither would the user need to save any data (e.g., the incoming request, some transient data received from one upstream host, etc.); Luasio will schedule everything asynchronously and the data would be preserved with the power of Lua coroutine.

An example can be found at 
https://github.com/wallyatgithub/h2loadrunner/blob/main/examples/sdm_get/nudm_sdm.lua

In this example, a server with configuration file named *maock_nudm_sdm_get.json* is started, in which a match rule named *nudm-sdm-get* is defined, which is to match incoming request with method = GET, and path header contains *nudm-sdm/v2*:

	start_server("maock_nudm_sdm_get.json")

Then a handler registration is done to register a function to be called whenever the above matched request is received:

	register_service_handler(server_id, "nudm_sdm_get", "handle_request", 20)

Here the *server_id* is the ID of the server that is started just now, and "nudm_sdm_get" is the matching rule defined in "maock_nudm_sdm_get.json", and "handle_request" is the function to be called when a request is received and matched. 20 is the max number of parallel connections to the same upstream host and this is for the case when the application need to originate outgoing request to upstream hosts. 

With this set up, for every matched incoming request, Luasio would schedule a Lua coroutine and then call the function "handle_request" to process it.

So what can "handle_request" do?

Here in this example, "handle_request" parses the incoming request, and extracts the user identify (supi) as well as other parameters (plmn id, data set name) from the incoming request, and then use those information to compose a new request to the upstream host (listed in nUDRs table) , and then await the response:

	udr_query[3] = supi
    udr_query[5] = serving_plmn_id
    udr_query[7] = data_set
    
    headers[":path"] = table.concat(udr_query)
    index = math.random(table.getn(nUDRs))
    headers[":authority"] = nUDRs[index]
    
    headers[":method"] = "GET"
    
    udr_response_header, response_body = send_http_request_and_await_response(headers, "")

This *send_http_request_and_await_response* seems to be blocking, and indeed it is blocking, but only to this coroutine or we can call it "transaction". 
Internally, Luasio sends the http2 request to the nUDR and await the response asynchronously without blocking anything. 
Once the response from nUDR is received, Luasio will resume the exact coroutine or transaction, and let it proceed with application logic that follows: compose response and send it back:

	multiple_dataset_get_resp_amdata[2] = response_body;
	
    udm_resp_header = {[":status"] = "200", ["x-who-am-I"] = "I am a powerful UDM-SDM instance running Lua script"}
    
    send_response(response_addr, udm_resp_header, table.concat(multiple_dataset_get_resp_amdata))

## APIs offered by **Luasio**

Data types of common variables used next:
request_header/response_header: a map with string as the key, and string as the value
request_payload/response_payload: a string
client_id: integer
stream_id: integer
timeout: an integer in millisecond

**send_http_request_and_await_response**
response_header, response_payload = send_http_request_and_await_response(request_header, request_payload, timeout)

timeout is to specify how long this function should wait for the response before returning

**send_http_request**
client_id, stream_id = send_http_request((request_header, request_payload)

client id and stream id can be pass to **await_response** after a call to **send_http_request**

**await_response**
response_header, response_payload = await_response(client_id, stream_id)

**sleep_for_ms**
sleep_for_ms(timeout)
sleep for millisecond specified by timeout

**time_since_epoch**
time_since_epoch takes no argument, but it returns the milliseconds since the clock's epoch (may NOT necessarily to be 1970)

**start_server**
start_server takes 1 argument which is the config file name (the Json configuration , say maock.json, prepared in this way: https://github.com/wallyatgithub/maock#how-maock-works--usage) for the server, and it returns a string which is the server id.

**register_service_handler**
register_service_handler takes 4 arguments: the server id; the request name specified in the Json configuration above; a function name which is the request processor, that would be called when the request specified right before enters the server; and a last argument which is a interger, to specify the parallel connections to the same upstream host (if you may originate outgoing connection in the request processor).

the request processor takes 3 arguments: response_address which is a table (don't modify this table) and would be used by send_response next, request_header, request_payload

**send_response**
send_response takes 3 arguments, the response_address mentioned above, the response_header, the response_payload


**stop_server**
stop server specified by the server id
stop_server(server_id_which_is_returned_from_start_server)

**resolve_hostname**
It takes 2 arguments, the host name which is a string, and the ttl which is an integer. It returns a table with interger as the key and string as the value, each value is a resolved address. The ttl is in milliseconds, it specifies the interval Luasio would cache the resolution result, i.e., a next call to resolve_hostname within ttl, would return the previous resolution result directly.

## How to run Luasio
As mentioned at the beginning, Luasio is the program mode of h2loadrunner; so once you have prepared your script, just run the script with h2loadrunner:
./h2loadrunner --script=your_script.lua