#include "dashboard.hpp"
#include "lorea.hpp"
#include "render.hpp"
#include "types.hpp"
#include "secutil.hpp"
#include "pty_session.hpp"

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <chrono>
#include <sstream>
#include <cstdint>
#include <cstring>
#include <cctype>
#include <cerrno>
#include <csignal>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <unistd.h>

namespace ocli {

namespace {

std::mutex g_agent_mutex;

std::string trim_copy(const std::string& s) {
    std::size_t a = 0;
    std::size_t b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string lower_copy(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string lan_ipv4() {
    struct ifaddrs* ifap = nullptr;
    std::string result = "127.0.0.1";
    if (getifaddrs(&ifap) == 0) {
        for (struct ifaddrs* ifa = ifap; ifa != nullptr; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == nullptr) continue;
            if (ifa->ifa_addr->sa_family != AF_INET) continue;
            if (ifa->ifa_flags & IFF_LOOPBACK) continue;
            if (!(ifa->ifa_flags & IFF_UP)) continue;
            char ip[INET_ADDRSTRLEN] = {0};
            auto* sin = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
            if (inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip)) != nullptr) {
                std::string s(ip);
                if (!s.empty() && s != "127.0.0.1") {
                    result = s;
                    break;
                }
            }
        }
        freeifaddrs(ifap);
    }
    return result;
}

bool read_http_request(int fd, std::string& method, std::string& path, std::string& body) {
    std::string buf;
    char tmp[4096];
    std::size_t header_end = std::string::npos;
    while ((header_end = buf.find("\r\n\r\n")) == std::string::npos) {
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        buf.append(tmp, static_cast<std::size_t>(n));
        if (buf.size() > static_cast<std::size_t>(16) * 1024 * 1024) return false;
    }

    std::string headers = buf.substr(0, header_end);
    std::size_t line_end = headers.find("\r\n");
    std::string request_line = (line_end == std::string::npos) ? headers : headers.substr(0, line_end);

    {
        std::istringstream iss(request_line);
        std::string ver;
        iss >> method >> path >> ver;
    }
    if (method.empty() || path.empty()) return false;

    std::size_t qpos = path.find('?');
    if (qpos != std::string::npos) path.erase(qpos);

    std::size_t content_length = 0;
    std::size_t pos = (line_end == std::string::npos) ? headers.size() : line_end + 2;
    while (pos < headers.size()) {
        std::size_t eol = headers.find("\r\n", pos);
        std::size_t line_stop = (eol == std::string::npos) ? headers.size() : eol;
        std::string line = headers.substr(pos, line_stop - pos);
        pos = (eol == std::string::npos) ? headers.size() : eol + 2;
        std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = lower_copy(trim_copy(line.substr(0, colon)));
        if (key == "content-length") {
            try {
                content_length = static_cast<std::size_t>(std::stoul(trim_copy(line.substr(colon + 1))));
            } catch (...) {
                content_length = 0;
            }
        }
    }

    body = buf.substr(header_end + 4);
    while (body.size() < content_length) {
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        body.append(tmp, static_cast<std::size_t>(n));
        if (body.size() > static_cast<std::size_t>(16) * 1024 * 1024) break;
    }
    if (body.size() > content_length) body.resize(content_length);
    return true;
}

void send_response(int fd, int status, const std::string& content_type, const std::string& body) {
    const char* reason = "OK";
    if (status == 400) reason = "Bad Request";
    else if (status == 404) reason = "Not Found";
    else if (status == 500) reason = "Internal Server Error";

    std::ostringstream oss;
    oss << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
        << "Content-Type: " << content_type << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Access-Control-Allow-Origin: *\r\n"
        << "Cache-Control: no-store\r\n"
        << "Connection: close\r\n"
        << "\r\n"
        << body;

    std::string out = oss.str();
    std::size_t sent = 0;
    while (sent < out.size()) {
        ssize_t n = ::send(fd, out.data() + sent, out.size() - sent, 0);
        if (n <= 0) break;
        sent += static_cast<std::size_t>(n);
    }
}

void send_json(int fd, int status, const json& j) {
    send_response(fd, status, "application/json; charset=utf-8", j.dump());
}

bool parse_json_body(const std::string& body, json& out) {
    try {
        out = json::parse(body.empty() ? std::string("{}") : body);
    } catch (...) {
        return false;
    }
    return out.is_object();
}

std::string build_shared_context(const std::vector<Message>& msgs) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    const std::size_t max_msgs = 20;
    if (msgs.size() > max_msgs) start = msgs.size() - max_msgs;
    for (std::size_t i = start; i < msgs.size(); ++i) {
        std::string role = msg_role(msgs[i]);
        if (role != "user" && role != "assistant") continue;
        std::string content = msg_content(msgs[i]);
        if (content.empty()) continue;
        if (content.size() > 2000) content = content.substr(0, 2000) + " ...";
        lines.push_back((role == "user" ? std::string("User: ") : std::string("Assistant: ")) + content);
    }
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i) out += "\n\n";
        out += lines[i];
    }
    return out;
}

void handle_info(LOREA& agent, int fd, int port) {
    json j;
    {
        std::lock_guard<std::mutex> lk(g_agent_mutex);
        j["model"] = agent.model_name;
        j["backend"] = agent.backend;
        j["lan_url"] = std::string("http://") + lan_ipv4() + ":" + std::to_string(port);
        j["auto"] = agent.auto_mode;
        j["tool_access"] = agent.tool_access;
    }
    send_json(fd, 200, j);
}

void handle_history(LOREA& agent, int fd) {
    json msgs = json::array();
    {
        std::lock_guard<std::mutex> lk(g_agent_mutex);
        for (std::size_t i = 0; i < agent.messages.size(); ++i) {
            std::string role = msg_role(agent.messages[i]);
            if (role != "user" && role != "assistant") continue;
            std::string content = msg_content(agent.messages[i]);
            if (content.empty()) continue;
            msgs.push_back(json{{"role", role}, {"content", content}});
        }
    }
    send_json(fd, 200, json{{"messages", msgs}});
}

void handle_chat(LOREA& agent, int fd, const std::string& body) {
    json req;
    if (!parse_json_body(body, req)) {
        send_json(fd, 400, json{{"error", "invalid json body"}});
        return;
    }
    std::string message = req.value("message", std::string(""));
    if (trim_copy(message).empty()) {
        send_json(fd, 400, json{{"error", "missing message"}});
        return;
    }

    std::string response_text;
    std::string log_text;
    {
        std::lock_guard<std::mutex> lk(g_agent_mutex);

        std::string shared = build_shared_context(agent.messages);
        agent.messages.push_back(json{{"role", "user"}, {"content", message}});

        std::string ta = (agent.tool_access == "read_only" || agent.tool_access == "full")
                             ? agent.tool_access
                             : std::string("full");

        bool ok = false;
        try {
            json spec = json{{"name", "dashboard"}, {"task", message}};
            json result = agent.spawn_agent_chat(spec, shared, 180, 4, ta);
            if (result.is_object()) {
                std::string st = result.value("status", std::string("ok"));
                response_text = result.value("response", std::string(""));
                log_text = result.value("log", std::string(""));
                if (!response_text.empty() && st != "error" && st != "timeout") ok = true;
            }
        } catch (const std::exception&) {
            ok = false;
        }

        if (!ok) {
            try {
                std::string fallback = agent.agent_completion_once(agent.messages, 0.2, 1800);
                if (!fallback.empty()) response_text = fallback;
            } catch (const std::exception& e) {
                if (response_text.empty()) response_text = std::string("Error: ") + e.what();
            }
        }

        if (response_text.empty()) response_text = "The agent did not produce a response.";
        agent.messages.push_back(json{{"role", "assistant"}, {"content", response_text}});
    }

    send_json(fd, 200, json{{"response", response_text}, {"log", log_text}});
}

void handle_exec(LOREA& agent, int fd, const std::string& body) {
    json req;
    if (!parse_json_body(body, req)) {
        send_json(fd, 400, json{{"error", "invalid json body"}});
        return;
    }
    std::string command = req.value("command", std::string(""));
    if (!command.empty()) {
        terminal_session().write_input(command + "\n");
    }
    (void)agent;
    send_json(fd, 200, json{{"output", std::string()}});
}

bool term_send_all(int fd, const char* data, std::size_t len) {
    std::size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, data + sent, len - sent, 0);
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

bool term_send_event(int fd, const std::string& data) {
    std::string ev;
    ev.reserve(data.size() + 16);
    ev += "data: ";
    for (char c : data) {
        if (c == '\r') continue;
        if (c == '\n') ev += "\ndata: ";
        else ev += c;
    }
    ev += "\n\n";
    return term_send_all(fd, ev.data(), ev.size());
}

void handle_term_stream(int fd) {
    terminal_session().start();

    static const char* kHeaders =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: keep-alive\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "X-Accel-Buffering: no\r\n"
        "\r\n";
    if (!term_send_all(fd, kHeaders, std::strlen(kHeaders))) return;

    std::string snap = terminal_session().snapshot();
    std::size_t cursor = 0;
    terminal_session().read_since(cursor);
    if (!snap.empty()) {
        if (!term_send_event(fd, snap)) return;
    }

    for (;;) {
        std::string chunk = terminal_session().read_since(cursor);
        if (!chunk.empty()) {
            if (!term_send_event(fd, chunk)) return;
        } else {
            if (!term_send_all(fd, ":\n\n", 3)) return;
            if (!terminal_session().alive()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}

void handle_term_input(int fd, const std::string& body) {
    json req;
    if (!parse_json_body(body, req)) {
        send_json(fd, 400, json{{"error", "invalid json body"}});
        return;
    }
    std::string data = req.value("data", std::string(""));
    terminal_session().write_input(data);
    send_json(fd, 200, json{{"ok", true}});
}

void handle_term_resize(int fd, const std::string& body) {
    json req;
    if (!parse_json_body(body, req)) {
        send_json(fd, 400, json{{"error", "invalid json body"}});
        return;
    }
    int rows = req.value("rows", 24);
    int cols = req.value("cols", 80);
    if (rows < 1) rows = 1;
    if (cols < 1) cols = 1;
    terminal_session().resize(rows, cols);
    send_json(fd, 200, json{{"ok", true}});
}

void route(LOREA& agent, int fd, const std::string& method,
           const std::string& path, const std::string& body, int port) {
    if (method == "GET" && (path == "/" || path == "/index.html")) {
        send_response(fd, 200, "text/html; charset=utf-8",
                      DASHBOARD_HTML != nullptr ? std::string(DASHBOARD_HTML) : std::string());
        return;
    }
    if (method == "GET" && path == "/api/info") {
        handle_info(agent, fd, port);
        return;
    }
    if (method == "GET" && path == "/api/history") {
        handle_history(agent, fd);
        return;
    }
    if (method == "POST" && path == "/api/chat") {
        handle_chat(agent, fd, body);
        return;
    }
    if (method == "POST" && path == "/api/exec") {
        handle_exec(agent, fd, body);
        return;
    }
    if (method == "GET" && path == "/api/term/stream") {
        handle_term_stream(fd);
        return;
    }
    if (method == "POST" && path == "/api/term/input") {
        handle_term_input(fd, body);
        return;
    }
    if (method == "POST" && path == "/api/term/resize") {
        handle_term_resize(fd, body);
        return;
    }
    if (method == "OPTIONS") {
        send_response(fd, 200, "text/plain", std::string());
        return;
    }
    send_json(fd, 404, json{{"error", "not found"}});
}

void handle_client(LOREA& agent, int fd, int port) {
    int set = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &set, sizeof(set));
    std::string method;
    std::string path;
    std::string body;
    if (read_http_request(fd, method, path, body)) {
        try {
            route(agent, fd, method, path, body, port);
        } catch (const std::exception& e) {
            send_json(fd, 500, json{{"error", std::string(e.what())}});
        } catch (...) {
            send_json(fd, 500, json{{"error", "internal error"}});
        }
    }
    ::close(fd);
}

void serve(LOREA& agent, int port) {
    ::signal(SIGPIPE, SIG_IGN);

    int srv = ::socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        log_warn("dashboard: socket() failed");
        return;
    }

    int opt = 1;
    ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (::bind(srv, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        log_warn(std::string("dashboard: bind failed on port ") + std::to_string(port));
        ::close(srv);
        return;
    }
    if (::listen(srv, 16) < 0) {
        log_warn("dashboard: listen failed");
        ::close(srv);
        return;
    }

    std::string lan_url = std::string("http://") + lan_ipv4() + ":" + std::to_string(port);
    log_info("Dashboard live at " + lan_url);

    for (;;) {
        struct sockaddr_in cli;
        socklen_t cli_len = sizeof(cli);
        int fd = ::accept(srv, reinterpret_cast<struct sockaddr*>(&cli), &cli_len);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        std::thread(handle_client, std::ref(agent), fd, port).detach();
    }

    ::close(srv);
}

}

void start_dashboard(LOREA& agent, int port) {
    terminal_session();
    g_shared_terminal_active = true;
    std::thread([&agent, port]() {
        serve(agent, port);
    }).detach();
}

}
