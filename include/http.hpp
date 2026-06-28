#pragma once

#include <string>
#include <vector>
#include <map>
#include <utility>
#include <functional>

#include "types.hpp"

namespace ocli {

void http_global_init();

struct HttpRequest {
    std::string method = "GET";
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    long        timeout_ms       = 20000;
    bool        follow_redirects = true;
    long        max_redirects    = 5;
};

struct HttpResponse {
    long        status = 0;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string error;
    bool        network_error = false;

    bool        ok() const { return error.empty() && status >= 200 && status < 300; }
    std::string header(const std::string& name) const;
    std::string content_type() const;
};

HttpResponse http_perform(const HttpRequest& req);

HttpResponse http_stream(const HttpRequest& req,
                         const std::function<bool(const char* data, size_t n)>& on_chunk);

class HttpClient {
public:
    HttpClient();
    ~HttpClient();
    HttpResponse perform(const HttpRequest& req);
    HttpResponse stream(const HttpRequest& req,
                        const std::function<bool(const char* data, size_t n)>& on_chunk);

    std::vector<std::string> cookie_names() const;
private:
    void* curl_share_ = nullptr;
    std::string cookie_jar_;
};

std::string url_encode(const std::string& s);
std::string build_query(const std::map<std::string, std::string>& params);

}
