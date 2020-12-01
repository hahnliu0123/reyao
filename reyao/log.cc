#include "reyao/log.h"
#include "reyao/util.h"

#include <iostream>
#include <functional>

namespace reyao {

thread_local char t_time[64]; //缓存格式化输出时间，避免频繁调用strftime，做缓存后，wsl2中写300m数据的速度从36s提升到了25s
thread_local time_t t_last_second; //上一次日格式化时间的秒数

LogData::LogData(LogLevel::Level level, time_t time, uint32_t elapse,
                 uint32_t thread_id, uint32_t coroutine_id, 
                 std::string thread_name, const char *file_name, uint32_t line)
    : level_(level),
      time_(time),
      elapse_(elapse),
      thread_id_(thread_id),
      coroutine_id_(coroutine_id),
      thread_name_(thread_name),
      file_name_(file_name),
      line_(line) {
                     
}
        
LogDataWrap::LogDataWrap(LogLevel::Level level, time_t time, uint32_t elapse,
            uint32_t thread_id, uint32_t coroutine_id, std::string thread_name, 
            const char* file_name, uint32_t line)
    : data_(level, time, elapse, thread_id, 
            coroutine_id, thread_name, file_name, line) {

}

LogDataWrap::~LogDataWrap() {
    g_logger->append(data_);
}

class MessageFmtItem : public LogFormatter::FmtItem {
 public:
    explicit MessageFmtItem(const std::string &str = "") {}
    void format(std::stringstream &ss, const LogData& data) override {
        ss << data.getContent();
    }
};

class LevelFmtItem : public LogFormatter::FmtItem {
 public:
    explicit LevelFmtItem(const std::string& str = "") {}
    void format(std::stringstream& ss, const LogData& data) override { 
        ss << LogLevel::ToString(data.getLevel());
    }
};

class DateTimeFmtItem : public LogFormatter::FmtItem {
 public:
    explicit DateTimeFmtItem(const std::string& str = "") {}
    void format(std::stringstream& ss, const LogData& data) override {   
        struct tm tm;
        time_t second = data.getTime();
        if (second != t_last_second)  {
            //缓存的格式化时间需要更新
            t_last_second = second;
            localtime_r(&second, &tm);
            strftime(t_time, sizeof(t_time), "%Y-%m-%d %H:%M:%S", &tm);
        }
        ss << t_time;
        // ss << GetCurrentTime();
    }
};

class ElapseFmtItem : public LogFormatter::FmtItem {
 public:
    explicit ElapseFmtItem(const std::string& str = "") {}
    void format(std::stringstream& ss, const LogData& data) override {
        ss << data.getElapse();
    }
};

class ThreadIdFmtItem : public LogFormatter::FmtItem {
 public:
    explicit ThreadIdFmtItem(const std::string& str = "") {}
        void format(std::stringstream& ss, const LogData& data) override {
        ss << data.getThreadId();
    }
};

class CoroutineIdFmtItem : public LogFormatter::FmtItem {
 public:
    explicit CoroutineIdFmtItem(const std::string& str = "") {}
    void format(std::stringstream& ss, const LogData& data) override {
        ss << data.getCoroutineId();
    }
};

class ThreadNameFmtItem : public LogFormatter::FmtItem {
 public:
    explicit ThreadNameFmtItem(const std::string& str = "") {}
    void format(std::stringstream& ss, const LogData& data) override {
        ss << data.getThreadName();
    }
};

class FileNameFmtItem : public LogFormatter::FmtItem {
 public:
    explicit FileNameFmtItem(const std::string& str = "") {}
    void format(std::stringstream& ss, const LogData& data) override {
        ss << data.getFileName();
    }
};

class LineFmtItem : public LogFormatter::FmtItem {
 public:
    explicit LineFmtItem(const std::string& str = "") {}
    void format(std::stringstream& ss, const LogData& data) override {
        ss << data.getLine();
    }
};

class NextLineFmtItem : public LogFormatter::FmtItem {
 public:
    explicit NextLineFmtItem(const std::string& str = "") {}
    void format(std::stringstream& ss, const LogData& data) override {
        ss << "\n";
    }
};

class TableFmtItem : public LogFormatter::FmtItem {
 public:
    explicit TableFmtItem(const std::string& str = "") {}
    void format(std::stringstream& ss, const LogData& data) override {
        ss << "\t";
    }
};

class StringFmtItem : public LogFormatter::FmtItem {
 public:
    explicit StringFmtItem(const std::string& str):str_(str) {}
    void format(std::stringstream& ss, const LogData& data) override {
        ss << str_;
    }
 private:
    std::string str_;
};

const char* LogLevel::ToString(LogLevel::Level level) {
    switch (level) {
        case LogLevel::DEBUG:   return "DEBUG";
        case LogLevel::INFO:    return "INFO";
        case LogLevel::WARN:    return "WARN";
        case LogLevel::ERROR:   return "ERROR";
        case LogLevel::FATAL:   return "FATAL";

        default:    return "UNKNOW";
    }
}

LogFormatter::LogFormatter(const std::string& pattern): pattern_(pattern) {
    init();
}

//%xxx %%
void LogFormatter::init() {   
    //str type  // %str
    std::vector<std::pair<std::string, int>> vec;
    std::string s;
    for (size_t i = 0; i < pattern_.size(); i++) {
        if (pattern_[i] != '%') {
            //如果是普通字符就放进s
            s.append(1, pattern_[i]);
            continue;
        }
        
        //pattern_[i]为 %' 

        //此时 i 为 % 且 i + 1 为 % ，说明出现 %% 转义， 此时 % 是一个普通的 % ，跳过不解析
        if (i + 1 < pattern_.size() && pattern_[i + 1] == '%') {
            s.append(1, '%');
            continue;
        }

        //从 i + 1 开始解析
        size_t n = i + 1;
        std::string str;
        while (n < pattern_.size()) {
            if (!isalpha(pattern_[n])) {
                //不是字母
                str = pattern_.substr(i + 1, n - i - 1); //如 %xx% ，str = xx
                break;
            }       
            ++n;
            if (n == pattern_.size()) {
                if (str.empty()) {
                    str = pattern_.substr(i + 1); //遍历完 % 后面都是字母，则全部记下来
                }
            }
        }

        //i 为 % 且解析完毕
        if (!s.empty()) {
            vec.push_back({s, 0});
            s.clear();
        }
        vec.push_back({str, 1});
        i = n - 1;
    }

    if (!s.empty()) {
        //如果 pattern 中没有 % ，说明是个普通的 str ，这时 s == pattern_ ， 放进 vec 中
        vec.push_back({s, 0});
    }

    //开始对每一种情况解析
    static std::map<std::string, std::function<FmtItem::SPtr()>> format_to_item = {
#define XX(c, Item) \
        {#c, [] () { return FmtItem::SPtr(std::make_shared<Item>());} }

        XX(m, MessageFmtItem),
        XX(p, LevelFmtItem),
        XX(d, DateTimeFmtItem),
        XX(r, ElapseFmtItem),
        XX(t, ThreadIdFmtItem),
        XX(c, CoroutineIdFmtItem),
        XX(N, ThreadNameFmtItem),
        XX(f, FileNameFmtItem),
        XX(l, LineFmtItem),
        XX(n, NextLineFmtItem),
        XX(T, TableFmtItem),

#undef XX
    };

    for (auto& i : vec) {
        if (i.second == 0) {
            items_.push_back(FmtItem::SPtr(std::make_shared<StringFmtItem>(i.first))); //普通字符串
        } else {
            auto iter = format_to_item.find(i.first); //得到 str 对应的 Item ，如 d 对应 DateTimeFmtItem
            if (iter == format_to_item.end()) {
                items_.push_back(FmtItem::SPtr(std::make_shared<StringFmtItem>("<<error format %" + i.first + ">>"))); //格式字符不在map里
            } else {
                items_.push_back(iter->second()); //生成对应的 Item 实例
            }
        }
    }
}

std::string LogFormatter::format(const LogData& data) {
    std::stringstream ss;
    for (auto item : items_) {
        item->format(ss, data); //将每个item写进流里
    }
    return ss.str();
}

Logger::Logger()
    : level_(LogLevel::DEBUG), //默认为DEBUG级别
      formatter_(std::make_shared<LogFormatter>("[%d]%T[%p]%T[%N:%t]%T[%c]%T[%f:%l]%T%m%n")), //默认日志格式
      appender_(std::make_shared<ConsoleAppender>()) {

}

void Logger::setLevel(LogLevel::Level level) {   
    MutexGuard lock(mutex_);
    level_ = level; 
}

void Logger::setFormatter(std::string pattern) {   
    MutexGuard lock(mutex_);
    formatter_ = std::make_shared<LogFormatter>(pattern);
}

void Logger::setConsoleAppender() {   
    MutexGuard lock(mutex_);
    appender_ = std::make_shared<ConsoleAppender>();
}

void Logger::setFileAppender() {
    MutexGuard lock(mutex_);
    appender_ = std::make_shared<FileAppender>();
}

void Logger::append(const LogData& data) {   
    std::shared_ptr<LogFormatter> formatter;
    std::shared_ptr<LogAppender> appender;
    {
        MutexGuard lock(mutex_);
        formatter = formatter_;
        appender = appender_;
    }
    std::string logline = formatter->format(data);
    appender->append(logline);
}

void ConsoleAppender::append(const std::string& logline) {
    printf(logline.c_str(), logline.size());
}

FileAppender::FileAppender() {
    log_.start();
}

FileAppender::~FileAppender() {   
    log_.stop();
}

void FileAppender::append(const std::string& logline) {
    log_.append(logline.c_str(), logline.size());
}

} //namespace reyao