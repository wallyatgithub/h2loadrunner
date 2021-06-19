[![license](https://img.shields.io/github/license/wallyatgithub/h2loadrunner.svg?style=flat-square)](https://github.com/wallyatgithub/h2loadrunner)
![build status](https://github.com/wallyatgithub/h2loadrunner/actions/workflows/cmake.yml/badge.svg)

*Read this in other languages: [English](README.md).*

# h2loadrunner 是一个用于HTTP 1.x / HTTP2的性能测试工具
  h2loadrunner既支持HTTP 1.x，也支持HTTP2。
  
  h2loadrunner是从nghttp2项目的h2load工具fork而来。

  h2load本身基于libEv构建，借助libEv底层的epoll/kqueue等机制，h2load能够产生非常巨大的测试流量。
  
  但是，h2load缺乏若干必要的功能，比如可变的URI和可变的payload，前后Request之间的关联，Request可编程，等等，使它无法作为一个生产力工具。

  所以，h2loadrunner在h2load良好性能架构的基础上，加入了若干必备的功能，使得其能满足一个性能测试工具的基本要求。
  
  1. 支持可变的URI和payload
  
  2. Stream 超时处理
  
  3. 前后Request之间的跟踪关联（典型场景：5G SBA中的事件订阅机制）
  
  4. 支持加载Lua脚本，实现对HTTP / HTTP2 Request消息的完全定制
  
  5. 支持命令行配置和JSON配置文件，可以通过JSON文件可视化编辑配置数据，方便定制复杂的测试场景
     
  6. 动态报告测试进度和主要统计数据，并且支持动态的改变测试流量大小
  

# h2loadrunner性能如何?
  对于如下的测试场景，执行60K QPS/s 的测试，h2loadrunner只需要消耗不到一个CPU逻辑核心:
  
    POST，根据模板动态生成path，以及根据设定规则动态，更新消息体模板，并携带更新后的消息体，消息体大约300字节
    监测POST响应，并根据设定好的规则从中检索出新的path，对新的path发起PATCH操作，并携带300字节的根据模板动态生成的消息体    
    监测PATCH的响应，收到后发起DELETE请求

  60K QPS/s的CPU利用率:
  
   2756 root      20   0  366240  39668   7340 S  87.5   0.5   0:02.56 h2loadrunner
   2756 root      20   0  366240  40988   7340 S  96.0   0.5   0:03.04 h2loadrunner
   2756 root      20   0  366240  42044   7340 S  96.1   0.5   0:03.53 h2loadrunner
   2756 root      20   0  366240  43364   7340 S  96.0   0.5   0:04.01 h2loadrunner
   2756 root      20   0  366240  44684   7340 S  96.1   0.5   0:04.50 h2loadrunner
   2756 root      20   0  366240  45740   7340 S  94.0   0.6   0:04.97 h2loadrunner

  120K QPS/s 的CPU利用率，基本上是60K的线性叠加，并无其它额外开销或者瓶颈：
  
   2749 root      20   0  404640 102876   7460 S 198.0   1.3   0:23.21 h2loadrunner
   2749 root      20   0  404640 102876   7460 S 190.2   1.3   0:24.18 h2loadrunner
   2749 root      20   0  404640 102876   7460 S 198.0   1.3   0:25.17 h2loadrunner
   2749 root      20   0  404640 102876   7460 S 190.2   1.3   0:26.14 h2loadrunner
   2749 root      20   0  404640 102876   7460 S 198.0   1.3   0:27.13 h2loadrunner
   2749 root      20   0  404640 102876   7460 S 190.2   1.3   0:28.10 h2loadrunner
   2749 root      20   0  404640 102876   7460 S 196.0   1.3   0:29.08 h2loadrunner
 
  所以h2loadrunner在多核机器上，输出可以线性增长。
  
  需要额外备注的是，为了增加对h2loadrunner的压力，测试用的mock server刻意不应答一小部分应答，或者故意回复404应答，其中不应答的部分大约占3%
  
  这使得h2loadrunner在测试的同时，同时需要处理一小部分响应超时的情况，不然的话，累积的超时请求会造成Stream资源无法释放，进而阻塞测试
  
  测试结果显示，对于这样一个120K QPS/s的测试，外加3%响应超时处理，h2loadrunner应对的毫无压力：
  
   Sat Jun 19 12:44:11 2021, send: 121083, successful: 109202, 3xx: 0, 4xx: 8427, 5xx: 0, max resp time (us): 2031516, min resp time (us): 1204, successful/send: 90.1877%
   Sat Jun 19 12:44:12 2021, send: 120320, successful: 108389, 3xx: 0, 4xx: 8489, 5xx: 0, max resp time (us): 2035583, min resp time (us): 1104, successful/send: 90.0839%
   Sat Jun 19 12:44:13 2021, send: 120016, successful: 108134, 3xx: 0, 4xx: 8374, 5xx: 0, max resp time (us): 2022545, min resp time (us): 908, successful/send: 90.0997%
   Sat Jun 19 12:44:14 2021, send: 120069, successful: 108211, 3xx: 0, 4xx: 8347, 5xx: 0, max resp time (us): 2018241, min resp time (us): 1080, successful/send: 90.124%
   Sat Jun 19 12:44:15 2021, send: 120119, successful: 108290, 3xx: 0, 4xx: 8325, 5xx: 0, max resp time (us): 2017653, min resp time (us): 1012, successful/send: 90.1523%
   Sat Jun 19 12:44:16 2021, send: 119963, successful: 108078, 3xx: 0, 4xx: 8455, 5xx: 0, max resp time (us): 2023169, min resp time (us): 1097, successful/send: 90.0928%
   Sat Jun 19 12:44:17 2021, send: 119884, successful: 107971, 3xx: 0, 4xx: 8450, 5xx: 0, max resp time (us): 2022223, min resp time (us): 733, successful/send: 90.0629%
   Sat Jun 19 12:44:18 2021, send: 120210, successful: 108380, 3xx: 0, 4xx: 8349, 5xx: 0, max resp time (us): 2021652, min resp time (us): 870, successful/send: 90.1589%
   

# 如何编译构建目标可执行文件

  https://github.com/wallyatgithub/h2loadrunner#how-to-build

# 基本用法
  https://github.com/wallyatgithub/h2loadrunner#basic-usage


# 基于JSON格式的配置，以及如何用图形界面编辑JSON配置数据

  https://github.com/wallyatgithub/h2loadrunner#json-configuration-support-and-gui-interface-for-configuration

# Lua脚本的支持

  https://github.com/wallyatgithub/h2loadrunner#lua-script-support
  
    
# HTTP 1.x 的支持
  
  https://github.com/wallyatgithub/h2loadrunner#http-1x-support
  
  


