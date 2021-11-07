[![license](https://img.shields.io/github/license/wallyatgithub/h2loadrunner.svg?style=flat-square)](https://github.com/wallyatgithub/h2loadrunner)
![build status](https://github.com/wallyatgithub/h2loadrunner/actions/workflows/cmake.yml/badge.svg)

*Read this in other languages: [简体中文](README.zh-cn.md).*

# h2loadrunner is an HTTP and HTTP2 benchmarking / load testing / performance testing tool
  h2loadrunner is a benchmarking tool supporting both HTTP 1.x and HTTP2.
  
  It was forked from the h2load utility of nghttp2,  yet with a number of powerful features added.
  
  Thanks to libEv (w/ epoll/poll/kqueue), like h2load, h2loadrunner can generate a very large amount of load with multi-core.
  
  Besides, h2loadrunner supports powerful features that are not present in h2load:
  
  1. Variable support in URI and message body.
  
  2. Stream timeout handling.
  
  3. Transaction support with specific resource header tracking(e.g. location header in 5G SBA).
  
     Other types of resource tracking are to be added in the future, like, XPath for XML message body, Json pointer for Json message body, etc.

  4. Natively support Set-Cookie and Cookie headers, yet provides the flexibility of clearing Cookie before the execution of one request.
  
     Cookies handling complies to https://tools.ietf.org/html/rfc6265
  
  5. Lua script support.
     With lua script, user can customize every header and the payload of the request to be sent.
  
  6. Both command line interface and JSON based configuration.
     With JSON configuration, user can build the test scenario with a GUI editor.
     
  7. Dynamic report of the test, dynamic change of the QPS/RPS.
     h2loadrunner prints the test statistics every second; it also supports dynamic change of QPS/RPS.
     
  8. Support connections to multiple hosts

     H2loadrunner supports different hosts for different requests; when a new host is identified, h2loadrunner will initiate async DNS resolution and connect to the host dynamically

     Dynamic connections are re-used among different requests with same authority (host) header.

  9. Support delay between requests of the same scenario, with the delay interval configurable.

  10. Support configurable status code to determine if a response is successful in statistics report
  
  11. mTLS support

# How performant is h2loadrunner?
  For this test scenario, h2loadrunner will only need less than one core to reach 60K QPS/s:
  
    POST with dynamic path generation and dynamic message body of 300 bytes 
    Upon POST response, extract the resource created by POST from response header, and send PATCH with dynamic message body of 300 bytes for resource update
    Upon PATCH response, send DELETE to delete resource.

  CPU usage @ 60K QPS/s:
  
    2756 root      20   0  366240  39668   7340 S  87.5   0.5   0:02.56 h2loadrunner
    2756 root      20   0  366240  40988   7340 S  96.0   0.5   0:03.04 h2loadrunner
    2756 root      20   0  366240  42044   7340 S  96.1   0.5   0:03.53 h2loadrunner
    2756 root      20   0  366240  43364   7340 S  96.0   0.5   0:04.01 h2loadrunner
    2756 root      20   0  366240  44684   7340 S  96.1   0.5   0:04.50 h2loadrunner
    2756 root      20   0  366240  45740   7340 S  94.0   0.6   0:04.97 h2loadrunner

  CPU usage @ 120K QPS/s, basically is double that of 60K QPS/s, without noticable overhead or bottleneck：
  
    2749 root      20   0  404640 102876   7460 S 198.0   1.3   0:23.21 h2loadrunner
    2749 root      20   0  404640 102876   7460 S 190.2   1.3   0:24.18 h2loadrunner
    2749 root      20   0  404640 102876   7460 S 198.0   1.3   0:25.17 h2loadrunner
    2749 root      20   0  404640 102876   7460 S 190.2   1.3   0:26.14 h2loadrunner
    2749 root      20   0  404640 102876   7460 S 198.0   1.3   0:27.13 h2loadrunner
    2749 root      20   0  404640 102876   7460 S 190.2   1.3   0:28.10 h2loadrunner
    2749 root      20   0  404640 102876   7460 S 196.0   1.3   0:29.08 h2loadrunner
 
  So, the output of h2loadrunner can grow linearly on multi-core machines.
  
  Need to mention that, to stress h2loadrunner, the mock server intentionally maks some requests fail, by not sending back the response (3%), or sending back failure response.
  
  Meaning, in this test, h2loadrunner needs to take care of a small portion (3%) of stream timeout case during the load test runnning.
  
  Result shows, h2loadrunner handles this situation without any problem:
  
    Sat Jun 19 12:44:11 2021, send: 121083, successful: 109202, 3xx: 0, 4xx: 8427, 5xx: 0, max resp time (us): 2031516, min resp time (us): 1204, successful/send: 90.1877%
    Sat Jun 19 12:44:12 2021, send: 120320, successful: 108389, 3xx: 0, 4xx: 8489, 5xx: 0, max resp time (us): 2035583, min resp time (us): 1104, successful/send: 90.0839%
    Sat Jun 19 12:44:13 2021, send: 120016, successful: 108134, 3xx: 0, 4xx: 8374, 5xx: 0, max resp time (us): 2022545, min resp time (us): 908, successful/send: 90.0997%
    Sat Jun 19 12:44:14 2021, send: 120069, successful: 108211, 3xx: 0, 4xx: 8347, 5xx: 0, max resp time (us): 2018241, min resp time (us): 1080, successful/send: 90.124%
    Sat Jun 19 12:44:15 2021, send: 120119, successful: 108290, 3xx: 0, 4xx: 8325, 5xx: 0, max resp time (us): 2017653, min resp time (us): 1012, successful/send: 90.1523%
    Sat Jun 19 12:44:16 2021, send: 119963, successful: 108078, 3xx: 0, 4xx: 8455, 5xx: 0, max resp time (us): 2023169, min resp time (us): 1097, successful/send: 90.0928%
    Sat Jun 19 12:44:17 2021, send: 119884, successful: 107971, 3xx: 0, 4xx: 8450, 5xx: 0, max resp time (us): 2022223, min resp time (us): 733, successful/send: 90.0629%
    Sat Jun 19 12:44:18 2021, send: 120210, successful: 108380, 3xx: 0, 4xx: 8349, 5xx: 0, max resp time (us): 2021652, min resp time (us): 870, successful/send: 90.1589%


# How to build

  These packages are required to build h2loadrunner (take Ubuntu for example):
  
    libnghttp2-dev
    openssl
    libssl-dev
    libev-dev
    libluajit-5.1-dev
    rapidjson-dev
    c-ares (included in third-party/c-ares, as the c-ares devel package of linux distro is too old)

  Use cmake to build

    $git clone https://github.com/wallyatgithub/h2loadrunner.git
    
    $cd h2loadrunner
    
    $cd third-party/c-ares
    
    $cmake ./
    
    $cmake --build ./
    
    $cd ../../
    
    $mkdir build
    
    $cd build
    
    $cmake ..
    
    $cmake --build ./
    
    h2loadrunner would be generated
    
# How to build h2loadrunner docker image

  https://raw.githubusercontent.com/wallyatgithub/h2loadrunner/main/Dockerfile_CentOS7 is the Dockerfile to build a CentOS7 based image with h2loadrunner installed in /usr/bin

  https://raw.githubusercontent.com/wallyatgithub/h2loadrunner/main/Dockerfile_Ubuntu is the Dockerfile to build a latest Ubuntu based image with h2loadrunner installed in /usr/bin

  For example, to build latest Ubuntu based image with h2loadrunner:
  
    # mkdir h2loadrunner
    
    # cd h2loadrunner
    
    # wget https://raw.githubusercontent.com/wallyatgithub/h2loadrunner/main/Dockerfile_Ubuntu
    
    # docker build ./ -f Dockerfile_Ubuntu -t h2loadrunner:ubuntu
    
    Then use 'docker run -it h2loadrunner:ubuntu bash' to enter the container, h2loadrunner is located in /usr/bin

    
    

# Basic Usage

	h2loadrunner http://192.168.1.125:8080/nudm-ee/v2/imsi-2621012-USER_ID/sdm-subscriptions/  -t 5 -c 100 -D 10 -m 512 --rps=100 --crud-update-method=PATCH --crud-delete-method=DELETE --crud-create-method=POST --crud-request-variable-name="-USER_ID" --crud-request-variable-value-start=1 --crud-request-variable-value-end=1000000000 --crud-resource-header="location" --stream-timeout-interval-ms=2000 -m 512 --crud-create-data-file=datafile.json --crud-update-data-file=updatedatafile.json
  
  This runs a benchmark test for 10 seconds, using 5 threads, and keeping 100 HTTP2 connections open, with each connection @ 100 RPS/QPS, so total QPS/RPS = 100 * 100 = 10K RPS/s in this test.
  
  This test is done on a range of users, with user ID dynamically replaced and range specified in command line.
  
  This test also automatically tracks the response message for a specific header, and the subsequent request is built with the returned URI in this specific header.
  
  Output:
  
    finished in 11.03s, 9711.00 req/s, 772.10KB/s
    requests: 99420 total, 99999 started, 99420 done, 90115 succeeded, 9305 failed, 2310 errored, 2312 timeout
    status codes: 90115 2xx, 0 3xx, 6995 4xx, 0 5xx
    traffic: 7.54MB (7906295) total, 4.32MB (4529697) headers (space savings 57.56%), 1.54MB (1619718) data
                         min         max         mean         sd        +/- sd
    time for request:      158us     17.80ms       946us       481us    81.73%
    time for connect:      160us     15.78ms      3.94ms      2.56ms    70.00%
    time to 1st byte:    15.39ms     28.06ms     20.01ms      3.30ms    68.00%
    req/s           :      96.04       98.38       97.08        0.50    67.00%

  Here is what is going on with the above command:

  First, "POST" (--crud-create-method) request is sent to the URI (http://192.168.1.125:8080/nudm-ee/v2/imsi-2621012-USER_ID/sdm-subscriptions/) with "-USER_ID" replaced by an actual user ID whose range starts from 1 (--crud-request-variable-value-start) to 1000000000 (--crud-request-variable-value-end), with payload content specified in file datafile.json (--crud-create-data-file)

  Example content of datafile.json:
      
    {"callbackReference":"http://10.10.177.251:32050/nhss-ee/v1/msisdn-491971103488-USER_ID/ee-subscriptions","monitoringConfiguration":{"120984":{"eventType":"UE_REACHABILITY_FOR_SMS","immediateFlag":false,"referenceId":120984}},"reportingOptions":{"maxNumOfReports":0}}

  The "POST" response is monitored for the header named "location" (--crud-resource-header), whose value is a URI, which is the resource (5G EE-subscription) created by "POST".

  Next, "PATCH" (--crud-update-method) is sent to the URI above to update the resource created, with payload specified in updatedatafile.json (--crud-update-data-file), to modify the resource created.

  At last, "DELETE" (--crud-delete-method) is sent to delete the created resource, which is actually an unsubscription here in this 5G EE-subscription case.

  other parameters:

    --stream-timeout-interval-ms:
    
    how long would h2loadrunner wait for a response to come; when this is exceeded, RST_STREAM is sent to release the resource
    
    --rps: desired request per second per connection
    
    -t: number of thread
    
    -c: number of client, which is typically the number of connections
    
    -D: how long the test should run
    
    -m: max concurrent streams per connection, this is a key feature of HTTP2.
    
    For other possible parameters (derived from h2load), type h2loadrunner --help


# JSON configuration support and GUI interface for configuration

  h2loadrunner supports JSON based configuration.
  
  With this feature, h2loadrunner can support flexible scenario combinations, not limiting to typical CRUD (Create-Read-Update-Delete) scenario above.
  
  Json schema: https://github.com/wallyatgithub/h2loadrunner/blob/main/config_schema.json
   
  It is recommended to use a GUI based Json editor to load the Json schema above to edit and then export the Json configuration data.

  There are a couple of online Json editors available, for example: 

    https://json-editor.github.io/json-editor/
	
    https://pmk65.github.io/jedemov2/dist/demo.html

  Take https://json-editor.github.io/json-editor/ for example:
  
  Paste content of https://raw.githubusercontent.com/wallyatgithub/h2loadrunner/main/config_schema.json to "Schema" field of the above link
  
  Then click "Update Schema", a form named h2loadrunner_configuration is available on top for Edit
  
  After finishing editing, click "Update Form" to get the JSON data at the right side.
  
  Save it to a file <JSON FILE>, then use h2loadrunner --config-file=<JSON FILE> to start the load run
  
  You can also paste stored JSON data back to the right side, and click "Update Form", to sychronize that in the left side form for further edit in GUI.  

  Example screenshot of another Json editor at https://pmk65.github.io/jedemov2/dist/demo.html:
  
  ![Example of GUI configuration](https://raw.githubusercontent.com/wallyatgithub/h2loadrunner/main/Json_editor.png)
  ![Example of GUI configuration of scenario](https://raw.githubusercontent.com/wallyatgithub/h2loadrunner/main/Json_editor-scenario.png)
  
  
  When using Json configuration, if wanted, it is still possible to override parameters with command line interface.

  For example, with this command line:

    h2loadrunner --config-file=config.json -t 1 -c 3 --rps=1 -D 100  
  
  Command line input (1 thread, 3 connections, rps 100, duration 100) coming after --config-file will override those respective fields in config.json.

 
# Lua script support

  Like wrk/wrk2, h2loadrunner supports Lua script, capable of customizing every header and payload of the request to be sent.

  To explain how it works, let's first introduce the schema defining how h2loadrunner will run the test.
 
  h2loadrunner Json schema has a section called "scenario", and "scenario" is a list of requests h2loadrunner will execute sequentially.
  
  Note: Sequential execution is for requests within one "scenario"; h2loadrunner will keep track of the request and response of each request, and the next request can be started only if the response of the prior request is received. 
  
  Each "scenario" is executed sequentially, while h2loadrunner can run many "scenario" in parallel.

  For example, h2loadrunner can start 1000 "scenario" on 100 connections (concurrent streams, -m option) in parallel, each "scenario" represents a user's activities in sequence.
  
  The 1000 "scenario" are executed in parallel, while within each "scenario", the user activity is executed sequentially. 
  
  As said before, "scenario" is a list of requests, while each request has several basic fields, like path, method, payload, and additonalHeaders, and also a field called "luaScript".
  
  path, method, payload, and additonalHeaders, as the names suggest, are the path header, method header, message body, and other additional headers (such as user-agent) to build the request.
  
  In which the path field is a compound structure, which aims to provide some quick and handy options for quick definition of some typical test scenario. 
  
  For example, the user can specify in the path field to copy the path from the request prior to this one (sameWithLastOne), or to extract the path value from some specific header of the response responding to the request prior to this one (extractFromLastResponseHeader). Of course, direct input of the path is also supported.
  
  Now comes the "luaScript" field:
  
  The "luaScript" field is associated with each request within the "scenario" section. The Lua script will be executed by h2loadrunner for the request, as long as the request has a valid snippet of script (see next for format and naming convention of the snippet of script).

  "luaScript" field can be filled with a snippet of needed lua script directly, or with the path/name of a file, which has the script.

  Each request can have a different snippet of lua script, and lua scripts of different requests are executed indepdendently. 
  
  h2loadrunner requires the lua script of any request in this format and naming convention:
  
  It must be named "make_request", and it takes 4 input arguments, and it can return up to 2 output arguments: one table and one string. 
  
  Example:
  
    function make_request(response_header, response_payload, request_headers_to_send, request_payload_to_send)
        --[[
        -- do something, typically, modify request_headers_to_send and request_payload_to_send, for example:
        
           request_headers_to_send["user-agent"] = "h2loadrunner with lua"
           request_headers_to_send["authorization"] = response_header["authorization"]
        
        -- then, this function needs to return what is/are modified, a table, and a string, at most, are expected 
        -- the table is the full list of the header, while the string is the full content of the payload, from which, the request is going to be built and sent out
        -- h2loadrunner will take care of the content-length header, i.e., add/update the content-length header according to updated payload returned
        -- the header naming convention need to follow http2 naming convention, i.e., :path, :method, etc, 
        -- h2loadrunner will take care of the header name transformation needed for http 1.x
        --]]
        return request_headers_to_send, request_payload_to_send
    end

  Among the 4 input arguments, response_header, and response_payload, are the headers and body of the response message; the response message is to the last request; the last request is the request prior to the current request (being worked on) within the "scenario" section;
  
  request_headers_to_send, and request_payload_to_send, are the headers and message body of the current request; they are generated from path, method, payload, and additonalHeaders fields.
  
  The lua function make_request can do whatever it wants, with the available information (all content of last response, all content of current request so far), and make necessary update to the current request headers and payload, and return the modified.
  
  For example, the "scenario" section has 5 requests defined, and the 3rd request has a valid Lua function make_request, then when this make_request function is executed, it will have acess to not only the template of the 3rd request (built with the input of each Json fields of the request), but also the full content of the 2nd response. 

  To summarize: with Lua script and the information made available to the Lua script, theoretically, h2loadrunner can generate whatever request needed.
  
  Well, of course, to reach that, various Lua scripts are needed for various test needs. :)
  
    
# HTTP 1.x support
  
  Although named as 'h2'loadrunner (which is derived from h2load obviously), h2loadrunner can also support http 1.1 test without any known problem so far.
  
  h2loadrunner might not behave perfectly when dealing with http 1.0 servers, who will tear down the connection right after the response is sent.
  
  So in case of an old http 1.0 server, h2loadrunner may not be able to reach the QPS/RPS at the exact number specified by --rps (or "request-per-second" field in Json).
  
  


