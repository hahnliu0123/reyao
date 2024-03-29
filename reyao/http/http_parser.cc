#include "reyao/http/http_parser.h"
#include "reyao/http/http_request.h"
#include "reyao/http/http_response.h"
#include "reyao/log.h"
#include "reyao/util.h"

#include <string.h>

#include <string>
#include <algorithm>

namespace reyao {

static size_t s_maxReqBufferSize = 4 * 1024;
static size_t s_maxResBufferSize = 64 * 1024 * 1024;

HttpParser::HttpParser(SocketStream* stream)
    : stream_(stream),
      ba_(),
      contentLen_(0) {
}

HttpRequestParser::HttpRequestParser(SocketStream* stream, HttpRequest* req)
    : HttpParser(stream),
      req_(req) {
}

bool HttpRequestParser::parseRequest() {
    while (!finish_ && !error_) {
        if (readSize_ == s_maxReqBufferSize) {
            return false;
        }
        int len = stream_->read(&ba_, s_maxReqBufferSize);

        if (len <= 0) {
            return false;
        }

        while (!error_) {
            if (parseState_ == PARSE_FIRST_LINE) {
                LOG_INFO << "parse";
                const char* start = ba_.peek();
                const char* end = ba_.findCRLF();
                if (end == nullptr) {
                    break;
                }
                ba_.setReadPos(ba_.getReadPos() + end - start + 2);
                parseFirstLine(start, end);

            } else if (parseState_ == PARSE_HEADER) {
                const char* start = ba_.peek();
                const char* end = ba_.findCRLF();
                if (end == nullptr) {
                    break;
                }
                ba_.setReadPos(ba_.getReadPos() + end - start + 2);
                parseHeader(start, end);
            } else if (parseState_ == PARSE_BODY) {
                if (!chunked_ && !contentLen_) {
                    finish_ = true;
                }
                if (chunked_) {
                    parseChunkedBody();
                } else {
                    parseFixedBody();
                }
                //finish or read more
                break;
            }
        }
    }
    return !error_;
}

void HttpRequestParser::parseFirstLine(const char* begin, const char* end) {
//method scheme://host:port/path?query#fragment version'\0''\0'
    LOG_INFO << "parseline";
    const char* space  = std::find(begin, end, ' ');
    if (space == end) {
        error_ = true;
        return;
    }

    req_->setMethod(StringToHttpMethod(std::string(begin, space - begin)));
    begin = space + 1;
    space = std::find(begin, end, ' ');
    if (space == end) {
        error_ = true;
        return;
    }
    std::string version(space + 1, end - space - 1);
    if (version == "HTTP/1.0") {
        req_->setVersion(0x10);
    } else if (version == "HTTP/1.1") {
        req_->setVersion(0x11);
    } else {
        error_ = true;
        return;
    }

    //TODO: should handle "http:// https://"?
    begin = std::find(begin, space, '/');
    if (begin == space) {
        error_ = true;
        return;
    }
    end = space;
    end = parseURI(begin, end, '#');
    end = parseURI(begin, end, '?');
    end = parseURI(begin, end, '/');

    parseState_ = PARSE_HEADER;
}

void HttpRequestParser::parseHeader(const char* begin, const char* end) {
    if (*begin == '\r') {
        parseState_ = PARSE_BODY;
        return;
    }
    const char* temp = std::find(begin, end, ':');
    if (temp != end && temp + 2 != end) {
        std::string title(begin, temp - begin);
        std::string value(temp + 2, end - temp -2); 
        if (!strcasecmp(title.c_str(), "transfer-encoding") &&
            !strcasecmp(value.c_str(), "chunked")) {
            chunked_ = true;
        }
        if (!chunked_) {
            if (!strcasecmp(title.c_str(), "content-length")) {
                contentLen_ = std::stoi(value);
            }
        }
        req_->addHeader(title, value);
        return;
    }
    error_ = true;
}

void HttpRequestParser::parseChunkedBody() {
    while (!error_) {
        if (chunkSize_ == PARSE_CHUNK_SIZE) {
            const char* start = ba_.peek();
            const char* end = ba_.findCRLF();
            if (end == nullptr) {
                break;
            }
            ba_.setReadPos(ba_.getReadPos() + end - start + 2);
            std::string hex(start, end - start);
            chunkSize_ = HexToDec(hex);
            if (chunkSize_ == (size_t)-1) {
                error_ = true;
                break;
            } 
            if (chunkSize_ == 0) {
                req_->setBody(body_);
                finish_ = true;
                break;
            }
            chunkState_ = PARSE_CHUNK_CONTENT;
        } else if (chunkState_ == PARSE_CHUNK_CONTENT) {
                if (chunkSize_ + 2 <= ba_.getReadSize()) {
                    body_ += std::string(ba_.peek(), chunkSize_ + 2);
                    ba_.setReadPos(ba_.getReadPos() + chunkSize_ + 2);
                    chunkSize_ = 0;
                    chunkState_ = PARSE_CHUNK_SIZE;
                } else {
                    break;
                }
        }
    }
}

void HttpRequestParser::parseFixedBody() {
    if (contentLen_ == 0) {
        //body为chunk或没有body
        finish_ = true;
    } else if (ba_.getReadSize() >= contentLen_) {
        req_->setBody(std::string(ba_.peek(), contentLen_));
        ba_.setReadPos(ba_.getReadPos() + contentLen_);
        finish_ = true;
    }
}

const char* HttpRequestParser::parseURI(const char* begin, const char* end, const char& des) {
    const char* temp = std::find(begin, end, des);
    if (temp != end) {
        switch (des) {
            case '#': {
                req_->setFragment(std::string(temp + 1, end - temp - 1));
            break;
            }
            case '?': {
                req_->setQuery(std::string(temp + 1, end - temp - 1));
            break;
            }
            case  '/': {
                req_->setPath(std::string(temp, end - temp));
            break;
            }
            default: {
            break;
            }
        }
    }
    return temp;
}

HttpResponseParser::HttpResponseParser(SocketStream* stream, HttpResponse* rsp)
    : HttpParser(stream),
      rsp_(rsp) {
}

bool HttpResponseParser::parseResponse() {
    while (!finish_ && !error_) {
        if (readSize_ == s_maxResBufferSize) {
            return false;
        }
        int len = stream_->read(&ba_, s_maxResBufferSize);

        if (len <= 0) {
            return false;
        }

        while (!error_) {
            if (parseState_ == PARSE_FIRST_LINE) {
                const char* start = ba_.peek();
                const char* end = ba_.findCRLF();
                if (end == nullptr) {
                    break;
                }
                ba_.setReadPos(ba_.getReadPos() + end - start + 2);
                parseFirstLine(start, end);

            } else if (parseState_ == PARSE_HEADER) {
                const char* start = ba_.peek();
                const char* end = ba_.findCRLF();
                if (end == nullptr) {
                    break;
                }
                ba_.setReadPos(ba_.getReadPos() + end - start + 2);
                parseHeader(start, end);
            } else if (parseState_ == PARSE_BODY) {
                if (!chunked_ && !contentLen_) {
                    finish_ = true;
                }
                if (chunked_) {
                    parseChunkedBody();
                } else {
                    parseFixedBody();
                }
                //finish or read more
                break;
            }
        }
    }
    return !error_;
}

void HttpResponseParser::parseFirstLine(const char* begin, const char* end) {
    //version status_code reason
    const char* space  = std::find(begin, end, ' ');
    if (space == end) {
        error_ = true;
        return;
    }
    std::string version(begin, space - begin);
    if (version == "HTTP/1.0") {
       rsp_->setVersion(0x10);
    } else if (version == "HTTP/1.1") {
        rsp_->setVersion(0x11);
    } else {
        error_ = true;
        return;
    }

    begin = space + 1;
    space = std::find(begin, end, ' ');
    if (space == end) {
        error_ = true;
        return;
    }
    int status_code = stoi(std::string(begin, space - begin));
    if (!isHttpStatus(status_code)) {
        error_ = true;
        return;
    }
    rsp_->setStatus((HttpStatus)status_code);
    rsp_->setReason(std::string(space, end - space));


    parseState_ = PARSE_HEADER;
}

void HttpResponseParser::parseHeader(const char* begin, const char* end) {
    if (*begin == '\r') {
        parseState_ = PARSE_BODY;
        return;
    }
    const char* temp = std::find(begin, end, ':');
    if (temp != end && temp + 2 != end) {
        std::string title(begin, temp - begin);
        std::string value(temp + 2, end - temp -2); 
        if (!strcasecmp(title.c_str(), "transfer-encoding") &&
            !strcasecmp(value.c_str(), "chunked")) {
            chunked_ = true;
        }
        if (!chunked_) {
            if (!strcasecmp(title.c_str(), "content-length")) {
                contentLen_ = std::stoi(value);
            }
        }
        rsp_->addHeader(title, value);
        return;
    }
    error_ = true;
}

void HttpResponseParser::parseChunkedBody() {
    while (!error_) {
        if (chunkSize_ == PARSE_CHUNK_SIZE) {
            const char* start = ba_.peek();
            const char* end = ba_.findCRLF();
            if (end == nullptr) {
                break;
            }
            ba_.setReadPos(ba_.getReadPos() + end - start + 2);
            std::string hex(start, end - start);
            chunkSize_ = HexToDec(hex);
            if (chunkSize_ == (size_t)-1) {
                error_ = true;
                break;
            } 
            if (chunkSize_ == 0) {
                rsp_->setBody(body_);
                finish_ = true;
                break;
            }
            chunkState_ = PARSE_CHUNK_CONTENT;
        } else if (chunkState_ == PARSE_CHUNK_CONTENT) {
                    if (chunkSize_ + 2 <= ba_.getReadSize()) {
                    body_ += std::string(ba_.peek(), chunkSize_ + 2);
                    ba_.setReadPos(ba_.getReadPos() + chunkSize_ + 2);
                    chunkSize_ = 0;
                    chunkState_ = PARSE_CHUNK_SIZE;
                } else {
                    break;
                }
        }
    }
}

void HttpResponseParser::parseFixedBody() {
    if (contentLen_ == 0) {
        //body is chunk or no body
        finish_ = true;
    } else if (ba_.getReadSize() >= contentLen_) {
        rsp_->setBody(std::string(ba_.peek(), contentLen_));
        ba_.setReadPos(ba_.getReadPos() + contentLen_);
        finish_ = true;
    }
}

} // namespace reyao