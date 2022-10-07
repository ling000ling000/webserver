#include "http_conn.h"

int http_conn::m_epollfd = -1; // 所有的socket上的事件都被注册到同一个epoll事件中
int http_conn::m_user_count = 0; // 统计用户的数量

// 定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

// 网站的根目录
const char* doc_root = "/home/acs/webserver/resources";


// 设置文件描述符非阻塞
void setnonblocking(int fd) {
    int old_flag = fcntl(fd, F_GETFL);
    int new_flag = old_flag | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_flag);
}

// 向epoll添加需要监听的文件描述符
void addfd(int epollfd, int fd, bool one_shot) {
    epoll_event event;
    event.data.fd = fd;
    // event.events = EPOLLIN | EPOLLRDHUP; // 水平触发;EPOLLRDHUP是内核参数
    event.events = EPOLLIN | EPOLLRDHUP; // 边缘触发    

    // one_shot使一个socket连接在任一时刻都只被一个线程处理
    if(one_shot) {
        event.events | EPOLLONESHOT; // 防止同一个通信被不同的线程处理
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);

    // 设置文件描述符非阻塞
    setnonblocking(fd);
    
}

// 从epoll中删除监听文件描述符
void removefd(int epollfd, int fd) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// 修改文件描述符
// 重置socket上的EPOLLONESHOT事件，以确保下一次可读时，EPOLLPIN事件能被触发
void modfd(int epollfd, int fd, int ev) {
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

// 初始化连接
void http_conn::init(int sockfd, const sockaddr_in &addr) {
    m_sockfd = sockfd;
    m_address = addr;

    // 端口复用
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 添加到epoll对象中
    addfd(m_epollfd, sockfd, true);
    m_user_count ++; // 总用户数+1

    init();

}

// (函数重载)
void http_conn::init() {
    m_check_state = CHECK_STATE_REQUESTLINE; // 初始化状态为解析请求首行
    m_checked_index = 0;
    m_start_line = 0;
    m_read_index = 0;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_linger = false;

    bzero(m_read_buff, READ_BUFFER_SIZE); // 全部置为0
}

// 关闭连接
void http_conn::close_conn() {
    if(m_sockfd != -1) {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count --; // 用户数-1
    }
}

// 循环读取客户数据，直到无数据可读或对方关闭连接
bool http_conn::read() {
    // 缓冲已满
    if(m_read_index >= READ_BUFFER_SIZE) {
        return false;
    }

    // 已经读取到的字节
    int byte_read = 0;
    while(1) {
        byte_read = recv(m_sockfd, m_read_buff + m_read_index, READ_BUFFER_SIZE - m_read_index, 0);
        if(byte_read == -1) {
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                // 没有数据
                break;
            }
            return false;
        } else if(byte_read == 0) {
            // 对方关连接
            return false;
        }
        m_read_index += byte_read; // 因为已经读入了新数据，要更新索引
    }
    printf("读取到了数据：%s\n", m_read_buff);
    return true;
}

// 主状态机，解析请求
http_conn::HTTP_CODE http_conn::process_read() {
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;

    char *text = 0;

    // 解析到了一行完整的数据或者解析到了请求体，也是完成的数据
    while((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || (line_status = parse_line()) == LINE_OK) {
        // 获取一行数据
        text = get_line();
        m_start_line = m_checked_index;
        printf("got 1 http line: %s\n", text);

        switch(m_check_state) {
            case CHECK_STATE_REQUESTLINE:
            {
                ret = parse_request_line(text);
                if(ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER:
            {
                ret = parse_headers(text);
                if(ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                } else if(ret == GET_REQUEST) {
                    return do_request(); // 解析具体的内容
                }
            }
            case CHECK_STATE_CONTENT:
            {
                ret = parse_content(text);
                if(ret == GET_REQUEST) {
                    return do_request();
                }
                line_status = LINE_OPEN;
                break;
            }
            default:
            {
                return INTERNAL_ERROR;
            }
        }
    }
    return NO_REQUEST;
}

// 解析http请求行，获取请求方法、目标url、http版本
http_conn::HTTP_CODE http_conn::parse_request_line(char *text) {
    // GET /index.html HTTP/1.1
    m_url = strpbrk(text, " \t");
    // GET\0/index.html HTTP/1.1
    *m_url ++ = '\0';

    char *method = text; // 因为GET后是\0，所以method指针指向的是“GET”这段字符串
    if(strcasecmp(method, "GET") == 0) { // 判断状态是否为GET
        m_method = GET;
    } else {
        return BAD_REQUEST;
    }

    //  /index.html HTTP/1.1
    m_version = strpbrk(m_url, " \t");
    if(!m_version) {
        return BAD_REQUEST;
    }
    *m_version ++ = '\0'; //  /index.html\0HTTP/1.1
    if(strcasecmp(m_version, "HTTP/1.1") != 0) {
        return BAD_REQUEST;
    }

    // http://xxx/index.html
    if(strncasecmp(m_url, "http://", 7) == 0) {
        m_url += 7; // xxx/index.html
        m_url = strchr(m_url, '/'); // 找第一次出现/的索引
    }

    if(!m_url || m_url[0] != '/') {
        return BAD_REQUEST;
    }

    m_check_state = CHECK_STATE_HEADER; // 主状态机状态变成检查请求头‘
    return NO_REQUEST;
}

// 解析http请求一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text) {
    // 遇到空行，表示头部字段解析完毕
    if(text[0] == '\0') {
        // 如果http请求有消息体，则还需要读取m_content_length字节的消息体
        // 状态机转移到CHECK_STATE_CONTENT状态
        if(m_content_length != 0) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 否则说明已经得到一个完整的http请求
        return GET_REQUEST;
    } else if (strncasecmp(text, "Connection:", 11) == 0) {
        // 处理Connection头部字段
        text += 11;
        text += strspn(text, " \t");
        if(strcasecmp(text, "keep-alive") == 0) {
            m_linger = true;
        }
    } else if(strncasecmp(text, "Content-Length:", 15) == 0) {
        // 处理content-length头部字段
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    } else if(strncasecmp(text, "Host:", 5) == 0) {
        // 处理host头部字段
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    } else {
        printf("opp! unkonwn header %s\n", text);
    }
    return NO_REQUEST;
}

// 没有真正解析http请求的消息体，只是判断是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text) {
    if(m_read_index >= (m_content_length + m_checked_index)) {
        text[m_content_length] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

// 解析一行，判断依据\r和\n
http_conn::LINE_STATUS http_conn::parse_line() {
    char temp;

    for(; m_checked_index < m_read_index; m_checked_index ++) {
        temp = m_read_buff[m_checked_index];
        if(temp == '\r') {
            if(m_checked_index + 1 == m_read_index) { // 检查的位置+1刚好是读取到的位置
                return LINE_OPEN;
            } else if(m_read_buff[m_checked_index + 1] == '\n') {
                m_read_buff[m_checked_index ++] = '\0';
                m_read_buff[m_checked_index ++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        } else if(temp == '\n') {
            if(m_checked_index > 1 && m_read_buff[m_checked_index - 1] == '\r') { // 检查\n前面是不是\r
                m_read_buff[m_checked_index - 1] = '\0';
                m_read_buff[m_checked_index ++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

// 得到一个完整正确的http请求时，分析目标文件的属性
// 如果目标文件存在且对所有用户可读，且不是目录，使用mmap映射到内存地址m_file_address处，并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request() {
    // /home/webserver/resources
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    // 获取mrealfile文件的状态信息，-1失败0成功
    if(stat(m_real_file, &m_file_stat) < 0) {
        return NO_REQUEST;
    }

    // 判断访问权限
    if(!(m_file_stat.st_mode & S_IROTH)) {
        return FORBIDDEN_REQUEST;
    }

    // 判断是否是目录
    if(S_ISDIR(m_file_stat.st_mode)) {
        return BAD_REQUEST;
    }

    // 以只读方式打开文件
    int fd = open(m_real_file, O_RDONLY);
    // 创建内存映射
    m_file_address = (char*)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}

// 对内存映射区执行munmao操作
void http_conn::unmap() {
    if(m_file_address) {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

// 写http响应
bool http_conn::write() {
    int temp = 0;

    if(bytes_to_send == 0) {
        // 将要发送的字节是0，这一次响应结束
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }

    while(1) {
        // writev表示分散写
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if(temp <= -1) {
            // 如果tcp写缓冲没有空间，等待下一轮epollin事件
            // 此期间服务器无法立即接收到同一客户的下一请求，但保证连接的完整性
            if(errno == EAGAIN) {
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;

        if(bytes_have_send >= m_iv[0].iov_len) {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_index);
            m_iv[1].iov_len = bytes_to_send;
        } else {
            m_iv[0].iov_base = m_write_buff + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - temp;
        }

        if(bytes_to_send <= 0) {
            // 没有数据要发送
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);

            if(m_linger) {
                init();
                return true;
            } else {
                return false;
            }
        }
    }
}

// 往写缓存中写入待发送的数据
bool http_conn::add_response(const char* format, ...) {
    if(m_write_index >= WRITE_BUFFER_SIZE) {
        return false;
    }
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buff + m_write_index, WRITE_BUFFER_SIZE - 1 - m_write_index, format, arg_list);
    if(len >= (WRITE_BUFFER_SIZE - 1 - m_write_index)) { // 写不下了
        return false;
    }
    m_write_index += len;
    va_end(arg_list);
    return true;
}

bool http_conn::add_status_line(int status, const char* title) {
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

bool http_conn::add_headers(int content_len) {
    add_content_length(content_len);
    add_content_type();
    add_linger();
    add_blank_line();

    return true;
}

bool http_conn::add_content(const char* content) {
    return add_response("%s", content);
}

bool http_conn::add_content_length(int content_len) {
    return add_response( "Content-Length: %d\r\n", content_len );
}

bool http_conn::add_linger() {
    return add_response("Connection: %s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

bool http_conn::add_content_type() {
    return add_response("Content-Type:%s\r\n", "text/html");
}

bool http_conn::add_blank_line() {
    return add_response("%s", "\r\n");
}

// 根据服务器处理http请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret) {
    switch(ret) {
        case INTERNAL_ERROR:
            add_status_line(500, error_500_title);
            add_headers(strlen(error_500_form));
            if(!http_conn::add_content(error_500_form)) {
                return false;
            }
            break;
        case BAD_REQUEST:
            add_status_line(400, error_400_title);
            add_headers(strlen(error_400_form));
            if(!add_content(error_400_form)) {
                return false;
            }
            break;
        case NO_RESOURCE:
            add_status_line(404, error_404_title);
            add_headers(strlen(error_404_form));
            if(!http_conn::add_content(error_404_form)) {
                return false;
            }
            break;
        case FORBIDDEN_REQUEST:
            add_status_line(403, error_403_title);
            add_headers(strlen(error_403_form));
            if(!add_content(error_403_form)) {
                return false;
            }
            break;
        case FILE_REQUEST:
            add_status_line(200, ok_200_title);
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buff;
            m_iv[0].iov_len = m_write_index;
            m_iv[0].iov_base = m_file_address;
            m_iv[0].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            bytes_to_send = m_write_index + m_file_stat.st_size;
            return true;
        default:
            return false;
    }
    m_iv[0].iov_base = m_write_buff;
    m_iv[0].iov_len = m_write_index;
    m_iv_count = 1;
    bytes_to_send = m_write_index;
    return true;
}

// 由线程池中的工作线性调用，这是处理http请求的入口函数
void http_conn::process() {
    // 解析http请求
    HTTP_CODE read_ret = process_read();
    if(read_ret == NO_REQUEST) {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }
    
    
    // 生成响应
    bool write_ret = process_write(read_ret);
    if(!write_ret) {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT);
}