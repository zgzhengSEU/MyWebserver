# 半同步/半反应堆线程池

## 为什么使用线程池

该 Web 服务器属于 I/O 密集型，I/O 操作通常伴随着阻塞浪费 CPU，使用多线程可以有效解决这个问题，而线程池减少了创建线程的成本。



## 线程池

本项目中线程池拥有以下特性：

### 双向链表实现的请求队列

主线程将处理好的对象插入请求队列中，工作线程从队列中获取请求对象。



### 半同步/半反应堆 + 同步 I/O 模拟  Proactor  模式

- 主线程为异步，监听所有的 socket 事件，包括对新连接的处理，执行数据的读写，将处理好的数据封装成请求放进请求队列中，并通知工作线程 **完成事件**。
- 工作线程竞态地从队列中获取工作，对工作进行同步逻辑处理，然后返回 **就绪事件**。



## 工作流程

1. 主线程通过 epoll_wait 监听 socket 上的事件，假如接受到 EPOLLIN 事件。
2. 主线程读取数据，将处理好的读完成事件放入请求队列中，并通过**改变信号量**唤醒工作线程。
3. 工作线程通过信号量的改变被唤醒，竞态地从队列中获取工作。处理完工作后，往 epoll 注册写就绪事件。

### 



