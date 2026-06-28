#pragma once

#include <string>
#include <vector>
#include <map>
#include <set>
#include <deque>
#include <optional>
#include <functional>
#include <memory>
#include <tuple>
#include <utility>
#include <exception>

#include "types.hpp"
#include "ansi.hpp"
#include "render.hpp"
#include "terminal.hpp"
#include "widgets.hpp"
#include "interrupt.hpp"
#include "secutil.hpp"
#include "toolparse.hpp"
#include "http.hpp"

namespace ocli {

class LOREA {
public:

    LOREA(std::string model_name, bool auto_mode = false,
          std::string backend = "ollama", std::optional<std::string> url = std::nullopt);
    ~LOREA();
    void cleanup();

    std::optional<int>         menu_choice(const std::string& title,
                                           const std::vector<std::string>& options);
    std::optional<std::string> prompt_value(const std::string& label,
                                            std::optional<std::string> current = std::nullopt);

    ConnectOpts                parse_connect_args(const std::string& arg_text);
    std::optional<std::string> normalize_mpc_url(const std::string& url);
    std::map<std::string, std::string> mpc_headers();

    json  mpc_request(const std::string& method, const std::string& path,
                      const json* json_body = nullptr,
                      const std::map<std::string, std::string>* params = nullptr,
                      const std::map<std::string, std::string>* extra_headers = nullptr,
                      long timeout_s = MPC_REQUEST_TIMEOUT);
    void  connect_mpc_command(const std::string& arg_text = "");
    void  disconnect_mpc();
    bool  mpc_supports(const std::string& feature);
    void  print_mpc_status(const json* status = nullptr);
    void  print_mpc_downloads();
    void  mpc_control_menu();
    void  delete_mpc_model_menu();
    std::optional<std::string> choose_mpc_backend();
    json  mpc_model_catalog(const std::string& backend);
    bool  select_mpc_model(const std::string& backend, const std::string& model_name);
    void  sync_mpc_selection(const json& status);
    std::tuple<std::string, std::vector<ToolCall>, json>
          mpc_chat(const json& messages, const json& tools);
    std::tuple<std::string, std::vector<ToolCall>, json>
          mpc_chat_stream(const json& messages, const json& tools,
                          std::function<void(const std::string&)> on_token = nullptr,
                          std::function<bool()> should_stop = nullptr);
    void  apply_mpc_selection(const json& data);
    void  start_mpc_download(const std::string& backend, const std::string& model_name,
                             std::optional<std::string> url = std::nullopt,
                             std::optional<std::string> download_dir = std::nullopt);
    bool  cancel_mpc_download(const std::string& job_id);
    void  wait_for_mpc_download(const std::string& job_id);
    void  download_mpc_model_menu(std::optional<std::string> model_name = std::nullopt,
                                  std::optional<std::string> download_dir = std::nullopt);

    std::optional<std::string> api_key(const std::string& backend, bool prompt_if_missing = false);
    std::string                cloud_base();
    std::optional<std::map<std::string, std::string>>
                               anthropic_auth_headers(bool prompt_if_missing = false);
    std::pair<std::string, json> anthropic_messages();
    static json                to_anthropic_tools(const json& tools);
    void  set_backend(const std::string& backend, std::optional<std::string> url = std::nullopt,
                      bool keep_model = false);
    void  backend_menu();
    std::vector<std::string> list_ollama_models();
    void  model_menu();
    void  vram_command(const std::string& arg_text = "");
    void  loop_command(const std::string& goal);

    std::pair<std::optional<std::string>, std::optional<std::string>>
          parse_download_args(const std::string& arg_text);
    std::string                model_download_default_dir(const std::string& model_name = "",
                                                          const std::string& url = "");
    std::optional<std::string> normalize_download_dir(const std::string& path);
    std::optional<std::string> prompt_download_dir(const std::string& model_name = "",
                                                   const std::string& url = "");
    std::optional<std::string> run_model_download(const std::string& model_name,
                                                  std::optional<std::string> url = std::nullopt,
                                                  std::optional<std::string> download_dir = std::nullopt,
                                                  bool ask_for_path = true);
    void  download_model_menu(std::optional<std::string> model_name = std::nullopt,
                              std::optional<std::string> download_dir = std::nullopt);

    bool  is_port_open(const std::string& host, int port);
    bool  ensure_lorea();
    void  ensure_mlx_server();
    std::optional<std::string> find_llama_server_bin();
    void  ensure_llamacpp_server();
    bool  llamacpp_health_ok(const std::string& host, int port);
    void  ensure_local_server();
    std::pair<std::string, int> inference_hostport();
    void  kill_inference_procs_on_port(int port);
    bool  wait_port_closed(const std::string& host, int port, double timeout = 12.0);
    void  restart_inference_server();
    void  save_restart_reload();
    std::optional<std::string> server_crash_note(const std::exception& exc);

    bool  authorized_engagement_ok(const std::string& name, const std::string& text,
                                   const std::string& reason);
    std::pair<bool, std::optional<std::string>>
          offensive_gate(const std::string& name, const json& args);

    std::string run_cmd(const std::string& command);
    std::string test_cmd(const std::string& command);
    std::string download_mlx_model(const std::string& repo_id, const std::string& download_dir = "");
    std::string send_input(const std::string& text);
    std::string list_files(const std::string& path = ".");
    std::string search_files(const std::string& query, const std::string& path = ".");
    std::string grep(const std::string& pattern, const std::string& path = ".");
    std::string git_status();
    std::string git_diff(const std::string& path = "");
    std::string web_search(const std::string& query, int num_results = 20);
    std::string read_url(const std::string& url);
    std::string http_request(const std::string& url, const std::string& method = "GET",
                             const json& data = json(), const json& headers = json(),
                             const json& params = json(), const json& json_body = json(),
                             bool follow_redirects = true, const json& cookies = json());
    std::string read_file(const std::string& path);
    std::string write_file(const std::string& path, const std::string& content);

    std::vector<Task> parse_plan_tasks(const std::string& plan);
    void        print_tasks();
    std::string create_plan(const std::string& plan);
    std::string update_task(const json& index, const std::string& status);

    json        spawn_agent_chat(const json& agent, const std::string& shared_context = "",
                                 int timeout_seconds = 120, int max_steps = 3,
                                 const std::string& tool_access = "read_only");
    std::string spawn_agents(const json& agents, const std::string& shared_context = "",
                             int timeout_seconds = 120, int max_steps = 3,
                             const std::string& tool_access = "read_only");
    AgentOptions parse_agent_args(const std::string& arg_text);
    std::vector<json> build_agent_team(const std::string& goal, int count);
    std::string agent_completion_once(const std::vector<Message>& messages,
                                      double temperature = 0.2, int max_tokens = 1800);
    std::string coordinate_agent_results(const std::string& goal, const std::vector<json>& results);
    void        agent_command(const std::string& arg_text = "");

    std::string setup_llama_cpp();
    std::string setup_mlx();

    std::string                save_session(const std::string& path = "");
    std::optional<std::string> session_menu();
    std::string                load_session(const std::string& path = "");
    void                       print_sessions();
    void                       display_metrics(const json& response);

    std::string        compact_message_text(const Message& message);
    std::optional<std::string> objective_text();
    std::string        compact_source_text(const std::vector<Message>& messages);
    std::vector<Message> recent_context_slice();
    std::vector<Message> trim_recent_context(const std::vector<Message>& messages);
    int                estimate_context_tokens();
    std::string        norm_tool_sig(const std::string& name, const json& args);
    std::string        fallback_compaction_summary(const std::vector<Message>& messages);
    std::string        request_compaction_summary(const std::vector<Message>& messages,
                                                  const std::string& prior_summary = "");
    bool               effort_deep();
    std::vector<std::string> extract_referenced_files();
    std::vector<std::string> model_output_log(const std::vector<Message>& messages);
    void               persist_outputs(const std::vector<std::string>& outs);
    void               compact_history();

    std::string        stall_decision(const std::string& content, int attempt);
    int                prune_repeated_assistant(const std::string& sig);
    std::string        tool_reminder_text();
    bool               has_open_plan();
    bool               in_working_context();
    std::optional<Message> tool_reminder_message();
    std::optional<std::string> plan_state_text();
    std::optional<Message> plan_state_message();
    std::optional<Message> effort_message();
    std::optional<Message> advr_reminder_message();
    std::vector<Message>   ephemeral_reminders();
    std::vector<Message>   server_messages();
    std::string            latest_tool_result_text();
    std::optional<Message> recent_tool_results_message(int limit = 10, int per_result_chars = 600);
    std::string            last_assistant_text();

    void  copy_last_answer();
    bool  retry_last_turn();
    void  undo_last_write();
    void  run_oneoff_shell(const std::string& command);
    void  show_working_diff(const std::string& path = "");
    void  print_usage();
    void  theme_command(const std::string& name);
    void  goodbye();
    std::vector<std::string> welcome_lines(const std::string& phrase);

    void  run();
    bool  process_chat();

    json        build_tools_schema();
    bool        tool_available(const std::string& name);
    std::string invoke_tool(const std::string& name, const json& args);

    std::string                 model_name;
    bool                        auto_mode = false;
    std::string                 backend;
    std::string                 url;
    bool                        planning_enabled = false;
    std::string                 effort_level = "basic";
    std::vector<Task>           tasks;
    std::shared_ptr<Subprocess> active_process;
    int                         active_master = -1;
    std::optional<std::string>  last_tool_signature;
    int                         repeated_tool_count = 0;
    std::set<std::string>       turn_call_norms;
    std::string                 last_user_goal;
    std::optional<std::string>  last_failure_signature;
    int                         repeated_failure_count = 0;
    int                         tool_steps_this_turn = 0;
    int                         compaction_count = 0;
    std::string                 last_summary;
    double                      session_started_at = 0.0;
    int                         session_tools_run = 0;
    int                         session_turns = 0;
    std::set<std::string>       session_files_touched;
    std::vector<UndoEntry>      undo_stack;
    std::vector<std::string>    prompt_history;
    std::optional<std::string>  server_model;
    std::optional<std::string>  mpc_url;
    std::optional<std::string>  mpc_token;
    json                        mpc_features = json::object();
    std::optional<std::string>  mpc_version;
    std::string                 tool_access = "full";
    bool                        allow_spawn_agents = true;
    bool                        non_interactive = false;
    std::string                 PLAN_PROMPT;
    std::vector<Message>        messages;
    std::shared_ptr<Subprocess> server_process;

    bool                        airllm_model = false;
    std::optional<std::string>  airllm_compression;
    int                         airllm_max_length = 4096;
    int                         airllm_max_new_tokens = 2048;
    std::map<std::string, std::string> api_keys;
    std::shared_ptr<HttpClient> http_session;

    std::string                 stall_last_sig;
    std::string                 stuck_last_response;
    std::deque<std::string>     recent_answer_sigs;
    std::map<std::string, SigCacheEntry> turn_sig_cache;

    static const std::string    LOOP_SENTINEL;
    static const int            LOOP_MAX_ITERATIONS;

private:
    bool cleaned_up_ = false;
};

int run_spawn_agent_worker();
int run_main(const std::vector<std::string>& argv);

}
