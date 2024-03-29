#include "reyao/hook.h"
#include "reyao/log.h"
#include "reyao/coroutine.h"
#include "reyao/fdmanager.h"
#include "reyao/singleton.h"
#include "reyao/worker.h"

#include <dlfcn.h>
#include <stdarg.h>
#include <sys/epoll.h>

#include <iostream>

#include <memory>
#include <functional>

namespace reyao {

static thread_local bool t_hookEnable = false;

static int64_t s_connectTimeout = 5000;

#define HOOK_FUNC(XX) \
    XX(sleep) \
    XX(usleep) \
    XX(nanosleep) \
    XX(socket) \
    XX(connect) \
    XX(accept) \
    XX(close) \
    XX(read) \
    XX(write) \
    XX(readv) \
    XX(writev) \
    XX(recv) \
    XX(recvfrom) \
    XX(recvmsg) \
    XX(recvmmsg) \
    XX(send) \
    XX(sendto) \
    XX(sendmsg) \
    XX(sendmmsg) \
    XX(fcntl) \
    XX(ioctl) \
    XX(getsockopt) \
    XX(setsockopt) \

void hook_init() {
    static bool is_inited = false;
    if (is_inited) {
        return;
    }
#define XX(name) name ## _origin = (name ## Func_t)dlsym(RTLD_NEXT, #name);
    HOOK_FUNC(XX);
#undef XX
}

struct HookIniter {
    HookIniter() {
        hook_init();
    }
};

static HookIniter s_hook_initer;

bool IsHookEnable() {
    return t_hookEnable;
}

void SetHookEnable(bool flag) {
    t_hookEnable = flag;
}

struct timerInfo {
    int cancelled = 0;
};

template <typename OriginFunc, typename ... Args>
static ssize_t do_io(int fd, OriginFunc func, const char* hookFuncName,
                     uint32_t type, int timeoutSo,  Args&& ... args) {
    
    if (!t_hookEnable) {
        return func(fd, std::forward<Args>(args)...);
    }
    // LOG_DEBUG << "in do_io " << "fd= " << fd << " hookFunc=" << hookFuncName;
    auto fdctx = g_fdmanager->getFdContext(fd);
    if (!fdctx) {
        return func(fd, std::forward<Args>(args)...);
    }
    
    if (fdctx->isClose()) {
        errno = EBADF;
        return -1;
    }

    // 用户将 sockfd 设置成非阻塞，则直接对非阻塞的 sockfd 进行非阻塞 io 操作
    // 如果 io 操作没准备好，则直接返回 EWOULDBLOCK，与原生函数一致
    if (!fdctx->isSocketFd() || fdctx->getUserNonBlock()) {
        return func(fd, std::forward<Args>(args)...);
    }

    // 获取 sockfd 设置的超时时间（通过 setsockopt 的 SO_RCVTIMEO 和 SO_SNDTIMEO）
    int64_t timeout = fdctx->getTimeout(timeoutSo);
    std::shared_ptr<timerInfo> tinfo(new timerInfo);

retry:  
    // 对非阻塞的 sockfd 调用一次 io 函数，如果没准备好（返回 EAGAIN）
    // 则放入 epoll 队列中等待，则设置定时器，超时时取消 epoll 事件并直接返回
    ssize_t n = func(fd, std::forward<Args>(args)...);
    while (n == -1 && errno == EINTR) {
        n = func(fd, std::forward<Args>(args)...);
    }
    if (n == -1 && errno == EAGAIN) {
        Timer::SPtr timer;
        std::weak_ptr<timerInfo> winfo(tinfo);

        // 设置定时器
        if (timeout != -1) {                                                                 
            timer = Worker::GetScheduler()->addConditonTimer(timeout, [winfo, fd, type]() {
                auto t = winfo.lock();
                // winfo 观察 tinfo 是否存在以判断 do_io 函数是否返回
                if (!t || t->cancelled) {
                    return;
                }
                t->cancelled = ETIMEDOUT;
                // 直接触发 IO 事件，继续执行当前协程
                // 发现 cancelled 标志为 ETIMEDOUT 时直接返回
                Worker::HandleEvent(fd, type);
            }, winfo);
        }

        // 将 sockfd 添加到线程的 epoll 队列中，
        // 并将当前协程放入 epoll 队列的 IOEvent 中，触发 IO 事件时则继续执行该协程
        // 如果添加失败，则取消记录超时的定时器并直接返回
        if (!Worker::AddEvent(fd, type)) {
            LOG_ERROR << hookFuncName << "addEvent("
                        << fd << ". " << type << ")";
            if (timer) {
                timer->cancel();
            }
            return -1;
        } else {
            Coroutine::YieldToSuspend();

            if (timer) {
                // 在超时时间内触发 IO 事件
                timer->cancel();
            }
            //该事件已超时，直接返回
            if (tinfo->cancelled) {
                errno = tinfo->cancelled;
                return -1;
            }
            goto retry;
        }
    }

    return n;
}
    
} // namespace reyao

extern "C" {

#define XX(name) name ## Func_t name ## _origin = nullptr;
    HOOK_FUNC(XX);
#undef XX


unsigned int sleep(unsigned int seconds) {
    if (!reyao::t_hookEnable) {
        return sleep_origin(seconds);
    }
    auto co = reyao::Coroutine::GetCurCoroutine();
    auto worker = reyao::Worker::GetWorker();
    worker->getScheduler()->addTimer(seconds * 1000, std::bind(
                                    (void(reyao::Worker::*)(reyao::Coroutine::SPtr coroutine))
                                    &reyao::Worker::addTask, worker, co)
                                    );
    reyao::Coroutine::YieldToSuspend();
    return 0;
}

int usleep(useconds_t usec) {
    if (!reyao::t_hookEnable) {
        return usleep_origin(usec);
    }
    auto co = reyao::Coroutine::GetCurCoroutine();
    auto worker = reyao::Worker::GetWorker();
    worker->getScheduler()->addTimer(usec / 1000, std::bind(
                                    (void(reyao::Worker::*)(reyao::Coroutine::SPtr coroutine))
                                    &reyao::Worker::addTask, worker, co)
                                    );
    reyao::Coroutine::YieldToSuspend();
    return 0;
}


int nanosleep(const struct timespec* req, struct timespec* rem) {
    if (!reyao::t_hookEnable) {
        return nanosleep_origin(req, rem);
    }
    int64_t timeout = req->tv_sec * 1000 + req->tv_nsec / 1000000;
    auto co = reyao::Coroutine::GetCurCoroutine();
    auto worker = reyao::Worker::GetWorker();
    worker->getScheduler()->addTimer(timeout, std::bind(
                                       (void(reyao::Worker::*)(reyao::Coroutine::SPtr coroutine))
                                       &reyao::Worker::addTask, worker, co)
                                       );
    reyao::Coroutine::YieldToSuspend();
    return 0;
}

int socket(int domain, int type, int protocol) {
    if (!reyao::t_hookEnable) {
        return socket_origin(domain, type, protocol);
    }
    int fd = socket_origin(domain, type, protocol);
    if (fd == -1) {
        return fd;
    }
    g_fdmanager->addFd(fd);
    return fd;
}

int connect_with_timeout(int sockfd, const struct sockaddr* addr,
            socklen_t addrlen, int64_t timeout) {
    if (!reyao::t_hookEnable) {
        return connect_origin(sockfd, addr, addrlen);
    }
    auto fdctx = g_fdmanager->getFdContext(sockfd);
    if (!fdctx || fdctx->isClose()) {
        errno = EBADF;
        return -1;
    }
    if (!fdctx->isSocketFd()) {
        return connect_origin(sockfd, addr, addrlen);
    }
    if (fdctx->getUserNonBlock()) {
        return connect_origin(sockfd, addr, addrlen);
    }
    
    int n = connect_origin(sockfd, addr, addrlen);
    if (n == 0) {
        return n;
    } else if (n != -1 || errno != EINPROGRESS) {
        return n;
    }

    auto worker = reyao::Worker::GetWorker();
    reyao::Timer::SPtr timer;
    std::shared_ptr<reyao::timerInfo> tinfo(new reyao::timerInfo);
    std::weak_ptr<reyao::timerInfo> winfo(tinfo);

    if (timeout != -1) {
        timer = worker->getScheduler()->addConditonTimer(timeout, [winfo, sockfd]() {
            auto tinfo = winfo.lock();
            if (!tinfo || tinfo->cancelled) {
                return;
            }
            tinfo->cancelled = ETIMEDOUT;
            reyao::Worker::HandleEvent(sockfd, EPOLLOUT);
        }, winfo);
    }

    bool rt = reyao::Worker::AddEvent(sockfd, EPOLLOUT);
    if (rt) {
        reyao::Coroutine::YieldToSuspend();
        if (timer) {
            timer->cancel();
        }
        if (tinfo->cancelled) {
            errno = tinfo->cancelled;
            return -1;
        }
    } else {
        //添加事件失败
        if (timer) {
            timer->cancel();
        }
        LOG_ERROR << "connect addEvent(" << sockfd << ", WRITE) errno=" << strerror(errno);
    }

    // 读取错误码并返回，与原生 connect 返回值保持一致
    int error = 0;
    socklen_t len = sizeof(int);
    if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &error, &len)) {
        return -1;
    } else if (!error) {
        return 0;
    } else {
        errno = error;
        return -1;
    }
}

int connect(int sockfd, const struct sockaddr* addr,
            socklen_t addrlen) {
    return connect_with_timeout(sockfd, addr, addrlen, reyao::s_connectTimeout);
}

int accept(int sockfd, struct sockaddr* addr, socklen_t* addrlen) {
    int fd = reyao::do_io(sockfd, accept_origin, "accept", EPOLLIN, 
                          SO_RCVTIMEO, addr, addrlen);
    if (fd >= 0) {
        g_fdmanager->addFd(fd);
    }
    return fd;
}

int close(int fd) {
    if (!reyao::t_hookEnable) {
        return close_origin(fd);
    }

    auto fdctx = g_fdmanager->getFdContext(fd);
    if (fdctx) {
        reyao::Worker::HandleAllEvent(fd);
        g_fdmanager->delFd(fd);
    }
    return close_origin(fd);
}

ssize_t read(int fd, void *buf, size_t count) {
    return reyao::do_io(fd, read_origin, "read", EPOLLIN,
                        SO_RCVTIMEO, buf, count);
}

ssize_t write(int fd, const void *buf, size_t count) {
    return reyao::do_io(fd, write_origin, "write", EPOLLOUT,
                        SO_SNDTIMEO, buf, count);
}

ssize_t readv(int fd, const struct iovec *iov, int iovcnt) {
    return reyao::do_io(fd, readv_origin, "readv", EPOLLIN,
                        SO_RCVTIMEO, iov, iovcnt);
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt) {
    return reyao::do_io(fd, writev_origin, "writev", EPOLLOUT,
                        SO_SNDTIMEO, iov, iovcnt);
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
    return reyao::do_io(sockfd, recv_origin, "recv", EPOLLIN,
                        SO_RCVTIMEO, buf, len, flags);
}

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen) {
    return reyao::do_io(sockfd, recvfrom_origin, "recvfrom", EPOLLIN,
                        SO_RCVTIMEO, buf, len, flags, src_addr, addrlen);
}

ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags) {
    return reyao::do_io(sockfd, recvmsg_origin, "recvmsg", EPOLLIN,
                        SO_RCVTIMEO, msg, flags);
}

int recvmmsg(int sockfd, struct mmsghdr *msgvec, 
             unsigned int vlen, int flags, 
             struct timespec *timeout) {
    return reyao::do_io(sockfd, recvmmsg_origin, "recvmmsg", EPOLLIN,
                        SO_RCVTIMEO, msgvec, vlen, flags, timeout);
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags) {
    return reyao::do_io(sockfd, send_origin, "send", EPOLLOUT,
                        SO_SNDTIMEO, buf, len, flags);
}

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen) {
    return reyao::do_io(sockfd, sendto_origin, "sendto", EPOLLOUT,
                        SO_SNDTIMEO, buf, len, flags, dest_addr, addrlen);
}

ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags) {
    return reyao::do_io(sockfd, sendmsg_origin, "sendmsg", EPOLLOUT,
                        SO_SNDTIMEO, msg, flags);
}

int sendmmsg(int sockfd, struct mmsghdr *msgvec,
		     unsigned int vlen, int flags) {
    return reyao::do_io(sockfd, sendmmsg_origin, "sendmmsg", EPOLLOUT,
                        SO_SNDTIMEO, msgvec, vlen, flags);
}

int fcntl(int fd, int cmd, ... /* arg */ ) {
    va_list va;
    va_start(va, cmd);
    switch(cmd) {
        case F_SETFL:
            {
                int arg = va_arg(va, int);
                va_end(va);
                auto fdctx = g_fdmanager->getFdContext(fd);
                if(!fdctx || fdctx->isClose() || !fdctx->isSocketFd()) {
                    return fcntl_origin(fd, cmd, arg);
                }
                // 只针对 sockfd

                // 用户设置 NONBLOCK
                fdctx->setUserNonBlock(arg & O_NONBLOCK);
                
                // 对于是否真正地对 fd 设置非阻塞标志，看的是 fdctx 中的 sysNonBlock 标志
                // sysNonBlock 在初始化 fdctx 时默认为 true
                if(fdctx->getSysNonBlock()) {
                    arg |= O_NONBLOCK;
                } else {
                    arg &= ~O_NONBLOCK;
                }
                return fcntl_origin(fd, cmd, arg);
            }
        break;
        case F_GETFL:
            {
                va_end(va);
                int arg = fcntl_origin(fd, cmd);
                auto fdctx = g_fdmanager->getFdContext(fd);
                if(!fdctx || fdctx->isClose() || !fdctx->isSocketFd()) {
                    return arg;
                }
                // 只针对 sockfd

                // 用户读写 sockfd 的 NonBlock 标志，其实只是将结果放在 fdctx 的 userNonBolck 中
                // 使得返回结果看起来与原生函数一致，实际是否阻塞看的是看的是 fdctx 中的 sysNonBlock 标志
                if(fdctx->getUserNonBlock()) {
                    return arg | O_NONBLOCK;
                } else {
                    return arg & ~O_NONBLOCK;
                }
            }
        break;

        case F_DUPFD:
        case F_DUPFD_CLOEXEC:
        case F_SETFD:
        case F_SETOWN:
        case F_SETSIG:
        case F_SETLEASE:
        case F_NOTIFY:
#ifdef F_SETPIPE_SZ
        case F_SETPIPE_SZ:
#endif
        case F_ADD_SEALS:
            {
                int arg = va_arg(va, int);
                va_end(va);
                return fcntl_origin(fd, cmd, arg); 
            }
            break;

            // 没有多余参数，默认分支足够
//         case F_GETFD:
//         case F_GETOWN:
//         case F_GETSIG:
//         case F_GETLEASE:
// #ifdef F_GETPIPE_SZ
//         case F_GETPIPE_SZ:
// #endif
//         case F_GET_SEALS:
//             {
//                 va_end(va);
//                 return fcntl_origin(fd, cmd);
//             }
//             break;

        case F_SETLK:
        case F_SETLKW:
        case F_GETLK: 
        case F_OFD_SETLK:
        case F_OFD_SETLKW:  
        case F_OFD_GETLK:
            {
                struct flock* arg = va_arg(va, struct flock*);
                va_end(va);
                return fcntl_origin(fd, cmd, arg);
            }
            break;

        case F_GETOWN_EX:
        case F_SETOWN_EX:
            {
                struct f_owner_exlock* arg = va_arg(va, struct f_owner_exlock*);
                va_end(va);
                return fcntl_origin(fd, cmd, arg);
            }
            break;

        case F_GET_RW_HINT:
        case F_SET_RW_HINT:
        case F_GET_FILE_RW_HINT:
        case F_SET_FILE_RW_HINT:
            {
                uint64_t* arg = va_arg(va, uint64_t*);
                va_end(va);
                return fcntl_origin(fd, cmd, arg);
            }
            break;

        default:
            va_end(va);
            return fcntl_origin(fd, cmd);
    }
}

int ioctl(int fd, unsigned long request, ...) {
    va_list va;
    va_start(va, request);
    void* arg = va_arg(va, void*);
    va_end(va);

    if (request == FIONBIO) {
        bool user_noblock = !!*(int*)arg;
        auto fdctx = g_fdmanager->getFdContext(fd);
        if (!fdctx || fdctx->isClose() || !fdctx->isSocketFd()) {
            return ioctl_origin(fd, request, arg);
        }
        // 只针对 sockfd
        fdctx->setUserNonBlock(user_noblock);
    }
    return ioctl_origin(fd, request, arg);
}

int getsockopt(int sockfd, int level, int optname,
               void *optval, socklen_t *optlen) {
    return getsockopt_origin(sockfd, level, optname, optval, optlen);
}

int setsockopt(int sockfd, int level, int optname,
               const void *optval, socklen_t optlen) {
    if (!reyao::t_hookEnable) {
        return setsockopt_origin(sockfd, level, optname, optval, optlen);
    }
    if (level == SOL_SOCKET) {
        // 在设置 sockfd 超时的同时更新 fdctx 的超时时间
        if (optname == SO_RCVTIMEO || optname == SO_SNDTIMEO) {
            auto fdctx = g_fdmanager->getFdContext(sockfd);
            if (fdctx) {
                const timeval* timeout = (const timeval*)optval;
                fdctx->setTimeout(optname, timeout->tv_sec * 1000 + timeout->tv_usec / 1000);
            }
        }
    }
    return setsockopt_origin(sockfd, level, optname, optval, optlen);
}


} // extern "C"
