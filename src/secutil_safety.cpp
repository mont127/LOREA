#include "secutil.hpp"
#include "ansi.hpp"

#include <string>
#include <vector>
#include <set>
#include <utility>
#include <regex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <filesystem>
#include <system_error>
#include <cstdlib>

#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

namespace ocli {

const std::vector<std::regex> DANGEROUS_PATTERNS = {
    std::regex("\\brm\\b"), std::regex("\\bmv\\b"), std::regex("\\bsudo\\b"),
    std::regex("\\bchmod\\b"), std::regex("\\bchown\\b"), std::regex("\\bdd\\b"),
    std::regex("\\bmkfs\\b"), std::regex("\\bformat\\b"), std::regex("\\bkill\\b"),
    std::regex(">\\s*/dev/"), std::regex("\\bshred\\b"), std::regex("\\bwipe\\b"),
    std::regex("\\brm\\s+-[rf]"), std::regex("\\bgit\\s+clean\\b"),
    std::regex("\\bgit\\s+reset\\s+--hard"),
    std::regex(":\\(\\)\\s*\\{"), std::regex("\\bfork\\b.*bomb"),
    std::regex("/dev/sd[a-z]"), std::regex("\\bmkfs\\."),
    std::regex("\\bcurl\\b[^|]*\\|\\s*(ba)?sh"),
    std::regex("\\bwget\\b[^|]*\\|\\s*(ba)?sh"),
    std::regex("\\bbase64\\b\\s+-{0,2}d"),
    std::regex(">\\s*/etc/"), std::regex(">\\s*~?/\\.(ssh|aws|config)"),
    std::regex("\\bcrontab\\b"), std::regex("\\blaunchctl\\b"),
    std::regex("\\bdiskutil\\b"), std::regex("\\bnvram\\b"), std::regex("\\bspctl\\b"),
    std::regex("\\bnc\\b\\s+-[a-z]*l"), std::regex("\\bnetcat\\b"),
    std::regex("\\bchflags\\b"), std::regex("\\bdefaults\\s+write"),
    std::regex("\\bkillall\\b"), std::regex("\\bpkill\\b"),
};

const std::vector<std::regex> CATASTROPHIC_PATTERNS = {
    std::regex("\\brm\\s+-[rf]{1,2}\\s+(/|~|\\$HOME|\\*)"),
    std::regex(":\\(\\)\\s*\\{.*\\}\\s*;"),
    std::regex("\\bmkfs\\b"),
    std::regex("\\bdd\\b.*of=/dev/"),
    std::regex(">\\s*/dev/(sd|disk|hd)"),
    std::regex("\\bdiskutil\\s+(erase|partition)"),
    std::regex("\\bgit\\s+reset\\s+--hard"),
};

const std::regex QUOTED_LITERAL("\"[^\"]*\"|'[^']*'");

const std::vector<std::regex> EVAL_EXEC_INVOKE = {
    std::regex("(?:^|[;&|`]|\\$\\()\\s*eval\\b"),
    std::regex("(?:^|[;&|`]|\\$\\()\\s*exec\\b"),
};

const std::vector<std::pair<std::regex, std::string>> OFFENSIVE_HARD_DENY = {
    {std::regex("/dev/tcp/", std::regex::icase), "reverse shell / raw TCP socket"},
    {std::regex("/dev/udp/", std::regex::icase), "reverse shell / raw UDP socket"},
    {std::regex("\\bnc\\b[^\\n]*\\s-[a-zA-Z]*e\\b", std::regex::icase), "netcat -e exec backdoor"},
    {std::regex("\\bncat\\b[^\\n]*--exec\\b", std::regex::icase), "ncat --exec backdoor"},
    {std::regex("\\bmkfifo\\b[^\\n]*\\|[^\\n]*\\b(nc|ncat|/bin/(ba)?sh)\\b", std::regex::icase), "named-pipe reverse shell"},
    {std::regex("\\bpty\\.spawn\\b", std::regex::icase), "interactive shell spawn (reverse-shell stage)"},
    {std::regex("socket\\.socket\\([^\\n]*\\)[^\\n]*connect\\(", std::regex::icase), "python raw-socket reverse shell"},
    {std::regex("os\\.dup2\\([^\\n]*s(ock)?\\b", std::regex::icase), "python socket->shell redirection (reverse shell)"},
    {std::regex("\\bperl\\b[^\\n]*-e[^\\n]*socket", std::regex::icase), "perl reverse shell"},
    {std::regex("\\bphp\\b[^\\n]*-r[^\\n]*fsockopen", std::regex::icase), "php reverse shell"},
    {std::regex("\\bruby\\b[^\\n]*-rsocket", std::regex::icase), "ruby reverse shell"},
    {std::regex("\\bbash\\b\\s+-i\\b[^\\n]*(>&|>|2>&1)", std::regex::icase), "interactive bash redirected to a socket"},
    {std::regex("\\bmimikatz\\b", std::regex::icase), "credential-dumping tool (mimikatz)"},
    {std::regex("\\bsekurlsa\\b", std::regex::icase), "LSASS credential extraction"},
    {std::regex("\\blsadump\\b", std::regex::icase), "LSA secrets dump"},
    {std::regex("\\bgsecdump\\b|\\bwce\\.exe\\b|\\bpwdump\\b|\\bfgdump\\b", std::regex::icase), "credential-dumping tool"},
    {std::regex("\\bsecretsdump(\\.py)?\\b", std::regex::icase), "remote credential dump (secretsdump)"},
    {std::regex("procdump[^\\n]*lsass|comsvcs[^\\n]*MiniDump|\\blsass\\.dmp\\b", std::regex::icase), "LSASS memory dump"},
    {std::regex("/etc/shadow\\b[^\\n]*(\\bnc\\b|\\bcurl\\b|\\bwget\\b|\\bscp\\b|base64)", std::regex::icase), "exfiltrate /etc/shadow"},
    {std::regex("\\bfind\\b[^\\n]*-exec[^\\n]*(openssl\\s+enc|gpg\\s+(-c|--encrypt|--symmetric))", std::regex::icase), "bulk file encryption (ransomware pattern)"},
    {std::regex("\\bfor\\b[^\\n]*(openssl\\s+enc|gpg\\s+-c)[^\\n]*\\bdone\\b", std::regex::icase), "loop bulk encryption (ransomware pattern)"},
    {std::regex("\\bwevtutil\\s+cl\\b|Clear-EventLog\\b|\\bauditpol\\b[^\\n]*/clear", std::regex::icase), "event-log clearing (anti-forensics)"},
    {std::regex(">\\s*/var/log/(auth|secure|syslog|messages|wtmp|btmp)", std::regex::icase), "overwrite system logs (anti-forensics)"},
    {std::regex("\\bshred\\b[^\\n]*/var/log", std::regex::icase), "shred system logs (anti-forensics)"},
    {std::regex("\\bsetenforce\\s+0\\b", std::regex::icase), "disable SELinux enforcement"},
    {std::regex("\\bsystemctl\\s+(stop|disable|mask)\\s+(firewalld|auditd|ufw|apparmor)", std::regex::icase), "disable host security service"},
    {std::regex("\\bufw\\s+disable\\b", std::regex::icase), "disable firewall"},
    {std::regex("\\bcsrutil\\s+disable\\b|\\bspctl\\s+--master-disable\\b", std::regex::icase), "disable macOS SIP/Gatekeeper"},
    {std::regex("Set-MpPreference[^\\n]*Disable\\w*Monitoring[^\\n]*\\$true", std::regex::icase), "disable Microsoft Defender"},
    {std::regex("\\bnetsh\\s+advfirewall\\s+set[^\\n]*\\boff\\b", std::regex::icase), "disable Windows firewall"},
    {std::regex("(curl|wget)\\b[^\\n]*(-d\\b|--data|--upload-file|-T\\b)[^\\n]*/etc/(passwd|shadow)", std::regex::icase), "exfiltrate system credential file"},
    {std::regex("\\bscp\\b[^\\n]*/etc/(passwd|shadow)\\b", std::regex::icase), "exfiltrate system credential file"},
    {std::regex(">>?\\s*[^\\n]*authorized_keys", std::regex::icase), "append attacker SSH key (backdoor)"},
    {std::regex("echo[^\\n]*ssh-(rsa|ed25519|dss)[^\\n]*authorized_keys", std::regex::icase), "inject SSH key (backdoor)"},
    {std::regex("\\bmasscan\\b|\\bzmap\\b", std::regex::icase), "internet-scale mass scanner"},
    {std::regex("\\bnmap\\b[^\\n]*(\\b0\\.0\\.0\\.0/0\\b|/[0-9]\\b)", std::regex::icase), "mass-range network scan (/0–/9)"},
    {std::regex("\\bhping3\\b[^\\n]*(--flood|--rand-source)|\\bslowloris\\b|\\bgoldeneye\\b|\\bt50\\b", std::regex::icase), "denial-of-service flooding"},
};

const std::vector<std::pair<std::regex, std::string>> OFFENSIVE_AUTHORIZE = {
    {std::regex("\\bnmap\\b|\\brustscan\\b|\\bnaabu\\b|\\bnbtscan\\b", std::regex::icase), "active network/port scanning"},
    {std::regex("\\bnuclei\\b|\\bnikto\\b|\\bwpscan\\b|\\bwhatweb\\b", std::regex::icase), "active web vulnerability scanning"},
    {std::regex("\\bgobuster\\b|\\bffuf\\b|\\bferoxbuster\\b|\\bdirb\\b|\\bdirbuster\\b|\\bwfuzz\\b", std::regex::icase), "web content/dir brute-forcing"},
    {std::regex("\\benum4linux\\b|\\bsmbmap\\b|\\bsnmpwalk\\b|\\bnbtscan\\b", std::regex::icase), "service enumeration"},
    {std::regex("\\bdnsrecon\\b|\\bfierce\\b|\\bamass\\b|\\bsublist3r\\b|\\btheharvester\\b", std::regex::icase), "active recon/OSINT enumeration"},
    {std::regex("\\bmsf(console|venom|db)\\b|\\bmetasploit\\b|\\bmeterpreter\\b", std::regex::icase), "exploitation framework (Metasploit)"},
    {std::regex("\\bcobalt\\s*strike\\b|\\bsliver\\b|\\bpowershell\\s+empire\\b|\\bbeef-xss\\b|\\bmythic\\b", std::regex::icase), "command-and-control framework"},
    {std::regex("\\bveil\\b|\\bshellter\\b|\\bdonut\\b", std::regex::icase), "payload/shellcode generator"},
    {std::regex("\\bhydra\\s+-[a-zA-Z]|\\bmedusa\\s+-[a-zA-Z]|\\bncrack\\b|\\bcrowbar\\b", std::regex::icase), "credential brute-forcing"},
    {std::regex("\\bpatator\\b\\s+\\w+_\\w+|\\bkerbrute\\b", std::regex::icase), "credential brute-forcing"},
    {std::regex("\\bhashcat\\s+-|\\bjohn\\s+(--[\\w-]+|\\S+\\.(hash|txt|lst|pot))|\\bjohnny\\b", std::regex::icase), "offline password cracking"},
    {std::regex("\\bimpacket(-\\w+)?\\b|\\bcrackmapexec\\b|\\bnetexec\\b|\\bnxc\\b", std::regex::icase), "AD attack / lateral movement tooling"},
    {std::regex("\\bresponder\\b|\\bevil-winrm\\b|\\bbloodhound\\b|\\bsharphound\\b|\\brubeus\\b", std::regex::icase), "AD attack / credential relay tooling"},
    {std::regex("\\bpsexec(\\.py)?\\b|\\bwmiexec(\\.py)?\\b|\\bsmbexec(\\.py)?\\b|\\batexec(\\.py)?\\b", std::regex::icase), "remote code execution / lateral movement"},
    {std::regex("\\bsqlmap\\b|\\bxsstrike\\b|\\bcommix\\b", std::regex::icase), "automated web exploitation"},
    {std::regex("\\bbettercap\\b|\\bettercap\\b|\\barpspoof\\b|\\bmitmdump\\b", std::regex::icase), "man-in-the-middle tooling"},
    {std::regex("\\baircrack-ng\\b|\\bairodump-ng\\b|\\bairmon-ng\\b|\\breaver\\b|\\bwifite\\b", std::regex::icase), "wireless attack tooling"},
    {std::regex("\\bnc\\b\\s+-[a-zA-Z]*l|\\bncat\\b[^\\n]*-[a-zA-Z]*l|\\bsocat\\b[^\\n]*LISTEN", std::regex::icase), "raw listener / bind shell"},
    {std::regex("(curl|wget)\\b[^|]*\\|\\s*(sudo\\s+)?(ba)?sh\\b", std::regex::icase), "pipe remote script to a shell"},
};

const std::set<std::string> SENSITIVE_PATH_PARTS = {
    ".ssh", ".aws", ".gnupg", ".kube", ".docker", ".netrc",
    "id_rsa", "id_ed25519", "id_ecdsa", ".env", "credentials",
    ".git-credentials", ".npmrc", ".pypirc", "secrets",
};

const std::vector<std::pair<std::regex, std::string>> SENSITIVE_WRITE_PATHS = {
    {std::regex("/etc/(passwd|shadow|sudoers)", std::regex::icase), "system credential / sudoers file"},
    {std::regex("authorized_keys$", std::regex::icase), "SSH authorized_keys (backdoor risk)"},
    {std::regex("/(etc/cron|var/spool/cron|Library/Launch(Daemons|Agents))", std::regex::icase), "system persistence location"},
    {std::regex("/etc/(rc\\.local|systemd/system|init\\.d)", std::regex::icase), "init / persistence location"},
    {std::regex("(\\\\|/)(System32|SysWOW64)(\\\\|/)", std::regex::icase), "Windows system directory"},
    {std::regex("\\\\(Run|RunOnce)\\\\?$", std::regex::icase), "Windows Run-key persistence"},
};

namespace {

std::string ascii_lower(const std::string& s) {
    std::string o = s;
    for (char& c : o)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return o;
}

std::vector<std::string> py_split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

std::string basename_of(const std::string& s) {
    auto pos = s.find_last_of('/');
    return pos == std::string::npos ? s : s.substr(pos + 1);
}

std::string getcwd_str() {
    char buf[4096];
    if (::getcwd(buf, sizeof buf)) return std::string(buf);
    return std::string();
}

std::string json_escape_py(const std::string& s) {
    std::u32string cp = utf8_to_u32(s);
    std::string out;
    out.reserve(cp.size() + 2);
    char buf[16];
    for (char32_t c : cp) {
        switch (c) {
            case U'"':  out += "\\\""; break;
            case U'\\': out += "\\\\"; break;
            case U'\b': out += "\\b";  break;
            case U'\f': out += "\\f";  break;
            case U'\n': out += "\\n";  break;
            case U'\r': out += "\\r";  break;
            case U'\t': out += "\\t";  break;
            default:
                if (c < 0x20 || c >= 0x80) {
                    if (c > 0xFFFF) {
                        char32_t v = c - 0x10000;
                        unsigned hi = 0xD800u + static_cast<unsigned>(v >> 10);
                        unsigned lo = 0xDC00u + static_cast<unsigned>(v & 0x3FF);
                        std::snprintf(buf, sizeof buf, "\\u%04x\\u%04x", hi, lo);
                        out += buf;
                    } else {
                        std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c));
                        out += buf;
                    }
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

struct IpFlags {
    bool is_private = false, is_loopback = false, is_link_local = false,
         is_multicast = false, is_reserved = false, is_unspecified = false;
};

bool v4_in(uint32_t ip_h, uint32_t base_h, int prefix) {
    if (prefix <= 0) return true;
    uint32_t mask = (prefix >= 32) ? 0xFFFFFFFFu : (0xFFFFFFFFu << (32 - prefix));
    return (ip_h & mask) == (base_h & mask);
}
uint32_t v4_base(const char* s) {
    struct in_addr a{};
    inet_pton(AF_INET, s, &a);
    return ntohl(a.s_addr);
}
bool v4_in_net(uint32_t ip_h, const char* net, int prefix) {
    return v4_in(ip_h, v4_base(net), prefix);
}

bool v4_is_private(uint32_t ip_h) {
    static const struct { const char* net; int p; } nets[] = {
        {"0.0.0.0", 8}, {"10.0.0.0", 8}, {"127.0.0.0", 8}, {"169.254.0.0", 16},
        {"172.16.0.0", 12}, {"192.0.0.0", 29}, {"192.0.0.170", 31}, {"192.0.2.0", 24},
        {"192.168.0.0", 16}, {"198.18.0.0", 15}, {"198.51.100.0", 24}, {"203.0.113.0", 24},
        {"240.0.0.0", 4}, {"255.255.255.255", 32},
    };
    for (const auto& n : nets)
        if (v4_in_net(ip_h, n.net, n.p)) return true;
    return false;
}

IpFlags classify_v4(uint32_t ip_h) {
    IpFlags f;
    f.is_private     = v4_is_private(ip_h);
    f.is_loopback    = v4_in_net(ip_h, "127.0.0.0", 8);
    f.is_link_local  = v4_in_net(ip_h, "169.254.0.0", 16);
    f.is_multicast   = v4_in_net(ip_h, "224.0.0.0", 4);
    f.is_reserved    = v4_in_net(ip_h, "240.0.0.0", 4);
    f.is_unspecified = (ip_h == 0);
    return f;
}

bool v6_in(const uint8_t ip[16], const uint8_t base[16], int prefix) {
    int full = prefix / 8, rem = prefix % 8;
    for (int i = 0; i < full; ++i)
        if (ip[i] != base[i]) return false;
    if (rem) {
        uint8_t m = static_cast<uint8_t>(0xFF << (8 - rem));
        if ((ip[full] & m) != (base[full] & m)) return false;
    }
    return true;
}
bool v6_in_net(const uint8_t ip[16], const char* net, int prefix) {
    uint8_t base[16];
    inet_pton(AF_INET6, net, base);
    return v6_in(ip, base, prefix);
}

bool v6_is_v4mapped(const uint8_t b[16], uint32_t& out_h) {
    for (int i = 0; i < 10; ++i)
        if (b[i] != 0) return false;
    if (b[10] != 0xff || b[11] != 0xff) return false;
    out_h = (static_cast<uint32_t>(b[12]) << 24) | (static_cast<uint32_t>(b[13]) << 16) |
            (static_cast<uint32_t>(b[14]) << 8)  |  static_cast<uint32_t>(b[15]);
    return true;
}

bool v6_is_private(const uint8_t b[16]) {
    static const struct { const char* net; int p; } nets[] = {
        {"::1", 128}, {"::", 128}, {"::ffff:0:0", 96}, {"100::", 64},
        {"2001::", 23}, {"2001:2::", 48}, {"2001:db8::", 32}, {"2001:10::", 28},
        {"fc00::", 7}, {"fe80::", 10},
    };
    for (const auto& n : nets)
        if (v6_in_net(b, n.net, n.p)) return true;
    return false;
}

bool v6_is_reserved(const uint8_t b[16]) {
    static const struct { const char* net; int p; } nets[] = {
        {"::", 8}, {"100::", 8}, {"200::", 7}, {"400::", 6}, {"800::", 5},
        {"1000::", 4}, {"4000::", 3}, {"6000::", 3}, {"8000::", 3}, {"A000::", 3},
        {"C000::", 3}, {"E000::", 4}, {"F000::", 5}, {"F800::", 6}, {"FE00::", 9},
    };
    for (const auto& n : nets)
        if (v6_in_net(b, n.net, n.p)) return true;
    return false;
}

IpFlags classify_v6(const uint8_t b[16]) {
    IpFlags f;
    f.is_private    = v6_is_private(b);
    f.is_loopback   = v6_in_net(b, "::1", 128);
    f.is_link_local = v6_in_net(b, "fe80::", 10);
    f.is_multicast  = v6_in_net(b, "ff00::", 8);
    f.is_reserved   = v6_is_reserved(b);
    bool zero = true;
    for (int i = 0; i < 16; ++i) if (b[i] != 0) { zero = false; break; }
    f.is_unspecified = zero;
    return f;
}

struct AddrInspect {
    bool        parseable = false;
    std::string addr;
    IpFlags     flags;
};
AddrInspect inspect_addr(const struct addrinfo* ai) {
    AddrInspect r;
    char buf[INET6_ADDRSTRLEN] = {0};
    if (ai->ai_family == AF_INET) {
        const auto* sin = reinterpret_cast<const struct sockaddr_in*>(ai->ai_addr);
        if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof buf)) r.addr = buf;
        r.flags = classify_v4(ntohl(sin->sin_addr.s_addr));
        r.parseable = true;
    } else if (ai->ai_family == AF_INET6) {
        const auto* sin6 = reinterpret_cast<const struct sockaddr_in6*>(ai->ai_addr);
        if (inet_ntop(AF_INET6, &sin6->sin6_addr, buf, sizeof buf)) r.addr = buf;
        uint32_t v4;
        if (v6_is_v4mapped(sin6->sin6_addr.s6_addr, v4)) r.flags = classify_v4(v4);
        else r.flags = classify_v6(sin6->sin6_addr.s6_addr);
        r.parseable = true;
    }
    return r;
}

bool resolve(const std::string& host, int port, struct addrinfo** out) {
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof hints);
    hints.ai_protocol = IPPROTO_TCP;
    char portbuf[16];
    std::snprintf(portbuf, sizeof portbuf, "%d", port);
    *out = nullptr;
    return getaddrinfo(host.c_str(), portbuf, &hints, out) == 0;
}

}

std::pair<bool, std::string> url_is_safe(const std::string& url) {
    ParsedUrl parsed = urlparse(url);
    if (!parsed.valid)
        return {false, "Malformed URL."};
    if (parsed.scheme != "http" && parsed.scheme != "https")
        return {false, "Only http/https URLs are allowed (got '" +
                       (parsed.scheme.empty() ? std::string("none") : parsed.scheme) + "')."};
    const std::string& host = parsed.host;
    if (host.empty())
        return {false, "URL has no host."};
    if (!parsed.user.empty() || !parsed.password.empty())
        return {false, "URLs with embedded credentials are not allowed."};

    int port = (parsed.port > 0) ? parsed.port : (parsed.scheme == "https" ? 443 : 80);
    struct addrinfo* res = nullptr;
    if (!resolve(host, port, &res))
        return {false, "Could not resolve host '" + host + "'."};

    for (struct addrinfo* ai = res; ai; ai = ai->ai_next) {
        AddrInspect r = inspect_addr(ai);
        if (!r.parseable) {
            freeaddrinfo(res);
            return {false, "Unresolvable address for '" + host + "'."};
        }
        const IpFlags& f = r.flags;
        if (f.is_private || f.is_loopback || f.is_link_local ||
            f.is_multicast || f.is_reserved || f.is_unspecified) {
            freeaddrinfo(res);
            return {false, "Refusing to fetch a private/loopback/link-local address (" + r.addr + ")."};
        }
    }
    freeaddrinfo(res);
    return {true, ""};
}

std::pair<bool, std::string> pentest_url_ok(const std::string& url) {
    ParsedUrl parsed = urlparse(url);
    if (!parsed.valid)
        return {false, "Malformed URL."};
    if (parsed.scheme != "http" && parsed.scheme != "https")
        return {false, "Only http/https URLs are allowed (got '" +
                       (parsed.scheme.empty() ? std::string("none") : parsed.scheme) + "')."};
    const std::string& host = parsed.host;
    if (host.empty())
        return {false, "URL has no host."};

    int port = (parsed.port > 0) ? parsed.port : (parsed.scheme == "https" ? 443 : 80);
    struct addrinfo* res = nullptr;
    if (!resolve(host, port, &res))
        return {false, "Could not resolve host '" + host + "'."};

    const char* envp = std::getenv("OCLI_HTTP_ALLOW_PUBLIC");
    bool allow_public = (envp && std::string(envp) == "1");

    for (struct addrinfo* ai = res; ai; ai = ai->ai_next) {
        AddrInspect r = inspect_addr(ai);
        if (!r.parseable) {
            freeaddrinfo(res);
            return {false, "Unresolvable address for '" + host + "'."};
        }
        const IpFlags& f = r.flags;
        if (f.is_link_local || f.is_multicast || f.is_reserved || f.is_unspecified) {
            freeaddrinfo(res);
            return {false, "Blocked address (" + r.addr + "): link-local/metadata, multicast and reserved "
                           "ranges are never allowed."};
        }
        if (!(f.is_loopback || f.is_private) && !allow_public) {
            freeaddrinfo(res);
            return {false, "'" + host + "' resolves to a public address (" + r.addr + "). This pentest client only "
                           "hits loopback/LAN by default. Export OCLI_HTTP_ALLOW_PUBLIC=1 to allow "
                           "public hosts (only for targets you are authorized to test)."};
        }
    }
    freeaddrinfo(res);
    return {true, ""};
}

std::string classify_command(const std::string& command) {
    const std::string& cmd = command;
    for (const auto& p : CATASTROPHIC_PATTERNS)
        if (std::regex_search(cmd, p)) return "catastrophic";
    for (const auto& p : DANGEROUS_PATTERNS)
        if (std::regex_search(cmd, p)) return "dangerous";

    std::string unquoted = std::regex_replace(cmd, QUOTED_LITERAL, std::string(" "));
    for (const auto& p : EVAL_EXEC_INVOKE)
        if (std::regex_search(unquoted, p)) return "dangerous";
    return "normal";
}

std::pair<std::string, std::string> classify_offensive(const std::string& text) {
    const std::string& t = text;
    for (const auto& pr : OFFENSIVE_HARD_DENY)
        if (std::regex_search(t, pr.first)) return {"deny", pr.second};
    for (const auto& pr : OFFENSIVE_AUTHORIZE)
        if (std::regex_search(t, pr.first)) return {"authorize", pr.second};
    return {"allow", ""};
}

void security_audit(const std::string& decision, const std::string& tool,
                    const std::string& reason, const std::string& detail) {
    try {
        std::string d = expanduser("~/.lorea");
        std::error_code ec;
        std::filesystem::create_directories(d, ec);

        std::time_t t = std::time(nullptr);
        std::tm tmv{};
        localtime_r(&t, &tmv);
        char tsbuf[32];
        std::strftime(tsbuf, sizeof tsbuf, "%Y-%m-%dT%H:%M:%S", &tmv);

        std::string det = utf8_substr(detail, 0, 600);

        std::string line = "{";
        line += "\"ts\": \"" + json_escape_py(tsbuf) + "\", ";
        line += "\"decision\": \"" + json_escape_py(decision) + "\", ";
        line += "\"tool\": \"" + json_escape_py(tool) + "\", ";
        line += "\"reason\": \"" + json_escape_py(reason) + "\", ";
        line += "\"detail\": \"" + json_escape_py(det) + "\"";
        line += "}";

        std::string fp = (std::filesystem::path(d) / "security-audit.log").string();
        std::ofstream f(fp, std::ios::app);
        if (f) f << line << "\n";
    } catch (...) {

    }
}

PathSafety check_path_safety(const std::string& path, bool for_write) {
    PathSafety ps;
    std::string abspath;
    try {
        abspath = realpath_str(expanduser(path));
    } catch (...) {
        ps.ok = false; ps.reason = "Malformed path."; ps.has_abspath = false;
        return ps;
    }

    std::string low = ascii_lower(abspath);
    std::set<std::string> parts;
    for (const auto& comp : py_split(abspath, '/'))
        parts.insert(ascii_lower(comp));
    std::string base = basename_of(low);

    for (const auto& s : SENSITIVE_PATH_PARTS) {
        if (parts.count(s) || s == base) {
            ps.ok = false;
            ps.reason = "That path looks like a secret/credential file; refusing.";
            ps.abspath = abspath; ps.has_abspath = true;
            return ps;
        }
    }

    std::string cwd = realpath_str(getcwd_str());
    bool inside = (abspath == cwd) || (abspath.rfind(cwd + "/", 0) == 0);
    if (for_write && !inside) {
        ps.ok = false;
        ps.reason = "Refusing to write outside the current working directory.";
        ps.abspath = abspath; ps.has_abspath = true;
        return ps;
    }

    ps.ok = true; ps.reason = ""; ps.abspath = abspath; ps.has_abspath = true;
    return ps;
}

std::vector<std::string> extract_allowed_domains(const std::string& text) {
    std::vector<std::string> found;
    std::string lowered = ascii_lower(text);
    for (const auto& domain : ALLOWED_SEARCH_DOMAINS)
        if (lowered.find(domain) != std::string::npos) found.push_back(domain);
    return found;
}

bool should_search_official_domains(const std::string& query) {
    std::string lowered = ascii_lower(query);
    static const char* official_words[] = {"official", "source", "vendor", "docs", "documentation"};
    static const char* known_products[] = {"ollama", "gemma", "google", "huggingface", "hugging face"};
    bool any_word = false;
    for (const char* w : official_words)
        if (lowered.find(w) != std::string::npos) { any_word = true; break; }
    if (!any_word) return false;
    for (const char* p : known_products)
        if (lowered.find(p) != std::string::npos) return true;
    return false;
}

bool domain_matches(const std::string& url, const std::vector<std::string>& allowed_domains) {
    if (allowed_domains.empty()) return true;
    std::string lowered = ascii_lower(url);
    for (const auto& domain : allowed_domains)
        if (lowered.find(domain) != std::string::npos) return true;
    return false;
}

}
