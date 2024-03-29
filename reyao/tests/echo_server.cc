#include "reyao/tcp_server.h"
#include "reyao/log.h"
#include "reyao/bytearray.h"
#include "reyao/hook.h"
#include "reyao/scheduler.h"
#include "reyao/socket_stream.h"

using namespace reyao;

class EchoServer : public TcpServer {
public:
    typedef std::shared_ptr<EchoServer> SPtr;
    EchoServer(Scheduler* scheduler, IPv4Address::SPtr addr)
        : TcpServer(scheduler, addr, "echo_server") {}


    void handleClient(Socket::SPtr client) override;

};

void EchoServer::handleClient(Socket::SPtr client) {
    LOG_INFO << "handle new client";
    SocketStream ss(client);
    ByteArray ba;
    while (ss.read(&ba, 1024) > 0) {
        ss.write(&ba, 1024);
    }
    
}

void test() {
    // g_logger->setLevel(LogLevel::INFO);

    auto addr = IPv4Address::CreateAddress("0.0.0.0", 30000);
    Scheduler scheduler;
    scheduler.startAsync();
    EchoServer server(&scheduler, addr);
    server.start();

    scheduler.wait();
}

int main(int argc, char** argv) {
    test();
    return 0;
}