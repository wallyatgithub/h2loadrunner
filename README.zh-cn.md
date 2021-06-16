[![license](https://img.shields.io/github/license/wallyatgithub/h2loadrunner.svg?style=flat-square)](https://github.com/wallyatgithub/h2loadrunner)

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
  

# 为什么要重新造一个轮子?
  出发点：寻找一个合适的能支持5G SBA （基于HTTP2）性能测试工具。

  当前，比较常见的HTTP2性能测试工具，大概应该是JMeter加载Blazemeter的HTTP2插件。

  但是，这个方法有几个比较明显的缺点：

  1. 性能不太好，需要的硬件资源多，但是产生的测试流量却不够大。

     跑JMeter的机器如果不够强，跑的时间长一点，因为Java垃圾回收等原因，产生的测试流量波动很大
   
  2. JMeter加载Blazemeter的HTTP2插件，无法使用HTTP2的多stream并发的特性。
   
     https://github.com/Blazemeter/jmeter-http2-plugin


  所以，JMeter加载Blazemeter的HTTP2插件的方式，不是很行。

  大家耳熟能详的HTTP性能测试工具，比如wrk, wrk2, 却又不支持HTTP2。

  有一些其它看起来很高大上的项目，比如Gatling，还有Locust，看上去就挺沉重，需要用户具有一定的编程功力，对想要快速上手的人不够友好。
  
  而且虽然Gatling宣称支持HTTP2，但是到底坑有几个，性能如何，并没有明确的数据。

  而至于Locust，则没有声称对HTTP2的支持，也许它所用的http client能自带HTTP2支持，但是疗效如何，暂时未知。

  如果去搜索"http 2 benchmark tool"，第一个出来的是h2load。
  
  前面说了，h2load只能支持静态的URI和URI列表，无法做Request之间的关联，无法动态定制Request的内容，所以，好像其实根本没有轮子。

  正因于此，造一个轮子也是不得已。

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
  
  


