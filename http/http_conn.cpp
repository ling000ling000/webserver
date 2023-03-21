#include "http_conn.h"

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

// 该函数将传入的文件描述符 fd 设置为非阻塞模式
// 在非阻塞模式下，当数据不可用时，读取操作会立即返回0，写入操作会立即返回-1，并将 errno 设置为 EAGAIN 或 EWOULDBLOCK（表示暂时无法完成的操作）
// 这可以避免程序在等待数据或缓冲区可用时被阻塞，从而提高程序的并发性能
int setnonblocking(int fd) 
{
    int old_option = fcntl(fd, F_GETFL); // 获取 fd 的旧文件描述符选项
    int new_option = old_option | O_NONBLOCK; // 计算新的文件描述符选项
    fcntl(fd, F_SETFL, new_option); // 使用 fcntl 函数将 fd 的选项设置为新的文件描述符选项
    return old_option;
}

// 向epoll中添加需要监听的文件描述符
// 使得程序能够通过 epoll_wait 函数等待和监控该文件描述符上的事件
void addfd(int epollfd, int fd, bool one_shot) 
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLRDHUP; // 将事件类型设置为 EPOLLIN（表示可读事件）和 EPOLLRDHUP（表示对端关闭连接）
    // 参数 one_shot 主要用于控制是否启用 EPOLLONESHOT 选项。
    // 如果启用该选项，则在监控到一次该文件描述符上的事件后，该文件描述符将自动从 epoll 实例中删除。
    // 这可以保证 不同的线程 不会同时处理 同一个文件描述符上的事件。
    if(one_shot) 
    {
        event.events |= EPOLLONESHOT; // 防止同一个通信被不同的线程处理
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event); // 向 epoll 实例中添加文件描述符 fd 和对应的事件 event
    setnonblocking(fd); // 设置文件描述符非阻塞
}

// 从epoll中移除监听的文件描述符
void removefd(int epollfd, int fd) 
{
    // 将操作类型设置为 EPOLL_CTL_DEL 表示删除该文件描述符
    // 第四个参数为 0，表示不需要传递事件结构体
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// 修改文件描述符，重置socket上的EPOLLONESHOT事件，以确保下一次可读时，EPOLLIN事件能被触发
void modfd(int epollfd, int fd, int ev) 
{
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

// 所有的客户数
int http_conn::m_user_count = 0;
// 所有socket上的事件都被注册到同一个epoll内核事件中，所以设置成静态的
int http_conn::m_epollfd = -1;

// 关闭连接
void http_conn::close_conn() 
{
    if(m_sockfd != -1) {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--; // 关闭一个连接，将客户总数量-1
    }
}

// 初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in& addr) 
{
    m_sockfd = sockfd;
    m_address = addr;
    
    // 设置套接字选项 SO_REUSEADDR，以启用端口复用。
    // 这意味着如果之前绑定到该端口的套接字处于 TIME_WAIT 状态，该端口可以立即重新使用。
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    addfd(m_epollfd, sockfd, true); // 将套接字文件描述符 sockfd 添加到 epoll 实例中，以监听该套接字上的事件。
    m_user_count ++;
    init();
}

// 重置 HTTP 连接对象的各个成员变量，以便在处理下一个客户端请求时可以使用一个干净的状态。
// 通过清空缓冲区和文件名等数据，可以避免之前处理的数据对当前请求的处理产生干扰。
void http_conn::init()
{

    bytes_to_send = 0;
    bytes_have_send = 0;

    m_check_state = CHECK_STATE_REQUESTLINE; // 初始状态为检查请求行
    m_linger = false; // 默认不保持链接  Connection : keep-alive保持连接

    m_method = GET; // 默认请求方式为GET
    m_url = 0;              
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;

    // 全部设置为 0 或空字符串，以清空缓冲区和文件名。
    bzero(m_read_buf, READ_BUFFER_SIZE);
    bzero(m_write_buf, READ_BUFFER_SIZE);
    bzero(m_real_file, FILENAME_LEN);
}


// 循环读取客户数据，并将其存储到 HTTP 连接对象的读缓冲区中
bool http_conn::read() 
{
    if(m_read_idx >= READ_BUFFER_SIZE) 
    {
        return false;
    }

    int bytes_read = 0;
    while(true) 
    {
        // 从m_read_buf + m_read_idx索引出开始保存数据，大小是READ_BUFFER_SIZE - m_read_idx
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0 );
        if (bytes_read == -1) //  -1 则表示读取出错，需要根据具体的错误码进行处理
        {
            // 如果是 EAGAIN 或 EWOULDBLOCK 错误，则表示当前没有数据可读，可以退出循环等待下一次读取。
            if(errno == EAGAIN || errno == EWOULDBLOCK) 
            {
                break; // 没有数据
            }
            return false;   
        } 
        else if (bytes_read == 0) // 表示对方已经关闭了连接，需要退出读取循环并返回 false。
        {
            return false;
        }
        m_read_idx += bytes_read;
    }
    return true;
}

// 解析HTTP请求报文中的每一行数据。
// 在一个 while 循环中执行的，每次循环会取出 m_read_buf 中的一个字符进行解析，直到读取完所有的字符为止。判断依据\r\n
http_conn::LINE_STATUS http_conn::parse_line() 
{
    char temp;
    for ( ; m_checked_idx < m_read_idx; ++ m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r') 
        {
            if ((m_checked_idx + 1 ) == m_read_idx) 
            {
                return LINE_OPEN; // 未完整读取当前行，需要继续读取。
            } 
            else if (m_read_buf[ m_checked_idx + 1] == '\n') 
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK; // 解析成功，当前行是一个完整的行，包括 \r\n 结束符。
            }
            return LINE_BAD; // 解析失败，当前行有语法错误或格式不正确。
        } 
        else if(temp == '\n')  
        {
            if((m_checked_idx > 1) && ( m_read_buf[m_checked_idx - 1] == '\r') ) 
            {
                m_read_buf[m_checked_idx-1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

// 解析HTTP请求行，获得请求方法，目标URL,以及HTTP版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char* text) 
{
    // 当前请求行：GET /index.html HTTP/1.1

    // 使用strpbrk函数在text字符串中查找第一个出现空格或制表符的位置，将其作为URL的起始位置
    //如果没有找到则返回错误状态码BAD_REQUEST。
    m_url = strpbrk(text, " \t"); 
    if (!m_url) 
    { 
        return BAD_REQUEST;
    }

    // 将找到的URL的起始位置处置为字符串结束符
    *m_url ++ = '\0'; // 此时的请求行：GET\0/index.html HTTP/1.1
    char* method = text;
    // 将请求方法设为GET，并检查是否合法
    if (strcasecmp(method, "GET") == 0) // 忽略大小写比较
    { 
        m_method = GET;
    } 
    else 
    {
        return BAD_REQUEST;
    }

    // /index.html HTTP/1.1
    // 使用strpbrk函数在URL字符串中查找第一个出现空格或制表符的位置，将其作为版本号的起始位置。
    // 如果没有找到则返回错误状态码BAD_REQUEST。
    m_version = strpbrk(m_url, " \t");
    if (!m_version) 
    {
        return BAD_REQUEST;
    }

    //将版本号的起始位置处置为字符串结束符，并检查版本号是否为HTTP/1.1。
    // 如果不是则返回错误状态码BAD_REQUEST。
    *m_version ++ = '\0'; // 此时的请求行：/index.html\0HTTP/1.1
    if (strcasecmp(m_version, "HTTP/1.1") != 0) 
    {
        return BAD_REQUEST;
    }
    /**
     * http://192.168.110.129:10000/index.html
    */
    // 检查URL是否包含"//"，如果包含则跳过"http://"字符串
    if (strncasecmp(m_url, "http://", 7) == 0) 
    {   
        m_url += 7;
        // 在参数 str 所指向的字符串中搜索第一次出现字符 c（一个无符号字符）的位置。
        m_url = strchr(m_url, '/');
    }
    // 接着查找"/"字符的位置，并将其设置为URL字符串的起始位置
    if (!m_url || m_url[0] != '/') 
    {
        return BAD_REQUEST;
    }
    m_check_state = CHECK_STATE_HEADER; // 检查状态变成检查头
    return NO_REQUEST;
}

// 解析HTTP请求的一个头部信息
// 包含了一些常见的头部字段，如Connection、Content-Length和Host。
// 该函数接受一个指向文本的指针，逐行解析文本，并根据不同的头部字段进行处理。
http_conn::HTTP_CODE http_conn::parse_headers(char* text) 
{   
    // 遇到空行，表示头部字段解析完毕
    if(text[0] == '\0') 
    {
        // 如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体，
        // 状态机转移到CHECK_STATE_CONTENT状态
        if (m_content_length != 0) 
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 否则说明我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;
    } 
    else if (strncasecmp(text, "Connection:", 11 ) == 0) 
    {
        // 处理 Connection 头部字段
        // 如果值为keep-alive，则设置m_linger为true，表示需要保持连接。
        text += 11;
        text += strspn(text, " \t");
        if ( strcasecmp(text, "keep-alive") == 0 ) 
        {
            m_linger = true;
        }
    } 
    else if (strncasecmp(text, "Content-Length:", 15 ) == 0) 
    {
        // 处理Content-Length头部字段
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text); // 将其值转换成整型并存储在m_content_length中，以便后续读取消息体。
    } 
    else if (strncasecmp(text, "Host:", 5) == 0) 
    {
        // 处理Host头部字段
        text += 5;
        text += strspn(text, " \t");
        m_host = text; // 将其值存储在m_host中，以便后续处理
    } 
    else 
    {
        printf("oop! unknow header %s\n", text);
    }
    return NO_REQUEST;
}

// 我们没有真正解析HTTP请求的消息体，只是判断它是否被完整的读入了
http_conn::HTTP_CODE http_conn::parse_content(char* text) 
{
    if (m_read_idx >= (m_content_length + m_checked_idx) )
    {
        text[m_content_length] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

// 主状态机，解析请求
http_conn::HTTP_CODE http_conn::process_read() 
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char* text = 0;
    while (((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK)) || ((line_status = parse_line()) == LINE_OK)) 
    {
        // 获取一行数据
        text = get_line();
        m_start_line = m_checked_idx;
        printf("got 1 http line: %s\n", text);

        // 根据 m_check_state 变量的值，分别进行不同的处理
        switch(m_check_state) 
        {
            case CHECK_STATE_REQUESTLINE: // 解析请求行
            {
                ret = parse_request_line(text);
                if (ret == BAD_REQUEST) 
                {
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER: // 解析请求头部
            {
                ret = parse_headers( text );
                if (ret == BAD_REQUEST) 
                {
                    return BAD_REQUEST;
                } 
                else if (ret == GET_REQUEST) 
                {
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT:  // 解析请求数据体
            {
                ret = parse_content(text);
                if (ret == GET_REQUEST) 
                {
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

// 当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性，
// 如果目标文件存在、对所有用户可读，且不是目录，则使用mmap将其
// 映射到内存地址m_file_address处，并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request()
{
    // "/home/nowcoder/webserver/resources"
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    // 获取m_real_file文件的相关的状态信息，-1失败，0成功
    if (stat( m_real_file, &m_file_stat ) < 0) 
    {
        return NO_RESOURCE;
    }

    // 判断访问权限
    if (!( m_file_stat.st_mode & S_IROTH)) 
    {
        return FORBIDDEN_REQUEST;
    }

    // 判断是否是目录
    if (S_ISDIR(m_file_stat.st_mode)) 
    {
        return BAD_REQUEST;
    }

    // 以只读方式打开文件
    int fd = open(m_real_file, O_RDONLY);
    // 创建内存映射
    m_file_address = (char*)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}

// 对内存映射区执行munmap操作
void http_conn::unmap() 
{
    if(m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

// 写HTTP响应
bool http_conn::write() 
{
    int temp = 0;
    
    if (bytes_to_send == 0) 
    {
        // 将要发送的字节为0，这一次响应结束。
        modfd(m_epollfd, m_sockfd, EPOLLIN); 
        init();
        return true;
    }

    while(1) 
    {
        // 分散写
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if (temp <= -1) 
        {
            // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，
            // 服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if(errno == EAGAIN) 
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;

        if (bytes_have_send >= m_iv[0].iov_len) 
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        } 
        else 
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - temp;
        }

        if (bytes_to_send <= 0) 
        {
            // 没有数据要发送了
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);

            if (m_linger) 
            {
                init();
                return true;
            } 
            else 
            {
                return false;
            }
        }

    }

    
}

// 往写缓冲中写入待发送的数据
bool http_conn::add_response(const char* format, ...) 
{
    if(m_write_idx >= WRITE_BUFFER_SIZE) 
    {
        return false;
    }
    
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if( len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx) ) 
    {
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);
    return true;
}

bool http_conn::add_status_line(int status, const char* title) 
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

bool http_conn::add_headers(int content_len) 
{
    add_content_length(content_len);
    add_content_type();
    add_linger();
    add_blank_line();
    return true;
}

bool http_conn::add_content_length(int content_len) 
{
    return add_response("Content-Length: %d\r\n", content_len);
}

bool http_conn::add_linger() 
{
    return add_response("Connection: %s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

bool http_conn::add_blank_line() 
{
    return add_response("%s", "\r\n");
}

bool http_conn::add_content(const char* content) 
{
    return add_response("%s", content);
}

bool http_conn::add_content_type() 
{
    return add_response("Content-Type:%s\r\n", "text/html");
}

// 根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret) 
{
    switch (ret)
    {
        case INTERNAL_ERROR:
            add_status_line(500, error_500_title);
            add_headers(strlen(error_500_form));
            if (!add_content(error_500_form)) 
            {
                return false;
            }
            break;
        case BAD_REQUEST:
            add_status_line(400, error_400_title);
            add_headers(strlen(error_400_form));
            if (!add_content( error_400_form )) 
            {
                return false;
            }
            break;
        case NO_RESOURCE:
            add_status_line(404, error_404_title);
            add_headers(strlen(error_404_form));
            if (!add_content( error_404_form)) 
            {
                return false;
            }
            break;
        case FORBIDDEN_REQUEST:
            add_status_line(403, error_403_title);
            add_headers(strlen(error_403_form));
            if (!add_content(error_403_form)) 
            {
                return false;
            }
            break;
        case FILE_REQUEST:
            add_status_line(200, ok_200_title );
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;

            bytes_to_send = m_write_idx + m_file_stat.st_size;

            return true;
        default:
            return false;
    }

    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

// 由线程池中的工作线程调用，这是处理HTTP请求的入口函数
void http_conn::process() 
{
    // 解析HTTP请求
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST) 
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }
    
    // 生成响应
    bool write_ret = process_write(read_ret);
    if (!write_ret) 
    {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT);
}
