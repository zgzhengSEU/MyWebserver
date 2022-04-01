#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <pthread.h>

#include <cstdio>
#include <exception>
#include <list>

// 引用 locker 线程同步类，因为工作队列被所有线程共享
#include "../locker/locker.h"
#include "../log/log.h"

template <typename T>  // T 表示任务类
class threadPool {
 public:
  threadPool(int thread_number = 4, int max_request = 20000);
  ~threadPool();
  bool append(T* request);  // 往请求队列中加入任务

 private:
  // 工作线程的运行函数，不断从请求队列中取出任务并执行
  static void* worker(void* arg);
  void run();

 private:
  int m_thread_number;        // 线程池中的线程数
  int m_max_requests;         // 请求队列中最大的请求数
  pthread_t* m_threads;       // 描述线程池的数组
  std::list<T*> m_workQueue;  // 请求队列，节点类型为任务 T
  locker m_queueLocker;       // 请求队列的互斥锁
  sem m_queueStat;            // 信号量，说明是否有任务需要处理
  bool m_stop;                // 是否结束线程
};

template <typename T>
threadPool<T>::threadPool(int thread_number, int max_requests)
    : m_thread_number(thread_number),
      m_max_requests(max_requests),
      m_stop(false),
      m_threads(NULL) {
  if ((thread_number <= 0) || (max_requests <= 0)) throw std::exception();
  LOG_INFO("%s%d", "thread_number is ", thread_number);
  LOG_INFO("%s%d", "max_requests is ", max_requests);
  m_threads = new pthread_t[m_thread_number];
  if (!m_threads) throw std::exception();

  // 创建 thread_number 个线程，并将他们设置为脱离线程
  for (int i = 0; i < thread_number; ++i) {
    // 第三个参数制定新建线程需要执行的函数
    // pthread_create 成功返回 0，失败返回错误码
    if (pthread_create(m_threads + i, NULL, worker, this) != 0) {
      delete[] m_threads;
      throw std::exception();
    }
    // 主线程和子线程分离，子线程结束后，资源自动回收
    if (pthread_detach(m_threads[i]) != 0) {  // 成功返回0，错误返回错误号
      delete[] m_threads;
      throw std::exception();
    }
  }
}

template <typename T>
threadPool<T>::~threadPool() {
  delete[] m_threads;
  m_stop = true;
}

// append 往请求队列中加入 T类型 任务
template <typename T>
bool threadPool<T>::append(T* request) {
  // 操作工作队列一定要加锁，因为它被所有线程共享
  m_queueLocker.lock();
  if (m_workQueue.size() > m_max_requests) {  // 队列已满
    m_queueLocker.unlock();
    return false;
  }

  m_workQueue.push_back(request); // 添加任务
  m_queueLocker.unlock();
  m_queueStat.post(); // 任务信号量+1
  return true;
}

template <typename T>
void* threadPool<T>::worker(void* arg) { // work的参数是进程池的this
  threadPool* pool = (threadPool*)arg;
  pool->run();
  return pool;
}

template <typename T>
void threadPool<T>::run() {
  while (!m_stop) {
    m_queueStat.wait();    // 阻塞等待 sem > 0
    m_queueLocker.lock();  // 上锁

    if (m_workQueue.empty()) { // 请求队列为空，没有任务
      m_queueLocker.unlock();
      continue;
    }

    // 从请求队列中拿出一个工作，并且更新队列
    T* request = m_workQueue.front();  // 拿出第一个工作
    m_workQueue.pop_front();           // 更新队列

    m_queueLocker.unlock();            // 解锁，关键代码区结束
    if (!request) continue;

    // 工作线程处理工作
    request->process();  // 调用模板类的 process 方法，即 http 类的 process
  }
}

#endif  // THREADPOOL_H
