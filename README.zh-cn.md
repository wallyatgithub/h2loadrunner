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
  对于如下的测试场景，执行60K/s QPS的测试，h2loadrunner只需要消耗8th Gen i3的一个逻辑核心:
  
    POST，根据模板动态生成path，以及根据设定规则动态，更新消息体模板，并携带更新后的消息体，消息体大约300字节
    监测POST响应，并根据设定好的规则从中检索出新的path，对新的path发起PATCH操作，并携带300字节的根据模板动态生成的消息体    
    监测PATCH的响应，收到后发起DELETE请求
    
  需要额外备注的是，为了增加对h2loadrunner的压力，测试用的mock server刻意不应答一小部分应答，或者故意回复404应答，其中不应答的部分大约占3%
  
  这使得h2loadrunner在测试的同时，同时需要处理一小部分响应超时的情况，不然的话，累积的超时请求会造成Stream资源无法释放，进而阻塞测试，
  
  测试结果显示，对于这样一个600K QPS/s的测试，外加3%响应超时，h2loadrunner应对的毫无压力：
  
    Fri Jun 18 11:35:13 2021, send: 59939, received: 58208, 3xx: 0, 4xx: 4275, 5xx: 0, max resp time (us): 2012445, min resp time (us): 444, received/send: 97.1121%
    Fri Jun 18 11:35:14 2021, send: 59952, received: 58204, 3xx: 0, 4xx: 4161, 5xx: 0, max resp time (us): 2012321, min resp time (us): 524, received/send: 97.0843%
    Fri Jun 18 11:35:15 2021, send: 60003, received: 58256, 3xx: 0, 4xx: 4221, 5xx: 0, max resp time (us): 2012618, min resp time (us): 423, received/send: 97.0885%
    Fri Jun 18 11:35:16 2021, send: 60068, received: 58294, 3xx: 0, 4xx: 4179, 5xx: 0, max resp time (us): 2012263, min resp time (us): 413, received/send: 97.0467%
    Fri Jun 18 11:35:17 2021, send: 59977, received: 58229, 3xx: 0, 4xx: 4232, 5xx: 0, max resp time (us): 2012121, min resp time (us): 395, received/send: 97.0855%
    Fri Jun 18 11:35:18 2021, send: 60106, received: 58372, 3xx: 0, 4xx: 4235, 5xx: 0, max resp time (us): 2012465, min resp time (us): 451, received/send: 97.1151%
    Fri Jun 18 11:35:19 2021, send: 60000, received: 58257, 3xx: 0, 4xx: 4183, 5xx: 0, max resp time (us): 2012432, min resp time (us): 420, received/send: 97.095%
    Fri Jun 18 11:35:20 2021, send: 59856, received: 58144, 3xx: 0, 4xx: 4165, 5xx: 0, max resp time (us): 2012015, min resp time (us): 409, received/send: 97.1398%
    Fri Jun 18 11:35:21 2021, send: 60119, received: 58375, 3xx: 0, 4xx: 4246, 5xx: 0, max resp time (us): 2012814, min resp time (us): 478, received/send: 97.0991%
    Fri Jun 18 11:35:22 2021, send: 59942, received: 58178, 3xx: 0, 4xx: 4176, 5xx: 0, max resp time (us): 2012436, min resp time (us): 502, received/send: 97.0572%
    Fri Jun 18 11:35:23 2021, send: 60040, received: 58308, 3xx: 0, 4xx: 4221, 5xx: 0, max resp time (us): 2012505, min resp time (us): 419, received/send: 97.1153%

  CPU usage:
  
    50037 root      20   0  327588  64840   7156 S 100.0   0.8   0:11.37 h2loadrunner
    50037 root      20   0  327588  66160   7156 S 103.9   0.8   0:11.90 h2loadrunner
    50037 root      20   0  327588  67216   7156 S 106.0   0.8   0:12.43 h2loadrunner
    50037 root      20   0  327588  68272   7156 S 102.0   0.8   0:12.95 h2loadrunner
    50037 root      20   0  327588  69592   7156 S 103.9   0.9   0:13.48 h2loadrunner
    50037 root      20   0  327588  70648   7156 S 104.0   0.9   0:14.00 h2loadrunner
    50037 root      20   0  327588  71704   7156 S 103.9   0.9   0:14.53 h2loadrunner

# 为什么要重新造一个轮子?
  出发点：寻找一个合适的能支持5G SBA （基于HTTP2）性能测试工具。

  当前，比较常见的HTTP2性能测试工具，大概应该是JMeter加载Blazemeter的HTTP2插件。

  但是，这个方法有几个比较明显的缺点：

  1. 性能不太好，需要的硬件资源多，但是产生的测试流量却不够大。

     跑JMeter的机器如果不够强，跑的时间长一点，因为Java垃圾回收等原因，产生的测试流量波动很大
   
  2. JMeter加载Blazemeter的HTTP2插件，无法使用HTTP2的多stream并发的特性。
   
     https://github.com/Blazemeter/jmeter-http2-plugin


  所以，JMeter加载Blazemeter的HTTP2插件的方式，不是非常行。

  大家耳熟能详的HTTP性能测试工具，比如wrk, wrk2, 却又不支持HTTP2。

  有一些其它看起来很高大上的项目，比如Gatling，还有Locust，看上去就挺沉重，需要用户具有一定的编程功力，对想要快速上手的人不够友好。
  
  而且虽然Gatling宣称支持HTTP2，但是到底坑有几个，性能如何，并没有明确的数据。

  而至于Locust，则没有声称对HTTP2的支持，也许它所用的http client能自带HTTP2支持，但是疗效如何，暂时未知。

  如果去搜索"http 2 benchmark tool"，第一个出来的是h2load。
  
  前面说了，h2load只能支持静态的URI和URI列表，无法做Request之间的关联，无法动态定制Request的内容。
  
  envoyproxy下面有一个性能测试项目，叫做nighthawk，看起来非常专业，既能支持HTTP1, 又能支持HTTP2。
  
  但是仔细一看，基本配置情况下，它似乎只能生成单一的静态请求，或者是重放之前记录的请求。虽然架构看起来很先进，但是要生成满足要求的测试流量，实际也不是那么轻而易举。
  
  

  所以看起来，现存的工具要么直接不行，要么有局限性没有实际可用性，要么太重量级不好上手，并且存在不确定性。
  
  所以，再造一个轮子，也不算是多此一举。

  h2load基于libEv，底层是epoll，支持多线程并发，从性能角度考虑，应该没什么问题。
  
  而且h2load出自nghttp2项目，对HTTP2的支持应该非常正统。

  基于h2load来造这个轮子，是一个很好的选择。

  所以，在h2load的基础上，加入了本文开头提到的几个性能测试必备的功能，就成了h2loadrunner。

  某种程度上，这不算造轮子，因为不造的话，只有轮毂，而只有轮毂，车是跑不动的；
  
  被造的，是安装到轮毂上的轮胎。

  问：为什么不把改动合并回nghttp2项目？

  答：pull request长时间没人理，是一个原因。

  而且，这些改动并不属于nghttp2的核心，即协议部分，h2load本身就是一个独立的工具，所以把h2loadrunner作为一个单独的工具，解除对nghttp2特定版本的绑定，未尝不可。


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
  
  


