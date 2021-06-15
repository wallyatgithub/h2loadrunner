# h2loadrunner is an HTTP and HTTP2 benchmarking / load testing / performance testing tool
  h2loadrunner is a benchmarking tool supporting both HTTP 1.x and HTTP2.
  
  It was forked from the h2load utility of nghttp2,  yet with a number of powerful features added.
  
  Thanks to libEv (w/ epoll/poll/kqueue), like h2load, h2loadrunner can generate a very large amount of load with multi-core.
  
  Besides, h2loadrunner supports powerful features that are not present h2load:
  
  1. Variable support in URI and message body.
  
  2. Stream timeout handling.
  
  3. Transaction support with specific resource header tracking(e.g. location header in 5G SBA).
  
  4. Lua script support.
     With lua script, user can customize every header and the payload of the request to be sent.
  
  5. Both command line interface and JSON based configuration.
     With JSON configuration, user can build the test scenario with a GUI editor.
     
  6. Dynamic report of the test, dynamic change of the QPS/RPS.
     h2loadrunner prints the test statistics every second; it also supports dynamic change of QPS/RPS.
  

# Why h2loadrunner?
  The initial motivation is to make a performant tool for 5G SBA load test on HTTP2.

  Currently, the common practice for HTTP2 performance test is to use JMeter with HTTP2 plugin from Blazemeter.

  However, there are a number of problems with JMeter:

  1. JMeter requires a considerable amount of compute resource, yet not generating very large amount of load.

  2. Synchronized Request is used in jmeter http2 plugin, in order to assert the responses:

     https://github.com/Blazemeter/jmeter-http2-plugin

     This actually prevents concurrent streams and flow controls occurring, which are very key features of HTTP2.
  
  The conclusion is, JMeter is not an ideal tool for HTTP2 load testing, at least as of today.

  Classic HTTP benchmarking tools, like wrk, wrk2, do NOT support HTTP2 at all.
  
  Gatling, which is believed to be powerful, yet, is heavy-weighted.
  
  Gatling requires Scala programing skill, is thus not easy for quick start and out-of-the-box usage.
  
  Locust, which aims to be a powerful tool for performance testing, however, requires Python programing skill, and is not ready for out-of-the-box usage.
  
  Besides, Locust has a number of terminologies like decorators, making it bit difficult for quick start.
  
  So, that is the background why this new tool is created.
  
  h2load is chosen to be the base of this new tool, as it comes from the nghttp2 project, which means it has the most native HTTP2 support.
  
  h2load uses libEv which is based on epoll/poll/kqueue, which makes it very performant with less footprint.
  
  Based on h2load, this new tool h2loadrunner is created, with a list of features added, making it at least a very good replacement to wrk/wrk2, yet, with full HTTP2 support.
  
  
  Is this kind of "reinventing the wheel"? 
  
  Well, I think not, as there is no such tool before, which is, simple, light-weighted, easy to start, with native HTTP2 support, robust, and with fully customizable HTTP/HTTP2 message.


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

  First, "POST" (--crud-create-method) request is sent to the URI (http://192.168.1.125:8080/nudm-ee/v2/imsi-2621012-USER_ID/sdm-subscriptions/) with "-USER_ID" replaced by an actual user ID whose range starts from 1 (--crud-request-variable-value-start) to 1000000000 (--crud-request-variable-value-end), with payload conent spelcified in file datafile.json (--crud-create-data-file)

  Example content of datafile.json:
      
    {"callbackReference":"http://10.10.177.251:32050/nhss-ee/v1/msisdn-491971103488-USER_ID/ee-subscriptions","monitoringConfiguration":{"120984":{"eventType":"UE_REACHABILITY_FOR_SMS","immediateFlag":false,"referenceId":120984}},"reportingOptions":{"maxNumOfReports":0}}

  The "POST" response is monitored for the header named "location" (--crud-resource-header), whose value is a URI, which is the resource (5G EE-subscription) creatd by "POST".

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

# How to build

  These packages are required to build h2loadrunner:
  
    openSSL
    libEv
    nghttp2
    liblua

  Use cmake to build

    $git clone https://github.com/wallyatgithub/h2loadrunner.git
    
    $cd h2loadrunner
    
    $mkdir build
    
    $cd build
    
    $cmake ..
    
    $cmake --build ./
    
    h2loadrunner would be generated


# JSON configuration support and GUI interface for configruation

  h2loadrunner supports JSON based configuration.
  
  With this feature, h2loadrunner can support flexibile scenario combinations, not limiting to CRUD (Create-Read-Update-Delete).
  
  Json schema: https://github.com/wallyatgithub/h2loadrunner/blob/main/config_schema.json
  
  Example Json data: https://github.com/wallyatgithub/h2loadrunner/blob/main/example_config.json
  
  It is recommended to use a Json editor to load the schema, and input data (Of course you can do it manually, but it is error-prone when dealing with scenarios section)
  
  https://github.com/wallyatgithub/h2loadrunner/blob/main/Json_editor.png
  
  Export Json data, and save to a file <JSON FILE>
  
  Then use h2loadrunner --config-file=<JSON FILE> to start the load run

  A handy Json editor (onde) is included this this repo under third-party/onde:

  Open the file third-party/onde/samples/app.html in a web browser (Firefox or Safari, won't work with Chrome locally due its strict cross-origin policy).
  
  Click the "Edit Schema" menu item.
  
  Paste the Json schema into the text box
  
  Push the "Update schema" button.
  
  Edit data
  
  Click "Export", and copy the generated Json data, and save it to a file <JSON FILE>
  
  Use h2loadrunner --config-file=<JSON FILE> to start the load run
  
  Acknowledgements:
  ================
  onde: https://github.com/exavolt/onde

# Lua script support


# HTTP 1.x support

