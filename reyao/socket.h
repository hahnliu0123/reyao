#pragma once 

#include "reyao/address.h"
#include "reyao/nocopyable.h"

#include <sys/socket.h>
#include <sys/types.h>

#include <memory>
#include <sstream>

namespace reyao {

class Socket : public NoCopyable,
               public std::enable_shared_from_this<Socket> {
public:
    typedef std::shared_ptr<Socket> SPtr;
    Socket(int type, int protocol = 0);
    ~Socket();

    int getType() const { return type_;}
    int getProtocol() const { return protocol_; }
    int getSockfd() const { return sockfd_; }
    IPv4Address::UPtr getLocalAddr() const;
    IPv4Address::UPtr getPeerAddr() const;
    bool isConnected() const { return connected_; }
    bool isValid() const { return sockfd_ != -1; }
    std::string toString() const;
    int64_t getRecvTimeout() const;
    void setRecvTimeout(int64_t timeout);
    int64_t getSendTimeout() const;
    void setSendTimeout(int64_t timeout);

    int getError();
    bool getOption(int level, int option, void* result, socklen_t* len);
    template <typename T>
    bool getOption(int level, int option, T& result) {
        socklen_t len = sizeof(T);
        return getOption(level, option, &result, &len);
    }

    bool setOption(int level, int option, const void* result, socklen_t len);
    template <typename T>
    bool setOption(int level, int option, const T& result) {
        return setOption(level, option, &result, sizeof(T));
    }

    bool init(int sockfd);
    void setReuseAddr();
    void setNoDelay();
    void socket();
    bool bind(const IPv4Address& addr);
    bool listen(int backlog = SOMAXCONN);
    Socket::SPtr accept();
    bool close();
    bool connect(const IPv4Address& addr, int64_t timeout = -1);
    int send(const void* buf, size_t len, int flags = 0);
    int send(iovec* buf, int iovcnt, int flags = 0);
    int sendTo(const void* buf, size_t len, const IPv4Address& to, int flags = 0);
    int sendTo(iovec* buf, int iovcnt, const IPv4Address& to, int flags = 0);
    int recv(void* buf, size_t len, int flags = 0);
    int recv(iovec* buf, int iovcnt, int flags = 0);
    int recvFrom(void* buf, size_t len, const IPv4Address& from, int flags = 0);
    int recvFrom(iovec* buf, int iovcnt, const IPv4Address& from, int flags = 0);

    bool cancelRead();
    bool cancelWrite();
    bool cancelAll();

private:
    int type_ = 0;
    int protocol_ = 0;
    int sockfd_ = -1;
    bool listening_ = false;
    bool connected_ = false;
};

} //namespace reyao