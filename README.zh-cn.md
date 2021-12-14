[![license](https://img.shields.io/github/license/wallyatgithub/h2loadrunner.svg?style=flat-square)](https://github.com/wallyatgithub/h2loadrunner)
![build status](https://github.com/wallyatgithub/h2loadrunner/actions/workflows/cmake.yml/badge.svg)

*Read this in other languages: [English](README.md).*

# h2loadrunner 是一个用于HTTP 1.x / HTTP2的性能测试工具
  h2loadrunner既支持HTTP 1.x，也支持HTTP2。
  
  h2loadrunner是从nghttp2项目的h2load工具fork而来。

  h2load本身基于libEv构建，借助libEv底层的epoll/kqueue等机制，h2load能够产生非常巨大的测试流量，并且在多核环境下，性能可以线性提升
  
  但是，h2load缺乏若干必要的功能，比如可变的URI和可变的payload，前后Request之间的关联，Request可编程，等等，使它无法作为一个生产力工具。

  所以，h2loadrunner在h2load良好性能架构的基础上，加入了若干必备的功能，使得其能满足一个性能测试工具的基本要求。
  
  1. 支持可变的URI和payload
  
  2. Stream 超时处理
  
  3. 前后Request之间的跟踪关联（典型场景：5G SBA中的事件订阅机制）
  
  4. 原生支持解析处理Set-Cookie，以及发送Cookie；并且支持可选的配置，允许在一个Request执行之前清空Cookie
  
     Cookie的处理符合https://tools.ietf.org/html/rfc6265
  
  5. 支持加载Lua脚本，实现对HTTP / HTTP2 Request消息的完全定制
  
  6. 支持命令行配置和JSON配置文件，可以通过JSON文件可视化编辑配置数据，方便定制复杂的测试场景
     
  7. 动态报告测试进度和主要统计数据，并且支持动态的改变测试流量大小
  
  8. 支持异步的方式建立动态连接

  9. 支持在Request执行结束之后，延迟执行下一个Request，延迟时间可通过JSON配置

  10. 支持配置不同的status code，来判定统计结果中response是成功的还是失败的
  
  11. 支持mTLS
  
  12. 支持建立并行的连接到负载均衡模式下的一组主机，并支持连接的failover和failback


# 如何编译构建目标可执行文件

  https://github.com/wallyatgithub/h2loadrunner#how-to-build


# 用法

  https://github.com/wallyatgithub/h2loadrunner#usage

# Lua脚本的支持

  https://github.com/wallyatgithub/h2loadrunner#lua-script-support
  
    
# HTTP 1.x 的支持
  
  https://github.com/wallyatgithub/h2loadrunner#http-1x-support
  
  


