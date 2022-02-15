[![license](https://img.shields.io/github/license/wallyatgithub/h2loadrunner.svg?style=flat-square)](https://github.com/wallyatgithub/h2loadrunner)
![build status](https://github.com/wallyatgithub/h2loadrunner/actions/workflows/cmake.yml/badge.svg)

*Read this in other languages: [English](README.md).*

# h2loadrunner 是一个用于HTTP 1.x / HTTP2的性能测试工具
  h2loadrunner既支持HTTP 1.x，也支持HTTP2。

  h2loadrunner最初是从nghttp2项目的h2load工具fork而来，并加入了许多有新功能

  与h2load不一样的是，h2loadrunner基于Boost ASIO而不是Libev构建，以获得最佳的性能和最佳的可移植性

  Libev的支持虽然还保留，但处于depricated状态，在不久的将来，Libev的支持可能会被移除

  以下是h2loadrunner支持的新功能，这些功能在nghttp2项目的h2load工具中均不支持：

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

  12. 支持建立并行的连接到负载均衡模式下的一组主机

  13. 连接的failover和failback


# 如何编译构建目标可执行文件

  https://github.com/wallyatgithub/h2loadrunner#how-to-build


# 用法

  https://github.com/wallyatgithub/h2loadrunner#usage

# Lua脚本的支持

  https://github.com/wallyatgithub/h2loadrunner#lua-script-support
  
    
# HTTP 1.x 的支持
  
  https://github.com/wallyatgithub/h2loadrunner#http-1x-support
  
  


