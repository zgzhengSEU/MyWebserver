#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "./http/http_conn.h"
#include "./locker/locker.h"
#include "./log/log.h"
#include "./threadPool/threadPool.h"
#include "./timer/time_heap.h"

#define MAX_FD 65536            // 最大文件描述符
#define MAX_EVENT_NUMBER 65536  // 最大事件数
#define TIMESLOT 30             // 最小超时时间

//#define SYNLOG                      // 同步写日志
#define ASYNLOG  // 异步写日志

#define listenfdET  // 监听非阻塞ET
//#define listenfdLT                  // 监听阻塞LT

// 该三文件在 http_conn.cpp 中
extern int addfd(int epollfd, int fd, bool one_shot);
void removefd(int epollfd, int fd);
int setNonBlocking(int fd);

// 设置定时器相关的参数
static int pipefd[2];  // 父子进程通信管道，传递信号
static time_heap timer_heap;
static int epollfd = 0;
static bool isAlarm = false;  // 是否有正在处理的定时器事件

// 信号处理函数
void sig_handler(int sig) {
  // 保证重入性，记录原来的 errno
  // 重入性：中断后重新进入该函数，环境变量与之前一样
  int save_errno = errno;
  int msg = sig;

  // 传入信号的信号序号，但是 send 接受字符，转换一下
  send(pipefd[1], (char*)&msg, 1, 0);
  errno = save_errno;
}

// 设置信号函数
// 信号序号、回调函数、被打断的系统调用是否自动重新发起
void addsig(int sig, void(handler)(int), bool restart = true) {
  struct sigaction sa;
  memset(&sa, '\0', sizeof(sa));
  sa.sa_handler = handler;
  if (restart) sa.sa_flags |= SA_RESTART;
  sigfillset(&sa.sa_mask);
  assert(sigaction(sig, &sa, NULL) != -1);
}

// 定时处理任务，不断定时触发 SIGALRM 信号
void timer_handler() {
  // 执行心搏函数，会删除过期的定时器
  // 心搏函数里面会执行过期定时器的回调函数，即删除非活跃的socket的注册事件，并关闭
  timer_heap.tick();  

  heap_timer* temp = nullptr; // 临时定时器变量
  // 如果当前没有定时事件正在处理，且堆顶存在定时器
  if (!isAlarm && (temp = timer_heap.Top())) {
    // 堆顶为接下来最早过期的定时器，delay是到下一次过期的时间间隔
    time_t delay = temp->expire - time(nullptr);
    if (delay <= 0) delay = 1;
    alarm(delay);
    isAlarm = true;
  }
}

// 定时器回调函数，删除非活跃的socket的注册事件，并关闭
void cb_func(client_data* user_data) {
  // 1. 从内核事件表中删除事件
  epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
  assert(user_data);
  // 2. 关闭文件fd
  close(user_data->sockfd);
  // 3. 更新连接的用户
  http_conn::m_user_count--;
  // 4. 设置当前无正在处理定时事件
  isAlarm = false;
  LOG_INFO("close fd %d", user_data->sockfd);
  Log::get_instance()->flush();
}

void show_error(int connfd, const char* info) {
  printf("%s", info);
  send(connfd, info, strlen(info), 0);
  close(connfd);
}

int main(int argc, char* argv[]) {
#ifdef ASYNLOG
  Log::get_instance()->init("ServerLog", 2000, 800000, 8);  // 异步写日志
#endif

#ifdef SYNLOG
  Log::get_instance()->init("ServerLog", 2000, 800000, 0);  // 同步写日志
#endif

  if (argc <= 1) {
    printf("you need to input ip、port\n", basename(argv[0]));
    return 1;
  }

  int port = atoi(argv[1]);
  int thread_number = atoi(argv[2]); // 线程池大小
  int max_request = atoi(argv[3]); // 请求队列中最大的请求数
  // 屏蔽掉SIGPIPE信号
  addsig(SIGPIPE, SIG_IGN);

  // 创建线程池
  threadPool<http_conn>* pool = NULL;
  try {
    pool = new threadPool<http_conn>(thread_number, max_request);
  } catch (...) {
    return 1;
  }

  // http连接用户数组
  http_conn* users = new http_conn[MAX_FD];
  assert(users);

  int listenfd = socket(PF_INET, SOCK_STREAM, 0);
  assert(listenfd >= 0);

  int ret = 0;
  struct sockaddr_in address;
  memset(&address, '\0', sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(port);

  /*
    端口复用
    端口复用最常用的用途是:
    防止服务器重启时之前绑定的端口还未释放
    程序突然退出而系统没有释放端口
  */
  int flag = 1;
  setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));

  ret = bind(listenfd, (sockaddr*)&address, sizeof(address));
  assert(ret >= 0);

  ret = listen(listenfd, 5);
  assert(ret >= 0);

  // 创建内核事件表
  epoll_event events[MAX_EVENT_NUMBER];
  epollfd = epoll_create(5);
  assert(epollfd != -1);

  // 将监听 fd 注册到内核事件表，不能是 one_shot
  addfd(epollfd, listenfd, false);
  http_conn::m_epollfd = epollfd;

  // 创建父子通信管道
  ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
  assert(ret != -1);
  setNonBlocking(pipefd[1]);  // 写管道不阻塞，写满直接返回errno
  addfd(epollfd, pipefd[0], false);  // 注册管道的读事件

  // 信号处理函数，只关注 alarm 和 ctrl + c 发送的信号
  addsig(SIGALRM, sig_handler, false);
  addsig(SIGTERM, sig_handler, false);
  bool stop_server = false;

  // 定时器绑定的客户数据数组，socket、定时器、客户地址
  client_data* users_timer = new client_data[MAX_FD];

  bool timeout = false;  // 是否超时
  alarm(TIMESLOT);       // 定时触发 alarm

  // 只要不发 SIGTERM，则一直执行下面的语句（服务器一直运行）
  while (!stop_server) {
    int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
    if (number < 0 && errno != EINTR) {
      LOG_ERROR("%s", "epoll failure");
      break;
    }

    for (int i = 0; i < number; ++i) {
      int sockfd = events[i].data.fd;

      // 如果是新到的客户连接
      if (sockfd == listenfd) {
        struct sockaddr_in client_address;
        socklen_t client_address_len = sizeof(client_address);

        while (1) {
          int connfd = accept(listenfd, (struct sockaddr*)&client_address,
                              &client_address_len);
          if (connfd < 0) {
            LOG_ERROR("%s:errno is:%d", "accept error", errno);
            break;
          }
          if (http_conn::m_user_count >= MAX_FD) {
            show_error(connfd, "Internal server busy");
            LOG_ERROR("%s", "Internal server busy");
            continue;
          }

          users[connfd].init(connfd, client_address);

          // 创建一个定时器，timer的expire为当前+TIMESLOT
          auto timer = new heap_timer(TIMESLOT);
          // 设置用户数据，和定时器相关
          users_timer[connfd].address = client_address;
          users_timer[connfd].sockfd = connfd;
          users_timer[connfd].timer = timer;
          timer->user_data = &users_timer[connfd];  // 定时器绑定用户数据
          timer->cb_func = cb_func;     // 设置定时器回调函数
          timer_heap.add_timer(timer);  // 将这个定时器加入时间堆
         
          if (!isAlarm) {
            isAlarm = true; // 有正在处理的定时器事件
            alarm(TIMESLOT);
          }
        }
        continue;
      }

      // 连接关闭事件
      else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
        // 服务器关闭连接，移除定时器
        auto curtimer = users_timer[sockfd].timer;  // 当前连接对应的定时器
        curtimer->cb_func(&users_timer[sockfd]);    // 删除连接，关闭fd
        if (curtimer) timer_heap.del_timer(curtimer);
      }

      // 主程序处理信号，IO复用系统调用来监听管道上的读端文件描述符上的可读事件，统一事件源
      else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN)) {
        int sig;
        char signals[1024];
        ret = recv(pipefd[0], signals, sizeof(signals), 0);
        if (ret == -1)
          continue;
        else if (ret == 0)
          continue;
        else {
          // 因为每个信号值占1字节，所以按字节来逐个接收信号
          for (int i = 0; i < ret; ++i) {
            switch (signals[i]) {
              case SIGALRM: {    // 有超时事件
                timeout = true;  // 打上超时标记，表示有超时事件
                break;
              }
              case SIGTERM:
                stop_server = true;
                break;
            }
          }
        }
      }

      // 处理客户连接上接受到的数据
      else if (events[i].events & EPOLLIN) {
        auto timer = users_timer[sockfd].timer;
        if (users[sockfd].read_once()) {
          LOG_INFO("deal with the client(%s)",
                   inet_ntoa(users[sockfd].get_address()->sin_addr));
          Log::get_instance()->flush();

          // 将处理好的读完成事件放入请求队列中
          pool->append(users + sockfd);

          // 该连接活跃，更新定时器过期时间
          if (timer) {
            time_t curr = time(NULL);
            timer->expire = curr + 2 * TIMESLOT;
            LOG_INFO("%s", "adjust timer once");
            Log::get_instance()->flush();
          }
        } else {
          // 读失败，断开连接
          timer->cb_func(&users_timer[sockfd]);
          if (timer) timer_heap.del_timer(timer);
        }
      } 

      // 处理客户连接上的写事件
      else if (events[i].events & EPOLLOUT) {
        auto curtimer = users_timer[sockfd].timer;
        if (users[sockfd].write()) {
          LOG_INFO("send data to the client(%s)",
                   inet_ntoa(users[sockfd].get_address()->sin_addr));
          Log::get_instance()->flush();

          // 成功写数据，是活跃节点，更新定时器到期时间
          if (curtimer) {
            time_t curr = time(NULL);
            curtimer->expire = curr + 2 * TIMESLOT;
            LOG_INFO("%s", "adjust timer once");
            Log::get_instance()->flush();
          }
        } else {
          // 写失败，断开连接
          curtimer->cb_func(&users_timer[sockfd]);
          if (curtimer) timer_heap.del_timer(curtimer);
        }
      }
    }



    // 如果有超时则执行超时处理函数
    if (timeout) { // 收到 SIGALRM 信号时 timeout 置 1
      timer_handler();  // 删除过期的定时器，并重新设置定时间隔
      timeout = false;  // 处理完，现在没有过期的定时器
    }
  }
  close(epollfd);
  close(listenfd);
  close(pipefd[1]);
  close(pipefd[0]);
  delete[] users;
  delete[] users_timer;
  delete pool;
  return 0;
}
