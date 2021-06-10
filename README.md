# h2loadrunner
http2 load runner, initially forked from nghttp2/nghttp2 h2load, with addtional features added.

Background information:
======================
The motivation is to provide an alternative tool for 5G SBI load test.

Most likely, today, JMeter is used to run http2 load test, just like how it was used in http 1.x load test.

However, the problems with JMeter for http2 load test are:

1. JMeter requires a considerable amount of resource, yet not generating very heavy test load.

2. Synchronized Request is used in JMeter for http2, in order to assert the responses:

   https://github.com/Blazemeter/jmeter-http2-plugin

   This actually prevents concurrent streams and flow controls occurring, which are very key features of http2.

So a new tool is needed to run http2 load test.

H2load from nghttp2 project is chosen as a very good base for the new tool.

Based on h2load, these additional features haven been added, as part of the new tool (h2loadrunner):

1. Add variable support in request URI and JSON data payload; with this, a range of user ID, like IMSI/MSISDN can be specified, for h2loadrunner to dynamically replace in the request URI and JSON payload.

2. Add stream timeout watch, to send RST_STREAM for timeout request; with this, it becomes a very robust tool in latency test, without the possibility of resource hang.

3. Add support for location header (customizable) tracking, to automatically identify any kind of subscription/resource created; with this, h2loadrunner can generate traffic mix of subscription/update/subscription automatically.

4. Dynamic change of TPS during the test. h2load already support constant of transactions per second (--rps option), which is quite useful to observe the system resource consumption under a fixed load.

   Now with the feature of dynamic change of the TPS in h2loadrunner, it is possible to steer the load up and down easier without restarting the load.

5. Dynamic report the load test. h2loadrunner will print the load test result every second, to make the test more intuitive.



Required packages to build and run:
==================================
openSSL
libEv
nghttp2

How to build:
============
Use cmake to build

$git clone https://github.com/wallyatgithub/h2loadrunner.git

$cd h2loadrunner

$mkdir build

$cd build

$cmake ..

$cmake --build ./

h2loadrunner would be generated

How to run:
==========
$./h2loadrunner http://172.18.62.237:8080/nudm-ee/v2/imsi-2621012-USER_ID/ee-subscriptions --crud-create-method=POST --crud-read-method=GET --crud-update-method=PATCH --crud-delete-method=DELETE --crud-create-data-file=datafile.json --crud-update-data-file=updatedata.json --crud-request-variable-name="-USER_ID" --crud-request-variable-value-start=1 --crud-request-variable-value-end=1000000000 --crud-resource-header="location" -H "content-type: application/json" --stream-timeout-interval-ms=2000 --rps=1000 -t 1 -c 10 -D 3000 -m 512

Here is what is going on with the above command:

First, "POST" (--crud-create-method) request is sent to the URI (http://172.18.62.237:8080/nudm-ee/v2/imsi-2621012-USER_ID/ee-subscriptions) with "-USER_ID" replaced by an actual user ID whose range starts from 1 (--crud-request-variable-value-start) to 1000000000 (--crud-request-variable-value-end), with payload conent spelcified in file datafile.json (--crud-create-data-file)

Example content of datafile.json:
{"callbackReference":"http://10.10.177.251:32050/nhss-ee/v1/msisdn-491971103488-USER_ID/ee-subscriptions","monitoringConfiguration":{"120984":{"eventType":"UE_REACHABILITY_FOR_SMS","immediateFlag":false,"referenceId":120984}},"reportingOptions":{"maxNumOfReports":0}}

The "POST" response is monitored for the header named "location" (--crud-resource-header), whose value is a URI, which is the resource (EE-subscription) creatd by "POST".

Next, "GET" (--crud-read-method) is sent to the URI above to query the created resource

After "GET" response is received, "PATCH" (--crud-update-method) is sent to the URI to update the resource created, with payload specified in updatedata.json (--crud-update-data-file)

Example content of updatedata.json:
{"callbackReference":"http://10.10.177.251:32050/nhss-ee/v1/msisdn-491971103488-USER_ID/ee-subscriptions","monitoringConfiguration":{"220984":{"eventType":"UE_REACHABILITY_FOR_SMS","immediateFlag":false,"referenceId":120984}},"reportingOptions":{"maxNumOfReports":0}}


At last, "DELETE" (--crud-delete-method) is sent to delete the created resource, which is actually an unsubscription here in this case

other parameters:

--stream-timeout-interval-ms:

how long would h2loadrunner wait for a response to come; when this is exceeded, RST_STREAM is sent to release the resource

--rps: desired request per second

-t: number of thread

-c: number of client, which is typically the number of connections

-D: how long the test should run

-m: max concurrent streams per connection

Example out put during the load runs:
Application protocol: h2c
Mon Jun  7 05:17:42 2021, actual RPS: 9585, successful responses: 9585, 3xx: 0, 4xx: 693, 5xx: 0, max resp time (us): 58217, min resp time (us): 451, successful rate: 100%


For other possible parameters (derived from h2load), type h2loadrunner --help




2021-06-10 update:
=================
h2loadrunner now supports JSON based configuration.

With this feature, h2loadrunner can support flexibile scenario combinations, not limiting to CRUD.

Json schema: config_schema.json

Example Json data to give to h2loadrunner: example_config.json

It is recommended to use a Json editor to load the schema, and input data (Of course you can do it manually, but it is error-prone when dealing with scenarios section)

Export Json data, and save to a file <JSON FILE>

Then use h2loadrunner --config-file=<JSON FILE> to start the load run

Note: 
a handy Json editor is included this this repo under third-party/onde:

    Open the file third-party/onde/samples/app.html in a web browser (Firefox or Safari, won't work with Chrome locally due its strict cross-origin policy).

    Click the "Edit Schema" menu item.

    Paste the Json schema into the text box

    Push the "Update schema" button.

    Edit data

    Click "Export", and copy the generated Json data, and save it to a file <JSON FILE>

    Use h2loadrunner --config-file=<JSON FILE> to start the load run

    Credits:
    ======
    onde: https://github.com/exavolt/onde
