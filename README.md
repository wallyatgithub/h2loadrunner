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


