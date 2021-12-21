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
  
  3. Transaction support with specific resource header tracking.
  
     Other types of resource tracking are to be added in the future, like, XPath for XML message body, Json pointer for Json message body, etc.

  4. Natively support Set-Cookie and Cookie headers, yet provides the flexibility of clearing Cookie before the execution of one request.
  
     Cookies handling complies to https://tools.ietf.org/html/rfc6265
  
  5. Lua script support.
     With lua script, user can customize every header and the payload of the request to be sent.
  
  6. Both command line interface and JSON based configuration.
     With JSON configuration, user can build the test scenario with a GUI editor.
     
  7. Dynamic report of the test, dynamic change of the RPS.
     h2loadrunner prints the test statistics every second; it also supports dynamic change of RPS.
     
  8. Async dynamic connection establishment.

  9. Support delay between requests of the same scenario, with the delay interval configurable.

  10. Support configurable status code to determine if a response is successful in statistics report
  
  11. mTLS support
  
  13. Parallel connections to multiple hosts in a load share pool, with connection failover and failback.

# How to build

  These packages are required to build h2loadrunner (take Ubuntu for example):
  
    libnghttp2-dev
    openssl
    libssl-dev
    libev-dev
    libluajit-5.1-dev
    rapidjson-dev
    libboost-all-dev
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


# Usage

  h2loadrunner uses JSON based configuration.
  
  With this feature, h2loadrunner can support flexible scenario combinations.
  
  h2loadrunner Json schema: https://github.com/wallyatgithub/h2loadrunner/blob/main/config_schema.json
   
  It has a section called "scenarios", which is an arrary of "scenario".

  Each scenario is associated with a name, a weight, and a list of requests.
  
  The name of the scenario is used in statistics output;
  
  The weight determines the ratio of the traffic from this scenario to all traffic. For example:
    
    If there are 2 scenarios, the first scenario has weight = 400, and 4 requests; the other scenario has weight = 100, and 8 requests.
    
    Then h2loadrunner will schedule the 2 scenario to make sure:
    
    80% of the requests (400/(400+100)) in the traffic mix, are the 4 requests of the first scenario;
    
    20% of the requests (100/(400+100)) in the traffic mix, are the 8 requests of the second scenario.
  
  Different scenarios will run in parallel.
  
  Same scenario of different users (see user-id-variable-in-path-and-data field, each instance of the varible in range represents a user), will also run in parallel.
  
  Each scenario has a list of requests; requests of the same scenario for the same user will be executed sequentially.

  For example, h2loadrunner can start 1000 parallel "scenario" on 100 connections (with concurrent streams), each "scenario" has a list of requests representing a user's activities in sequence.
  
  The 1000 "scenario" are executed in parallel, while within each "scenario", the requests are executed sequentially. 
  
  Each request has several basic fields, like path, method, payload, and additonalHeaders, and also a field called "luaScript" (see below).
  
  path, method, payload, and additonalHeaders, as the names suggest, are the path header, method header, message body, and other additional headers (such as user-agent) to build the request.
  
  In which the path field is a compound structure, which aims to provide some quick and handy options for quick definition of some typical test scenario. 
  
  For example, the user can specify in the path field to copy the path from the request prior to this one (sameWithLastOne), or to extract the path value from some specific header of the response responding to the request prior to this one (extractFromLastResponseHeader). Of course, direct input of the path is also supported.
  
  To generate configuration file from the Json schema above, it is recommended to use a GUI based Json editor.

  There are a couple of online Json editors available, for example: 

    https://json-editor.github.io/json-editor/
	
    https://pmk65.github.io/jedemov2/dist/demo.html

  Take https://pmk65.github.io/jedemov2/dist/demo.html for example:
  
  Paste raw content of https://raw.githubusercontent.com/wallyatgithub/h2loadrunner/main/config_schema.json to edit box of "Schema" tab of the above link
  
  Then click "Generate Form" button, a form named h2loadrunner_configuration is available in top left "Form" tab
  
  Check the help text associated with each field, to know what to fill/choose for each field.

  Leave the field with the default value if you are not sure what to fill/choose.

  After finishing editing the form, click "Output" tab, to get the JSON data from the edit box.
  
  Copy the content of the edit box and save it to a file <JSON FILE>, then use h2loadrunner --config-file=<JSON FILE> to start the load run

  Example screenshot of another Json editor at https://pmk65.github.io/jedemov2/dist/demo.html:
  
  ![Example of GUI configuration](https://raw.githubusercontent.com/wallyatgithub/h2loadrunner/main/Json_editor.png)
  ![Example of GUI configuration of scenario](https://raw.githubusercontent.com/wallyatgithub/h2loadrunner/main/Json_editor-scenario.png)
  
  
  If wanted, it is possible to override some parameters with command line interface after the Json configuration file is provided.

  For example, with this command line:

    h2loadrunner --config-file=config.json -t 1 -c 3 --rps=1 -D 100  
  
  Command line input (1 thread, 3 connections, rps 100, duration 100) coming after --config-file will override those respective fields in config.json.

 
# Lua script support
  
  Now comes the "luaScript" field:

  Like wrk/wrk2, h2loadrunner supports Lua script, capable of customizing every header and payload of the request to be sent.
  
  The "luaScript" field is associated with each request within the "scenarios" section. The Lua script will be executed by h2loadrunner for the request, as long as the request has a valid snippet of script (see next for format and naming convention of the snippet of script).

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
  
  


