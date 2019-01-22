# mstore

#### 项目介绍
mstore是一款简单的高并发服务端框架，基于epoll，支持通过安装动态模块的方式，来扩展框架的功能，开发者可以根据自己熟悉的语言来编写模块，加入到框架中，以实现需要的功能。目前支持c和java


#### 软件架构


![mstore结构](https://images.gitee.com/uploads/images/2018/1114/171314_98dfb08a_1516839.png "mstore_structure.png")


如上图所示，mstore的架构特点为：

1. 多进程方式
2. 像容器一样，可根据需要安装多个不同的功能模块
3. 各模块和mstore通过消息总线（eventpoll）共享外部网络事件，模块之间互不干扰
4. 支持被动访问方式和主动连接后端的使用方式（代理服务器方式）
5. 某些模块本身可以当作容器被使用，内部再跑子程序，达到用其它语言开发模块的目的，如JVM模块


#### 展望
考虑后续增加更多功能模块，如通用的代理服务器模块，python子模块，LUA 模块 等

