#pragma once

#include <string>
#include <vector>
#include <map>
#include <set>
#include <optional>
#include <utility>
#include <memory>
#include <regex>
#include <sys/types.h>

#include "types.hpp"

namespace ocli {

const std::string&         ensure_sessions_dir();
std::string                resolve_session_path(const std::string& name);
std::vector<SessionInfo>   list_saved_sessions();
int                        estimate_tokens(const std::string& text);
std::string                head_tail_trim(const std::string& text, size_t limit,
                                          const std::string& marker = " ... [trimmed] ... ");
std::string                truncate_output(const std::string& output, size_t limit = MAX_OUTPUT_LENGTH);

std::pair<bool, std::string> url_is_safe(const std::string& url);
std::pair<bool, std::string> pentest_url_ok(const std::string& url);

std::string classify_command(const std::string& command);
std::pair<std::string, std::string> classify_offensive(const std::string& text);
void        security_audit(const std::string& decision, const std::string& tool,
                           const std::string& reason, const std::string& detail);
PathSafety  check_path_safety(const std::string& path, bool for_write = false);

std::vector<std::string>   extract_allowed_domains(const std::string& text);
bool                       should_search_official_domains(const std::string& query);
bool                       domain_matches(const std::string& url, const std::vector<std::string>& allowed_domains);
bool                       launcher_matches_source(const std::string& path, const std::string& source);
std::optional<std::string> command_on_path(const std::string& name);
bool                       installed_via_package();
void                       install_cli_launcher();
bool                       model_matches_backend(const std::string& model, const std::string& backend);
bool                       is_large_mlx_model(const std::string& model);

bool       is_apple_silicon();
long       mac_total_ram_mb();
long       mac_current_wired_limit_mb();
VramBounds mac_vram_bounds(long total_mb);
long       mac_recommended_wired_limit_mb(long total_mb);

std::vector<std::string> shlex_split(const std::string& s);
std::string              shlex_quote(const std::string& s);

struct ParsedUrl {
    std::string scheme, host, user, password, path, query;
    int  port  = -1;
    bool valid = false;
};
ParsedUrl   urlparse(const std::string& url);
std::string urljoin(const std::string& base, const std::string& ref);

std::string expanduser(const std::string& path);
std::string realpath_str(const std::string& path);

double difflib_ratio(const std::string& a, const std::string& b);

struct ProcResult {
    int         exit_code = -1;
    std::string out;
    std::string err;
    bool        timed_out = false;
    bool        started   = false;
};
ProcResult run_subprocess(const std::vector<std::string>& argv,
                          const std::string& input = "", double timeout_s = 0.0,
                          bool combine_stderr = false);

ProcResult run_shell(const std::string& command, double timeout_s = 0.0);

class Subprocess {
public:
    pid_t pid = -1;
    int   stdin_fd  = -1;
    int   stdout_fd = -1;
    int   master_fd = -1;

    std::optional<int> poll();
    int  wait(double timeout_s = -1);
    void terminate();
    void kill();
    bool alive();
};

std::shared_ptr<Subprocess> spawn_process(const std::vector<std::string>& argv,
                                          bool devnull_output = true);

extern const std::vector<std::regex>                      DANGEROUS_PATTERNS;
extern const std::vector<std::regex>                      CATASTROPHIC_PATTERNS;
extern const std::regex                                   QUOTED_LITERAL;
extern const std::vector<std::regex>                      EVAL_EXEC_INVOKE;
extern const std::vector<std::pair<std::regex,std::string>> OFFENSIVE_HARD_DENY;
extern const std::vector<std::pair<std::regex,std::string>> OFFENSIVE_AUTHORIZE;
extern const std::set<std::string>                        SENSITIVE_PATH_PARTS;
extern const std::vector<std::pair<std::regex,std::string>> SENSITIVE_WRITE_PATHS;

}
