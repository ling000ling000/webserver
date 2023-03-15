#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <pthread.h>
#include <list>
#include <exception>
#include <cstdio>

#include "../lock/locker.h"

// 线程池类，定义成模板类是为了代码复用
template<typename T>
class threadpool 
{
public:
    // thread_number是线程池中的线程数量
    // max_requests是请求队列中最多允许的、等待处理的请求的数量
    threadpool(int thread_number = 8, int max_requests = 10000);
    ~threadpool();

    // 往请求队列添加任务
    bool append(T* request);

private:
    // 工作线程运行的函数，它不断从工作队列中取出任务并执行
    static void* worker(void* arg);
    void run();

private:
    // 线程的数量
    int m_thread_number;

    // 描述线程池的数组，大小为线程的数量
    pthread_t *m_threads;

    // 请求队列中最多允许的、等待处理的请求数量
    int m_max_requests;

    // 请求队列
    std::list<T*> m_workqueue;

    // 保护请求队列的互斥锁
    locker m_queuelocker;

    // 信号量用来判断是否有任务要处理
    sem m_queuestat;

    // 是否结束线程
    bool m_stop;
};

template<typename T>
threadpool<T>::threadpool(int thread_number, int max_requests) :
            m_thread_number(thread_number), m_max_requests(max_requests),
            m_stop(false), m_threads(NULL) 
    {
        if ((thread_number <= 0) || (max_requests <= 0)) 
        {
            throw std::exception();
        }

        m_threads = new pthread_t[m_thread_number]; // 创建线程池数组
        if (!m_threads) 
        {
            throw std::exception();
        }

        // 创建thread_number个线程，并将它们设置为脱离线程
        for (int i = 0; i < thread_number; i ++) 
        {
            printf("create the %dth thread\n", i);

            if (pthread_create(m_threads + i, NULL, worker, this) != 0) 
            {
                delete [] m_threads;
                throw std::exception();
            }

            if (pthread_detach(m_threads[i])) 
            {
                delete [] m_threads;
                throw std::exception();
            }
        }

    }

template<typename T>
threadpool<T>::~threadpool() 
{
    delete [] m_threads;
    m_stop = true;
}

template<typename T>
bool threadpool<T>::append(T* request) 
{
    // 操作工作队列时一定要加锁，因为它被所有线程共享
    m_queuelocker.lock();
    if (m_workqueue.size() > m_max_requests) // 如果 工作队列的元素个数 大于 最大请求数，不能再往工作队列加元素了
    {
        m_queuelocker.unlock();
        return false;
    }

    m_workqueue.push_back(request); // 把任务入队
    m_queuelocker.unlock(); // 操作完工作队列后解锁
    m_queuestat.post(); 
    return true;
}

template<typename T>
void* threadpool<T>::worker(void* arg) // 接受一个 void 指针类型的参数 arg，这个参数实际上是一个指向 threadpool 类的指针
{
    threadpool* pool = (threadpool*) arg; // 将指针类型强制转换为 threadpool 类型
    pool -> run(); // 然后调用该对象的 run 函数
    return pool; // worker 函数将指向 threadpool 类的指针作为返回值返回，这个指针可以用来 在外部访问 线程池的成员变量和函数
}

template<typename T>
void threadpool<T>::run() // run 函数是在 worker 函数内被调用的，它是线程池的核心函数，负责从请求队列中取出任务并执行
{
    while (!m_stop) 
    {
        m_queuestat.wait(); // 通过信号量 m_queuestat 等待任务的到来
        m_queuelocker.lock(); // 操作队列要加锁
        if (m_workqueue.empty()) 
        {
            m_queuelocker.unlock();
            continue;
        }

        T* request = m_workqueue.front(); // 取出队头元素
        m_workqueue.pop_front();
        m_queuelocker.unlock(); // 解锁

        if (!request) 
        {
            continue;
        }

        request -> process(); // 执行实际的任务处理逻辑
    }
}


#endif