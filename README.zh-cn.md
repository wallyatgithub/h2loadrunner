[![license](https://img.shields.io/github/license/wallyatgithub/h2loadrunner.svg?style=flat-square)](https://github.com/wallyatgithub/h2loadrunner)
![build status](https://github.com/wallyatgithub/h2loadrunner/actions/workflows/cmake.yml/badge.svg)

*Read this in other languages: [English](README.md).*

# h2loadrunner 是一个用于HTTP 1.x / HTTP2 / HTTP3 的性能测试工具
  h2loadrunner既支持HTTP 1.x，也支持HTTP2，也支持HTTP3 over QUIC。

  h2loadrunner最初基于nghttp2项目的h2load工具构建。
  
  后来，许多强大的新的功能被陆续加入，并且，h2loadrunner重新设计了执行框架，使之成为一个功能强大的、高性能的、跨平台的负载测试工具

  现在，与h2load不一样的是，h2loadrunner基于Boost ASIO而不是Libev构建，以获得最佳的性能和最佳的可移植性

  旧的Libev的支持虽然还保留，但因为Libev无法在Windows平台上高效工作，所以对Libev的支持已经处于depricated状态，在不久的将来，h2loadrunner可能会完全放弃对Libev的支持

  说到h2loadrunner强大的新的功能，以下是h2loadrunner支持的新功能，这些功能在nghttp2项目的h2load工具中均不支持：

  1. 支持可变的URI和payload
  
  2. Stream 超时处理
  
  3. 前后Request之间的跟踪关联（典型场景：5G SBA中的事件订阅机制）
  
  4. 原生支持解析处理Set-Cookie，以及发送Cookie；并且支持可选的配置，允许在一个Request执行之前清空Cookie
  
     Cookie的处理符合https://tools.ietf.org/html/rfc6265
  
  5. 支持加载Lua脚本，实现对HTTP / HTTP2 Request消息的完全定制
  
  6. 通过JSON文件可视化编辑配置数据，方便定制复杂的测试场景
     
  7. 动态报告测试进度和主要统计数据，并且支持动态的改变测试流量大小
  
  8. 支持异步的方式建立动态连接

  9. 支持在Request执行结束之后，延迟执行下一个Request，延迟时间可通过JSON配置

  10. 支持配置不同的status code，来判定统计结果中response是成功的还是失败的

  11. 支持mTLS

  12. 支持建立并行的连接到负载均衡模式下的一组主机

  13. 连接的failover和failback

  14. h2loadrunner原生支持Linux的Windows，在不同平台下，h2loadrunner均表现出强大的性能，这得益于Boost ASIO优秀的设计，可以充分利用Linux的epoll和Windows的IOCP的能力
  
  15. h2loadrunner支持直接加载并运行HAR文件

# 如何快速尝试一下

  从 https://github.com/wallyatgithub/maock/releases 下载预编译好的Linux或者Windows 10的Maock可执行文件的压缩包
  
  解压，然后运行:

  ./maock ./maock.json # for linux
  
  or
  
  maock.exe maock.json # for windows 10
  
  Maock现在在8081端口监听，并且已经加载了预定义好的规则
 
  从 https://github.com/wallyatgithub/h2loadrunner/releases 下载预编译好的Linux或者Windows 10的H2loadrunner可执行文件的压缩包
  
  解压，然后运行:
  
  ./h2loadrunner --config-file=./h2load.json # for linux
  
  or
  
  h2loadrunner.exe --config-file=h2load.json # for windows 10
  
  现在，H2loadrunner已经启动，并向127.0.0.1:8081的Maock建立了两条连接，每条连接的测试流量是每秒100个请求

  请查看Maock和Hloadrunner的各自屏幕输出，来获得当前的流量统计信息

# 如何在Windows下编译构建h2loadrunner.exe

  https://github.com/wallyatgithub/h2loadrunner#how-to-build-on-windows

# 如何在Linux下编译构建h2loadrunner

  https://github.com/wallyatgithub/h2loadrunner#how-to-build-on-linux


# 用法

  https://github.com/wallyatgithub/h2loadrunner#usage

# Lua脚本的支持

  https://github.com/wallyatgithub/h2loadrunner#lua-script-support
  
    
# HTTP 1.x 的支持
  
  https://github.com/wallyatgithub/h2loadrunner#http-1x-support
  
  
# Luasio: A cross-platform high-performance web platform

  https://github.com/wallyatgithub/h2loadrunner#luasio-a-cross-platform-high-performance-web-platform

# HTTP3 的支持

  https://github.com/wallyatgithub/h2loadrunner#http3-support
  
# HAR 的支持

  https://github.com/wallyatgithub/h2loadrunner#har-support