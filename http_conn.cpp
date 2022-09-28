#include "http_conn.h"

int http_conn::m_epollfd = -1; // 所有的socket上的事件都被注册到同一个epoll事件中
int http_conn::m_user_count = 0; // 统计用户的数量


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
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP; // 边缘触发    

    // one_shot使一个socket连接在任一时刻都只被一个线程处理
    if(one_shot) {
        event.events | EPOLLONESHOT;
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

//
bool http_conn::write() {
    printf("一次性写完数据\n");
    return true;
}

// 由线程池中的工作线性调用，这是处理http请求的入口函数
void http_conn::process() {
    // 解析http请求
    process_read();
    
    printf("parse request, create response\n");
    
    // 生成响应

}