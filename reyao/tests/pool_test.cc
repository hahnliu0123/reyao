#include "reyao/http/http_connectionpool.h"
#include "reyao/scheduler.h"
#include "reyao//log.h"

using namespace reyao;

void test() {
    HttpConnectionPool::SPtr pool(new HttpConnectionPool(
                                  "www.sylar.top", "", 80, 10,
                                  30 * 1000, 20));
    
    Worker::GetWorker()->addTimer(1000, [pool](){
        auto res = pool->doGet("/blog/", 10 * 1000);
        LOG_INFO << res->toString();
    }, true);
}

int main(int argc, char** argv) {
    Scheduler sh(0);
    sh.addTask(test);
    sh.start();
    return 0;
}