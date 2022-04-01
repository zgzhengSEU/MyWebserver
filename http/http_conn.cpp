#include "http_conn.h"

#include <fstream>
#include <map>

#include "../log/log.h"

#define connfdET  // ET非阻塞
//#define connfdLT      // 水平阻塞

#define listenfdET
//#define listenfdLT

// 定义 http 响应的一些状态信息
const char* ok_200_title = "OK";

const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax.\n";

const char* error_403_title = "Forbidden";
const char* error_403_form = "You don't have permission to get the file.\n";

const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found.\n";

const char* error_500_title = "Internal Error";
const char* error_500_form =
    "There was an unusual problem serving the request file.\n";

// root 文件夹的路径
const char* doc_root = "/home/admin/MyWebserver/root";

locker m_lock;

// 设置 fd 为非阻塞，返回旧属性
int setNonBlocking(int fd) {
  int old_option = fcntl(fd, F_GETFL);
  int new_option = old_option | O_NONBLOCK;
  fcntl(fd, F_SETFL, new_option);
  return old_option;
}

// 向内核事件表注册读事件， ET，EPOLLONESHOT 模式
void addfd(int epollfd, int fd, bool one_shot) {
  epoll_event event;
  event.data.fd = fd;

  // 设置 event 关注的事件
#ifdef connfdET
  event.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
#endif

#ifdef connfdLT
  event.events = EPOLLIN | EPOLLRDHUP;  // EPOLLRDHUP 表示对方关闭连接是事件
#endif

#ifdef listenfdET
  event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
#endif

#ifdef listenfdLT
  event.events = EPOLLIN | EPOLLRDHUP;
#endif

  if (one_shot) event.events |= EPOLLONESHOT;
  epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
  setNonBlocking(fd);
}

// 从内核事件表中删除 fd
void removefd(int epollfd, int fd) {
  epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
  close(fd);
}

// EPOLLONESHOT保证一个socket只被一个线程处理，线程处理完后需要重置EPOLLONESHOT事件
// 监听socket即listenfd不能注册该事件，否则server只能处理一个客户连接
void modfd(int epollfd, int fd, int ev) {
  epoll_event event;
  event.data.fd = fd;

#ifdef connfdET
  event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
#endif

#ifdef connfdLT
  event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
#endif

  epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

// 初始化 static 变量
int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

// 初始化连接，注册到内核事件表中，然后调用私有 init()
void http_conn::init(int sockfd, const sockaddr_in& addr) {
  m_sockfd = sockfd;
  m_address = addr;
  addfd(m_epollfd, sockfd, true);  // 注册到内核事件表
  ++m_user_count;
  init();
}

// 初始化新接受的连接
void http_conn::init() {
  bytes_to_send = 0;
  bytes_have_send = 0;
  m_check_state = CHECK_STATE_REQUESTLINE;  // 默认处理请求行
  m_linger = false;
  m_method = GET;
  m_url = 0;
  m_version = 0;
  m_content_len = 0;
  m_host = 0;
  m_start_line = 0;
  m_checked_idx = 0;
  m_read_idx = 0;
  m_write_idx = 0;
  cgi = 0;
  memset(m_read_buf, '\0', READ_BUFFER_SIZE);
  memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
  memset(m_real_file, '\0', FILENAME_LEN);
}

// 关闭连接
void http_conn::close_conn(bool real_close) {
  // 一个连接对应一个 m_sockfd
  if (real_close && m_sockfd != -1) {
    removefd(m_epollfd, m_sockfd);
    m_sockfd = -1;
    --m_user_count;  // 关闭一个连接时，将客户总量减一
  }
}

// 从状态机：分析当前行的内容
// 返回值表示读取状态：LINE_OK, LINE_BAD, LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line() {
  char temp;
  // m_checked_idx：当前正在分析的字符在区中的位置
  // 读缓冲区中已经读入的数据的最后一个字节的下一个位置
  // [m_checked_idx, m_read_idx):未读
  for (; m_checked_idx < m_read_idx; ++m_checked_idx) {
    // 完整行的标志：末尾 '\r\n'
    temp = m_read_buf[m_checked_idx];  // 当前字符

    // 如果当前字符是 '\r'，回车符，则可能读到完整行
    if (temp == '\r') {
      // 如果读到末尾,返回未读完
      if ((m_checked_idx + 1) == m_read_idx) return LINE_OPEN;
      // 如果下一个字符是'\n'，则说明完整
      else if (m_read_buf[m_checked_idx + 1] == '\n') {
        // 读取完整，替换 \r\n
        m_read_buf[m_checked_idx++] = '\0';
        m_read_buf[m_checked_idx++] = '\0';
        return LINE_OK;
      }
      return LINE_BAD;  // 除以上两种情况，则出现语法错误
    }
    // '\n' 也可能读到完整行
    else if (temp == '\n') {
      if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r') {
        m_read_buf[m_checked_idx - 1] = '\0';
        m_read_buf[m_checked_idx++] = '\0';
        return LINE_OK;
      }
      return LINE_BAD;
    }
  }
  return LINE_OPEN;
}

// read_once 读取请求报文，直到无数据可读或者对方关闭连接
bool http_conn::read_once() {
  if (m_read_idx >= READ_BUFFER_SIZE) return false;

  int bytes_read = 0;

#ifdef connfdLT
  // bytes_read 接收读缓冲区中下一个未读的数据
  bytes_read =
      recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
  m_read_idx += bytes_read;

  if (bytes_read <= 0) return false;

  return true;
#endif

#ifdef connfdET
  // ET 模式只会触发一次，所以要循环读取
  // ET 必须设置文件是非阻塞，因为读空 recv 阻塞的话会卡住，无法跳出 while
  // 非阻塞IO调用总是立即返回，如果事件没有立即发生，则返回-1并设置error
  // 事件未发生时，accept,send,recv：errno = EAGAIN / EWOULDBLOK
  // connect：errno = EINPROGRESS，这时需要通过error判断是真出错还是未发生
  while (true) {
    bytes_read = recv(m_sockfd, m_read_buf + m_read_idx,
                      READ_BUFFER_SIZE - m_read_idx, 0);

    if (bytes_read == -1) {  // 以下两个 errno 表示没有数据可读，可以退出
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      return false;  // 如果是其他 errno，则是发生了错误
    } else if (bytes_read == 0) {  // 客户端关闭连接
      return false;
    }

    m_read_idx += bytes_read;
  }
  return true;
#endif
}

// 解析 http 请求行，获得请求方法、url、http 版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char* text) {
  // strpbrk(const char* str1, const char *str2)
  // 检索 str1 中第一个匹配 str2 中字符的字符（匹配单个字符，不是str2的字符串）
  // 下面一段操作是跳过 method 开头，匹配请求的 url
  // GET /index.html HTTP/1.1
  m_url = strpbrk(text, " \t");
  if (!m_url) return BAD_REQUEST;
  *m_url++ = '\0';

  // strcasecmp,匹配两字符串是否相等，忽略大小写
  // 下面一段确定方法 method
  // GET\0/index.html HTTP/1.1
  char* method = text;
  if (strcasecmp(method, "GET") == 0)
    m_method = GET;
  else if (strcasecmp(method, "POST") == 0) {
    m_method = POST;
    cgi = 1;
  } else {
    return BAD_REQUEST;
  }

  // strspn(const char* str1, const char* str2)
  // 在str1中检索第一个不出现str2的下标，所以以下是跳过( \t)
  // 可能是 \t/index.html HTTP/1.1
  m_url += strspn(m_url, " \t");

  // m_url = /index.html HTTP/1.1
  // 跳过 url，匹配版本号
  m_version = strpbrk(m_url, " \t");
  // m_version = 空格HTTP/1.1
  if (!m_version) return BAD_REQUEST;
  *m_version++ = '\0';
  // m_version = HTTP/1.1
  m_version += strspn(m_version, " \t");
  if (strcasecmp(m_version, "HTTP/1.1") != 0) return BAD_REQUEST;

  // 解析包装请求的 url
  if (strncasecmp(m_url, "http://", 7) == 0) {
    m_url += 7;  // 跳过 “http://”
    // 返回 url 中第一次出现 / 的位置,匹配结果类似于"/index.html"
    m_url = strchr(m_url, '/');
  }

  if (strncasecmp(m_url, "https://", 8) == 0) {
    m_url += 8;
    m_url = strchr(m_url, '/');
  }

  if (!m_url || m_url[0] != '/') return BAD_REQUEST;

  // 如果 url = "/",则显示默认页面
  if (strlen(m_url) == 1) {
    strcat(m_url, "index.html");
  }

  m_check_state =
      CHECK_STATE_HEADER;  // request 解析完毕，状态转移至解析 header
  return NO_REQUEST;
}

// 解析 http 请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char* text) {
  if (text[0] == '\0') {  // 遇到空行，表示头部字段解析完毕
    // 如果有消息体，状态机转移至 CHECK_STATE_CONTENT
    if (m_content_len != 0) {
      m_check_state = CHECK_STATE_CONTENT;
      return NO_REQUEST;
    }
    return GET_REQUEST;
  }

  // Connection 头部
  // 这里用strncasecmp是因为比较两个字符串的前11个字符
  else if (strncasecmp(text, "Connection:", 11) == 0) {
    text += 11;
    text += strspn(text, " \t");
    if (strcasecmp(text, "keep-alive") == 0) m_linger = true;
  }
  // Content-length 头部
  else if (strncasecmp(text, "Content-length:", 15) == 0) {
    text += 15;
    text += strspn(text, " \t");
    m_content_len = atol(text);
  }
  // Host
  else if (strncasecmp(text, "Host:", 5) == 0) {
    text += 5;
    text += strspn(text, " \t");
    m_host = text;
  } else {
    LOG_INFO("unknown header:%s", text);
    Log::get_instance()->flush();
  }

  return NO_REQUEST;
}

// 解析 http 消息体
http_conn::HTTP_CODE http_conn::parse_content(char* text) {
  // 判断消息体是否被完整读入
  if (m_read_idx >= (m_content_len + m_checked_idx)) {
    text[m_content_len] = '\0';
    m_string = text;     // 存储消息内容
    return GET_REQUEST;  // http 请求解析完毕
  }
  return NO_REQUEST;
}

// 主状态机根据从状态机返回的状态，执行对应的函数
http_conn::HTTP_CODE http_conn::process_read() {
  LINE_STATUS line_status = LINE_OK;  // 记录当前行的读取状态
  HTTP_CODE ret = NO_REQUEST;         // 记录 HTTP 请求的处理结果
  char* text = 0;

  // 当读到完整的行(LINE_OK)时
  while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) ||
         ((line_status = parse_line()) == LINE_OK)) {
    text = get_line();  // m_read_buf + m_start_line;
    m_start_line = m_checked_idx;
    LOG_INFO("%s", text);
    Log::get_instance()->flush();

    switch (m_check_state) {
      // 从状态机改变 主状态机 m_check_state 的状态，
      // 驱动主状态机执行对应的函数（处理request/header/content）
      case CHECK_STATE_REQUESTLINE: {  // 主状态机第一个状态，分析请求行
        ret = parse_request_line(text);
        if (ret == BAD_REQUEST) return BAD_REQUEST;
        break;
      }
      case CHECK_STATE_HEADER: {  // 主状态机第二个状态，分析头部字段
        ret = parse_headers(text);
        if (ret == BAD_REQUEST)
          return BAD_REQUEST;
        else if (ret == GET_REQUEST)
          return do_request();
        break;
      }
      case CHECK_STATE_CONTENT: {  // 主状态机第三个状态，分析请求体
        ret = parse_content(text);
        if (ret == GET_REQUEST) return do_request();
        line_status = LINE_OPEN;
        break;
      }
      default:
        return INTERNAL_ERROR;
    }
  }

  return NO_REQUEST;
}

// 当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性，
// 如果目标文件存在、对所有用户可读，且不是目录，则使用mmap将其
// 映射到内存地址 m_file_address 处，并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request() {
  // /home/admin/MyWebserver/root
  strcpy(m_real_file, doc_root);
  int len = strlen(doc_root);
  // strrchr 指向 m_url 中 '/' 最后一次出现的位置
  const char* p = strrchr(m_url, '/');

  strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

  // 以下获取 m_real_file 的属性
  // stat(fileName, buf)，将fileName的文件状态复制到buf中，成功返回 0,失败-1
  if (stat(m_real_file, &m_file_stat) < 0) return NO_RESOURCE;

  // st_mode:文件的类型和存取权限
  if (!(m_file_stat.st_mode & S_IROTH))  // S_IROTH: Read by others
    return FORBIDDEN_REQUEST;
  if (S_ISDIR(m_file_stat.st_mode))  // 如果是目录
    return BAD_REQUEST;

  int fd = open(m_real_file, O_RDONLY);
  // 将文件映射到进程地址空间，实现不同进程共享该文件，只需要调用该
  // m_file_address 指针即可 void *mmap (void *__addr, size_t __len, int __prot,
  // int __flags, int __fd, __off_t __offset)
  m_file_address =
      (char*)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  return FILE_REQUEST;
}

void http_conn::unmap() {
  if (m_file_address) {
    // 解除映射
    munmap(m_file_address, m_file_stat.st_size);
    m_file_address = 0;
  }
}

// 将响应报文发送给客户端
bool http_conn::write() {
  // 发送的数据在 m_iv 数组中，m_iv[0]是头部信息，[1]是文件内容
  int temp = 0;
  int newadd = 0;

  // 如果发送的数据为0，这一次响应结束。
  if (bytes_to_send == 0) {
    modfd(m_epollfd, m_sockfd, EPOLLIN);
    init();
    return true;
  }

  while (1) {
    // writev用于一次函数调用中写多个非连续的缓冲区，分散写
    // 返回已写字节数
    temp = writev(m_sockfd, m_iv, m_iv_count);

    // 正常发送
    if (temp >= 0) {
      bytes_have_send += temp;                 // 更新已发送字节数
      newadd = bytes_have_send - m_write_idx;  // 偏移文件iovec的指针
    } else {
      // 判断是否是缓冲区已满
      if (errno == EAGAIN) {
        // 第一个iovec头部信息发送完，发送第二个iovec
        if (bytes_have_send >= m_iv[0].iov_len) {
          m_iv[0].iov_len = 0;  // 不再发[0]
          m_iv[1].iov_base = m_file_address + newadd;
          m_iv[1].iov_len = bytes_to_send;
        }
        // 继续发送第一个iovec头部信息的数据
        else {
          m_iv[0].iov_base = m_write_buf + bytes_have_send;
          m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }
        // 重新注册写事件
        modfd(m_epollfd, m_sockfd, EPOLLOUT);
        return true;
      }
      //如果发送失败，但不是缓冲区问题，取消映射
      unmap();
      return false;
    }

    bytes_to_send -= temp;

    // 如果数据发送完毕
    if (bytes_to_send <= 0) {
      unmap();
      modfd(m_epollfd, m_sockfd, EPOLLIN);
      if (m_linger) {
        init();  // 保持连接，不关闭，重新初始化 http 对象
        return true;
      } else
        return false;
    }
  }
}

// 响应报文的填写，往写缓冲中写入待发送的数据，更新m_write_idx
bool http_conn::add_response(const char* format, ...) {
  if (m_write_idx >= WRITE_BUFFER_SIZE) return false;

  va_list arg_list;
  va_start(arg_list, format);
  // 将可变参数写入写缓冲区，返回写入数据的长度
  int len = vsnprintf(m_write_buf + m_write_idx,
                      WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
  // 如果写入的数据超过缓冲区的剩余空间，报错
  if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx)) {
    va_end(arg_list);
    return false;
  }
  m_write_idx += len;  // 更新 m_write_idx 的位置
  va_end(arg_list);
  LOG_INFO("request:%s", m_write_buf);
  Log::get_instance()->flush();
  return true;
}

// 响应报文的状态行：
// HTTP/1.1 200 OK
bool http_conn::add_status_line(int status, const char* title) {
  return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

// 响应报文的消息报头，包括内容长度、是否保持连接、添加空行
bool http_conn::add_headers(int content_length) {
  add_content_length(content_length);  // 报文的长度
  add_linger();                        // 是否保持连接
  add_blank_line();                    // 添加空行
}

bool http_conn::add_content_length(int content_length) {
  return add_response("Content-Length:%d\r\n", content_length);
}

bool http_conn::add_content_type() {
  return add_response("Content-Type:%s\r\n", "text/html");
}

bool http_conn::add_linger() {
  return add_response("Connection:%s\r\n",
                      (m_linger == true) ? "keep-alive" : "close");
}

bool http_conn::add_blank_line() { return add_response("%s", "\r\n"); }

// 添加文本 content
bool http_conn::add_content(const char* content) {
  return add_response("%s", content);
}

// 根据 do_request 的返回状态，子线程调用 process_write向m_write_buf写入响应报文
// 根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret) {
  switch (ret) {
    case INTERNAL_ERROR: {
      add_status_line(500, error_500_title);
      add_headers(strlen(error_500_form));
      if (!add_content(error_500_form)) return false;
      break;
    }
    case BAD_REQUEST: {
      add_status_line(404, error_404_title);
      add_headers(strlen(error_404_form));
      if (!add_content(error_404_form)) return false;
      break;
    }
    case FORBIDDEN_REQUEST: {
      add_status_line(403, error_403_title);
      add_headers(strlen(error_403_form));
      if (!add_content(error_403_form)) return false;
      break;
    }
    case FILE_REQUEST: {
      add_status_line(200, ok_200_title);
      if (m_file_stat.st_size != 0) {
        add_headers(m_file_stat.st_size);
        // 第一个iovec指针指向响应报文缓冲区，长度为m_write_idx
        m_iv[0].iov_base = m_write_buf;
        m_iv[0].iov_len = m_write_idx;
        // 第二个iovec指向mmap返回的文件指针，长度为文件大小
        m_iv[1].iov_base = m_file_address;
        m_iv[1].iov_len = m_file_stat.st_size;
        m_iv_count = 2;
        // 发送的所有数据为响应头部和文件大小
        bytes_to_send = m_write_idx + m_file_stat.st_size;
        return true;
      } else {
        // 如果请求资源为空，则返回一个空 html
        const char* ok_string = "<html><body></body></html>";
        add_headers(strlen(ok_string));
        if (!add_content(ok_string)) return false;
      }
    }
    default:
      return false;
  }
  // 除了 FILE_REQUEST外，其余状态只申请一个iovec，指向响应报文缓冲区
  m_iv[0].iov_base = m_write_buf;
  m_iv[0].iov_len = m_write_idx;
  m_iv_count = 1;
  bytes_to_send = m_write_idx;
  return true;
}

// 处理 http 请求的入口函数
void http_conn::process() {
  // 进来首先解析请求报文,保存返回的状态
  HTTP_CODE read_ret = process_read();
  // 如果还没读取完，则继续读取
  if (read_ret == NO_REQUEST) {
    modfd(m_epollfd, m_sockfd, EPOLLIN);
    return;
  }
  // 根据解析后的状态填写响应报文
  bool write_ret = process_write(read_ret);
  if (!write_ret) close_conn();
  // 编写好响应报文后，注册 EPOLLOUT，主线程检测到写就绪事件，调用
  // http_conn::write 将报文发给客户端
  modfd(m_epollfd, m_sockfd, EPOLLOUT);
}
