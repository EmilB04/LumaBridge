// Incremental HTTP/1.1 request parser: enough for local JSON POSTs from game engines
// (Content-Length or chunked bodies, keep-alive). Pure C++, unit tested.
#pragma once

#include <cctype>
#include <cstdlib>
#include <string>

namespace luma::http {

struct Request {
    std::string method;
    std::string path;  // query string stripped
    std::string body;
    bool keepAlive = true;
};

class Parser {
public:
    enum class Status { NeedMore, Done, Error };

    static constexpr size_t kMaxHeader = 64 * 1024;
    static constexpr size_t kMaxBody = 4 * 1024 * 1024;

    void Feed(const char* data, size_t n) { buf_.append(data, n); }

    // Extracts one complete request from the buffered bytes. Leftover bytes (a pipelined next
    // request) stay buffered for the following call.
    Status Next(Request* out) {
        size_t headerEnd = buf_.find("\r\n\r\n");
        if (headerEnd == std::string::npos) return buf_.size() > kMaxHeader ? Status::Error : Status::NeedMore;

        Request r;
        size_t lineEnd = buf_.find("\r\n");
        std::string requestLine = buf_.substr(0, lineEnd);
        size_t sp1 = requestLine.find(' ');
        size_t sp2 = requestLine.find(' ', sp1 + 1);
        if (sp1 == std::string::npos || sp2 == std::string::npos) return Status::Error;
        r.method = requestLine.substr(0, sp1);
        r.path = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);
        r.path = r.path.substr(0, r.path.find('?'));
        const std::string version = requestLine.substr(sp2 + 1);
        r.keepAlive = version != "HTTP/1.0";

        size_t contentLength = 0;
        bool chunked = false;
        size_t pos = lineEnd + 2;
        while (pos < headerEnd) {
            size_t eol = buf_.find("\r\n", pos);
            std::string line = buf_.substr(pos, eol - pos);
            pos = eol + 2;
            size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string name = Lower(line.substr(0, colon));
            std::string value = Lower(Trim(line.substr(colon + 1)));
            if (name == "content-length") {
                contentLength = std::strtoul(value.c_str(), nullptr, 10);
                if (contentLength > kMaxBody) return Status::Error;
            } else if (name == "transfer-encoding") {
                chunked = value.find("chunked") != std::string::npos;
            } else if (name == "connection") {
                if (value.find("close") != std::string::npos) r.keepAlive = false;
                if (value.find("keep-alive") != std::string::npos) r.keepAlive = true;
            }
        }

        size_t bodyStart = headerEnd + 4;
        size_t consumed;
        if (chunked) {
            Status s = DecodeChunked(bodyStart, &r.body, &consumed);
            if (s != Status::Done) return s;
        } else {
            if (buf_.size() - bodyStart < contentLength) return Status::NeedMore;
            r.body = buf_.substr(bodyStart, contentLength);
            consumed = bodyStart + contentLength;
        }
        buf_.erase(0, consumed);
        *out = std::move(r);
        return Status::Done;
    }

private:
    Status DecodeChunked(size_t pos, std::string* body, size_t* consumed) const {
        body->clear();
        while (true) {
            size_t eol = buf_.find("\r\n", pos);
            if (eol == std::string::npos) return Status::NeedMore;
            size_t size = std::strtoul(buf_.substr(pos, eol - pos).c_str(), nullptr, 16);
            pos = eol + 2;
            if (size == 0) {
                // Optional trailers, then the final CRLF.
                size_t end = buf_.find("\r\n", pos);
                while (end != std::string::npos && end != pos) {
                    pos = end + 2;
                    end = buf_.find("\r\n", pos);
                }
                if (end == std::string::npos) return Status::NeedMore;
                *consumed = end + 2;
                return Status::Done;
            }
            if (body->size() + size > kMaxBody) return Status::Error;
            if (buf_.size() < pos + size + 2) return Status::NeedMore;
            body->append(buf_, pos, size);
            pos += size + 2;
        }
    }

    static std::string Lower(std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }
    static std::string Trim(const std::string& s) {
        size_t b = s.find_first_not_of(" \t"), e = s.find_last_not_of(" \t");
        return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
    }

    std::string buf_;
};

inline std::string BuildResponse(int status, const std::string& body, bool keepAlive) {
    const char* reason = status == 200 ? "OK" : status == 400 ? "Bad Request" : status == 404 ? "Not Found" : "Error";
    return "HTTP/1.1 " + std::to_string(status) + " " + reason +
           "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
           (keepAlive ? "\r\nConnection: keep-alive" : "\r\nConnection: close") + "\r\n\r\n" + body;
}

}  // namespace luma::http
