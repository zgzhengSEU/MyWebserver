#include <netinet/in.h>
#include <time.h>

#include <queue>
#include <vector>

#define BUFFER_SIZE 64

// 前向声明
class heap_timer;

// 客户数据，绑定 socket 和 定时器
struct client_data {
  sockaddr_in address;
  int sockfd;
  char buf[BUFFER_SIZE];
  heap_timer* timer;
};

// 定时器类
class heap_timer {
 public:
  heap_timer(int delay) { expire = time(nullptr) + delay; }

 public:
  time_t expire;                  // 定时器生效的绝对时间
  void (*cb_func)(client_data*);  // 定时器的回调函数
  client_data* user_data;         // 用户数据
};

// 函数对象，优先队列中的第三个参数，实现最小堆
struct cmp {
  bool operator()(const heap_timer* a, const heap_timer* b) {
    return a->expire > b->expire;
  }
};

// 时间堆
class time_heap {
 public:
  explicit time_heap();
  ~time_heap();

 public:
  void add_timer(heap_timer* timer);  // 添加定时器
  void del_timer(heap_timer* timer);  // 删除定时器
  heap_timer* Top();                  // 获得堆顶部的定时器
  void tick();                        // 心搏函数

 private:
  // 使用优先队列实现最小时间堆
  std::priority_queue<heap_timer*, std::vector<heap_timer*>, cmp> timer_pqueue;
};
