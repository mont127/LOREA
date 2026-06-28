#include "http.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <vector>

namespace ocli {

namespace {

inline char lower_ascii(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

bool iequal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (lower_ascii(a[i]) != lower_ascii(b[i])) return false;
    return true;
}

}

std::string HttpResponse::header(const std::string& name) const {
    for (const auto& kv : headers)
        if (iequal(kv.first, name)) return kv.second;
    return "";
}

std::string HttpResponse::content_type() const {
    return header("Content-Type");
}

namespace {

struct RequestCtx {
    HttpResponse* resp = nullptr;
    const std::function<bool(const char* data, size_t n)>* on_chunk = nullptr;
    bool aborted = false;
};

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<RequestCtx*>(userdata);
    size_t n = size * nmemb;
    if (ctx->on_chunk) {

        if (!(*ctx->on_chunk)(ptr, n)) {
            ctx->aborted = true;
            return 0;
        }
    } else {
        ctx->resp->body.append(ptr, n);
    }
    return n;
}

size_t header_cb(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* ctx = static_cast<RequestCtx*>(userdata);
    size_t n = size * nitems;
    std::string line(buffer, n);

    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();

    if (line.rfind("HTTP/", 0) == 0) {
        ctx->resp->headers.clear();
        return n;
    }
    if (line.empty()) return n;
    auto pos = line.find(':');
    if (pos == std::string::npos) return n;
    std::string name = line.substr(0, pos);
    std::string val = line.substr(pos + 1);
    size_t s = 0;
    while (s < val.size() && (val[s] == ' ' || val[s] == '\t')) ++s;
    val = val.substr(s);
    ctx->resp->headers.emplace_back(name, val);
    return n;
}

HttpResponse do_request(const HttpRequest& req, CURLSH* share, bool use_cookies,
                        const std::function<bool(const char* data, size_t n)>* on_chunk,
                        std::string* cookie_jar_out) {
    HttpResponse resp;

    CURL* h = curl_easy_init();
    if (!h) {
        resp.error = "curl_easy_init failed";
        resp.network_error = true;
        return resp;
    }

    RequestCtx ctx;
    ctx.resp = &resp;
    ctx.on_chunk = on_chunk;

    curl_easy_setopt(h, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, req.timeout_ms);

    curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, &ctx);

    if (req.follow_redirects) {
        curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(h, CURLOPT_MAXREDIRS, req.max_redirects);
    } else {
        curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 0L);
    }

    std::string method = req.method.empty() ? std::string("GET") : req.method;
    if (method == "HEAD") {
        curl_easy_setopt(h, CURLOPT_NOBODY, 1L);
    } else if (method != "GET") {
        curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, method.c_str());
    }
    if (!req.body.empty()) {
        curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, static_cast<long>(req.body.size()));
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, req.body.c_str());

        if (method != "POST" && method != "HEAD")
            curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, method.c_str());
    }

    struct curl_slist* hdrs = nullptr;
    for (const auto& kv : req.headers) {
        std::string line = kv.first + ": " + kv.second;
        hdrs = curl_slist_append(hdrs, line.c_str());
    }
    if (hdrs) curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);

    if (share) curl_easy_setopt(h, CURLOPT_SHARE, share);
    if (use_cookies) curl_easy_setopt(h, CURLOPT_COOKIEFILE, "");

    CURLcode res = curl_easy_perform(h);

    long code = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
    resp.status = code;

    if (res != CURLE_OK) {
        if (res == CURLE_WRITE_ERROR && ctx.aborted) {

        } else {
            resp.error = curl_easy_strerror(res);
            resp.network_error = true;
        }
    }

    if (cookie_jar_out) {
        struct curl_slist* cookies = nullptr;
        if (curl_easy_getinfo(h, CURLINFO_COOKIELIST, &cookies) == CURLE_OK) {
            std::string jar;
            for (struct curl_slist* c = cookies; c; c = c->next) {
                jar += c->data;
                jar += '\n';
            }
            *cookie_jar_out = jar;
        }
        if (cookies) curl_slist_free_all(cookies);
    }

    if (hdrs) curl_slist_free_all(hdrs);
    curl_easy_cleanup(h);
    return resp;
}

}

void http_global_init() {
    static bool done = false;
    if (done) return;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    done = true;
}

HttpResponse http_perform(const HttpRequest& req) {
    return do_request(req, nullptr, false, nullptr, nullptr);
}

HttpResponse http_stream(const HttpRequest& req,
                         const std::function<bool(const char* data, size_t n)>& on_chunk) {
    return do_request(req, nullptr, false, &on_chunk, nullptr);
}

HttpClient::HttpClient() {
    CURLSH* share = curl_share_init();
    if (share) {
        curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
        curl_share_ = share;
    }
}

HttpClient::~HttpClient() {
    if (curl_share_) {
        curl_share_cleanup(static_cast<CURLSH*>(curl_share_));
        curl_share_ = nullptr;
    }
}

HttpResponse HttpClient::perform(const HttpRequest& req) {
    return do_request(req, static_cast<CURLSH*>(curl_share_), true, nullptr, &cookie_jar_);
}

HttpResponse HttpClient::stream(const HttpRequest& req,
                                const std::function<bool(const char* data, size_t n)>& on_chunk) {
    return do_request(req, static_cast<CURLSH*>(curl_share_), true, &on_chunk, &cookie_jar_);
}

std::vector<std::string> HttpClient::cookie_names() const {

    std::set<std::string> names;
    size_t start = 0;
    while (start <= cookie_jar_.size()) {
        size_t nl = cookie_jar_.find('\n', start);
        std::string line = cookie_jar_.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (nl == std::string::npos)
            start = cookie_jar_.size() + 1;
        else
            start = nl + 1;
        if (line.empty() || line[0] == '#') {

            if (line.rfind("#HttpOnly_", 0) != 0) continue;
        }

        std::vector<std::string> fields;
        size_t fs = 0;
        while (true) {
            size_t tab = line.find('\t', fs);
            if (tab == std::string::npos) {
                fields.push_back(line.substr(fs));
                break;
            }
            fields.push_back(line.substr(fs, tab - fs));
            fs = tab + 1;
        }
        if (fields.size() >= 7 && !fields[5].empty()) names.insert(fields[5]);
    }
    return std::vector<std::string>(names.begin(), names.end());
}

std::string url_encode(const std::string& s) {
    char* esc = curl_easy_escape(nullptr, s.c_str(), static_cast<int>(s.size()));
    if (!esc) return std::string();
    std::string out(esc);
    curl_free(esc);
    return out;
}

std::string build_query(const std::map<std::string, std::string>& params) {
    std::string out;
    bool first = true;
    for (const auto& kv : params) {
        if (!first) out += '&';
        first = false;
        out += url_encode(kv.first);
        out += '=';
        out += url_encode(kv.second);
    }
    return out;
}

}
