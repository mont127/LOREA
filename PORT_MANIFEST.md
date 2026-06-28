# OCLI.py -> C++ Port Manifest

Faithful, complete port of `/Users/teoballesteros/OCLI.py/OCLI.py` (7863 lines,
`class LOREA`) to C++20 + libcurl + nlohmann::json (`third_party/json.hpp`, 3.12.0).

Everything lives in `namespace ocli`. Conversation records (`Message`, `ToolCall`) are
aliases of `nlohmann::json` (see the rationale in `include/types.hpp`): this is the only
representation that faithfully preserves heterogeneous dicts, polymorphic
`function.arguments` (object OR raw string), and arbitrary extra keys for session
save/load. Value-typed records (Task, EffortLevel, MouseEvent, …) are real structs.

## Headers (include/)

| Header          | Owns (declares)                                                                                  | Python lines |
|-----------------|--------------------------------------------------------------------------------------------------|--------------|
| `types.hpp`     | json aliases, enums, value structs, exceptions, scalar consts, config-table externs, EFFORT      | shared       |
| `ansi.hpp`      | `Colors`, ACCENT/MUTED, FLAIR_RAMP, UTF-8 helpers, width/box/frame/panel/gradient primitives      | 42–331       |
| `render.hpp`    | markdown renderer, log_*, tool-call display, strip/visible, print_diff, logo art, Spinner, glow   | 1090–1231, 1357–1630 |
| `terminal.hpp`  | RESIZE_FLAG/_on_resize, INPUT_PUSHBACK, input FSM, parse_mouse, bracketed paste, SLASH_COMMANDS    | 98–101, 327–458 |
| `widgets.hpp`   | slash_palette_lines, styled_input, menu_lines, interactive_menu, interactive_slider               | 459–1090     |
| `interrupt.hpp` | `Event`, `InterruptionManager`, global `interrupter`                                              | 1317–1355    |
| `secutil.hpp`   | sessions, safety gates, audit, path/url/ip, launcher, platform sysctl, shlex/urlparse/subprocess, difflib | 1630–2391, 2730–2779 |
| `toolparse.hpp` | extract_json_objects, xml/named/pythonic parsers, recover/normalize, prose heuristics, schema     | 2393–2918    |
| `http.hpp`      | libcurl `HttpRequest`/`HttpResponse`, `http_perform`, `http_stream`, `HttpClient` (cookie jar)    | (new infra)  |
| `lorea.hpp`     | `class LOREA` (ALL ~130 methods + member state), `run_spawn_agent_worker`, `run_main`              | 2919–7863    |

## Translation units (src/) — which .cpp implements what

| Source file              | Implements (Python fns/methods)                                                                                  | Python lines | Owns extern data defined here |
|--------------------------|-----------------------------------------------------------------------------------------------------------------|--------------|-------------------------------|
| `ansi.cpp`               | badge, clean_ansi, clean_len, term_cols/width, left_indent, center_pad, frame_title/bottom, status_label, gradient_text, progress_bar, sparkle_line, celebrate, mode_value, soft_rule, kv_row, truncate_visible, panel_lines, print_panel, print_frame_line/text, can_use_terminal_keys, raw_text, utf8_* | 42–331 | ACCENT, MUTED, FLAIR_RAMP, CELEBRATION_ICONS |
| `render.cpp`             | render_inline, render_text, fake_loading, log_tool/info/ok/warn, summarize_tool_args, print_tool_call, strip_tool_markup, visible_answer, print_diff, pick_logo_phrase + logo helpers, logo_lines, print_logo, Spinner, gold_gradient, glow_text | 1090–1231, 1357–1630 | CODE_FENCE_RE, TOOL_MARKUP_RE, TOOL_ICONS, LOGO_* art, LOGO_PHRASES, gradients, _GLOW_SHADES |
| `terminal.cpp`           | _on_resize, install_resize_handler, can_use_terminal_keys(dup), input_ready, wait_for_key_or_resize, read_input_byte, read_key, parse_mouse, read_bracketed_paste | 98–101, 327–458 | RESIZE_FLAG, INPUT_PUSHBACK, INPUT_PUSHBACK_MUTEX, SLASH_COMMANDS, MOUSE_SGR_RE |
| `widgets.cpp`            | slash_palette_lines, styled_input, menu_lines, interactive_menu, interactive_slider                              | 459–1090     | — |
| `interrupt.cpp`          | InterruptionManager::{start,stop,listen_loop}, `interrupter`                                                     | 1317–1355    | interrupter |
| `secutil_sessions.cpp`   | ensure_sessions_dir, resolve_session_path, list_saved_sessions, estimate_tokens, head_tail_trim, truncate_output | 1637–1700, 2087–2092, 1671–1700 | SESSIONS_DIR |
| `secutil_safety.cpp`     | url_is_safe, pentest_url_ok, classify_command, classify_offensive, security_audit, check_path_safety, extract_allowed_domains, should_search_official_domains, domain_matches | 1701–2310 | DANGEROUS/CATASTROPHIC_PATTERNS, QUOTED_LITERAL, EVAL_EXEC_INVOKE, OFFENSIVE_HARD_DENY/AUTHORIZE, SENSITIVE_PATH_PARTS, SENSITIVE_WRITE_PATHS |
| `secutil_launcher.cpp`   | launcher_matches_source, command_on_path, installed_via_package, install_cli_launcher, model_matches_backend, is_large_mlx_model | 2313–2391 | CLI_INSTALL_PATHS, ALLOWED_SEARCH_DOMAINS |
| `secutil_platform.cpp`   | is_apple_silicon, mac_total_ram_mb, mac_current_wired_limit_mb, mac_vram_bounds, mac_recommended_wired_limit_mb  | 2730–2779    | — |
| `secutil_helpers.cpp`    | shlex_split, shlex_quote, urlparse, urljoin, expanduser, realpath_str, difflib_ratio, run_subprocess, run_shell, Subprocess::*, spawn_process | (new infra)  | BACKEND_DEFAULT_URLS, BACKEND_DEFAULT_MODELS, CLOUD_BACKENDS, BACKEND_API_KEY_ENV, MODEL_SUGGESTIONS, DOWNLOAD_MODEL_OPTIONS, EFFORT_ORDER, effort_levels(), ADVR_LOOP, backend_kind/to_string, msg_* accessors |
| `toolparse.cpp`          | extract_json_objects, coerce_arg, parse_xml_tool_calls, parse_named_xml_tool_calls, balanced_paren_end, coerce_call_args, parse_pythonic_tool_calls, recover_tool_call_from_text, normalize_tool_call, direct_command_from_user, wants_system_and_ip, system_and_ip_command, is_continue_request, implies_web_search, promises_next_action, infer_web_search_query, spawn_agents_tool_schema, unicode_escape_decode | 2393–2918 | KNOWN_TOOL_NAMES, PY_REQUIRED_ARG, ZERO_ARG_TOOLS, TOOL_PRIMARY_ARG, NUMERIC_TOOL_ARGS, COMPLETION_RE, CONTINUATION_PROMISE_RE |
| `http.cpp`               | http_global_init, http_perform, http_stream, HttpClient::*, url_encode, build_query                             | (new infra)  | — |
| `lorea_core.cpp`         | LOREA ctor/dtor, cleanup, menu_choice, prompt_value, api_key, cloud_base, anthropic_auth_headers, anthropic_messages, to_anthropic_tools, set_backend, backend_menu, list_ollama_models, model_menu, vram_command, theme_command, display_metrics | 2920–3015, 3594–3806, 3807–3882, 6356–6376, 5642–5664 | LOREA::LOOP_SENTINEL, LOREA::LOOP_MAX_ITERATIONS |
| `lorea_mpc.cpp`          | parse_connect_args, normalize_mpc_url, mpc_headers, mpc_request, connect_mpc_command, disconnect_mpc, mpc_supports, print_mpc_status, print_mpc_downloads, mpc_control_menu, delete_mpc_model_menu, choose_mpc_backend, mpc_model_catalog, select_mpc_model, sync_mpc_selection, mpc_chat, mpc_chat_stream, apply_mpc_selection, start/cancel/wait_for_mpc_download, download_mpc_model_menu | 3015–3593 | — |
| `lorea_servers.cpp`      | is_port_open, ensure_lorea, ensure_mlx_server, find_llama_server_bin, ensure_llamacpp_server, llamacpp_health_ok, ensure_local_server, inference_hostport, kill_inference_procs_on_port, wait_port_closed, restart_inference_server, save_restart_reload, server_crash_note | 4098–4392 | — |
| `lorea_tools.cpp`        | offensive_gate, authorized_engagement_ok, run_cmd, test_cmd, download_mlx_model, send_input, list_files, search_files, grep, git_status, git_diff, web_search, read_url, http_request, read_file, write_file, build_tools_schema, tool_available, invoke_tool | 4393–5005, (helpers) | — |
| `lorea_plan_agents.cpp`  | parse_plan_tasks, print_tasks, create_plan, update_task, spawn_agent_chat, spawn_agents, parse_agent_args, build_agent_team, agent_completion_once, coordinate_agent_results, agent_command | 5006–5450 | — |
| `lorea_download.cpp`     | parse_download_args, model_download_default_dir, normalize_download_dir, prompt_download_dir, run_model_download, download_model_menu, loop_command, setup_llama_cpp, setup_mlx | 3883–4097, 5451–5513 | — |
| `lorea_session.cpp`      | save_session, session_menu, load_session, print_sessions | 5514–5641 | — |
| `lorea_context.cpp`      | compact_message_text, objective_text, compact_source_text, recent_context_slice, trim_recent_context, estimate_context_tokens, norm_tool_sig, fallback_compaction_summary, request_compaction_summary, effort_deep, extract_referenced_files, model_output_log, persist_outputs, compact_history, stall_decision, prune_repeated_assistant, tool_reminder_text, has_open_plan, in_working_context, tool_reminder_message, plan_state_text/message, effort_message, advr_reminder_message, ephemeral_reminders, server_messages, latest_tool_result_text, recent_tool_results_message, last_assistant_text | 5665–6253 | — |
| `lorea_repl.cpp`         | copy_last_answer, retry_last_turn, undo_last_write, run_oneoff_shell, show_working_diff, print_usage, goodbye, welcome_lines, run | 6238–6735 | — |
| `lorea_chat.cpp`         | process_chat (the streaming agent turn, all 6 backends + tool-call recovery + loop/stall/wedge state machine) | 6736–7760 | — |
| `lorea_main.cpp`         | run_spawn_agent_worker, run_main | 7761–7863 | — |
| `main.cpp`               | `int main(int argc,char**argv)` -> `ocli::run_main(...)`                                                         | 7862–7863    | — |

## Build order (dependency layers)

1. `types.hpp` (no deps) and `third_party/json.hpp`.
2. `ansi.cpp` (uses types).
3. `render.cpp`, `terminal.cpp` (use ansi, types). interrupt.cpp (termios only).
4. `widgets.cpp` (uses ansi, terminal, types).
5. `secutil_*.cpp` (use types; safety uses ansi/render for logging).
6. `toolparse.cpp` (uses types, secutil for shlex).
7. `http.cpp` (uses types).
8. `lorea_*.cpp` (use ALL of the above via lorea.hpp).
9. `lorea_chat.cpp` last (depends on the widest surface).
10. `lorea_main.cpp`, `main.cpp`.

All `.cpp` only need the headers; they can be compiled in parallel once the headers
exist. CMake `file(GLOB src/*.cpp)` builds them all into the single `ocli` target.

## extern-data ownership (avoid ODR double-definition)

Each `extern const …` table declared in a header is **defined exactly once** in the
`.cpp` noted in the "Owns extern data" column above. Do not redefine a table in two TUs.

## Cross-cutting gotchas every implementer must honor

- **Codepoints, not bytes**: all width/slice math uses `utf8_len`/`utf8_substr`/`clean_len`.
- **std::regex has no DOTALL**: rewrite `.` as `[\s\S]` in Python `re.DOTALL` patterns;
  `re.MULTILINE` -> `std::regex_constants::multiline`; `re.IGNORECASE` -> `icase`.
  Regex **alternation order** and substitution order are load-bearing.
- **Falsy-coalesce** `str(x or '')`: None/""/0/[]/False all collapse to "" before stringify.
- **difflib_ratio** is Ratcliff-Obershelp at threshold 0.85 with `[:4000]` prefixes and a
  `len>=40` floor — used by stall/prune/wedge.
- **arguments polymorphism**: `ToolCall.function.arguments` may be a parsed object OR a
  raw JSON string; keep it as `json` and re-parse with the {} fallback as Python does.
- **termios restore on every path** (RAII) for styled_input/menu/slider/interrupter;
  cbreak (not cfmakeraw) for the Esc listener; TCSADRAIN.
- **streaming abort**: `http_stream`'s `on_chunk` returns false to abort so the Esc
  interrupter / `should_stop` can cancel mid-flight; connection drops -> `MPCRetryable`.
- **model-facing string contracts** are load-bearing (run_cmd "(no output)" anti-reloop
  message, "Spawned agent results:\n"+json, untrusted `<...>` fences, reminder texts).
- **idempotent** start/stop for interrupter and cleanup() (guarded by `cleaned_up_`).
- airllm branches: unsupported in C++ (HF/torch in-process) — degrade to the fallback
  summary / an "AirLLM not supported" path; never silently drop the user-visible branch.

## Notes / deviations

- Python exceptions map to C++ types: KeyboardInterrupt -> `std::runtime_error("KeyboardInterrupt")`
  thrown by styled_input/menus and caught in `run()`; EOFError -> `"EOFError"`; ValueError on
  unbalanced quotes -> `ShlexError`; PermissionError(401) -> `MpcUnauthorized`;
  ValueError(no mpc_url) -> `MpcNoConnection`; raise_for_status -> `HttpStatusError`;
  `MPCRetryable` is its own type for the retry loop.
- The `ollama` python lib is replaced by direct HTTP to the Ollama API via `http.hpp`.
- `DDGS` (web_search) is reimplemented over libcurl in `lorea_tools.cpp`.
- `contextlib.redirect_stdout` (worker stdout capture) is replaced by an injectable
  output sink in `run_spawn_agent_worker` (see lorea_main.cpp); avoid global dup2 hacks.
- `inspect.signature` kwarg-filtering is replaced by a hardcoded per-tool accepted-arg
  table inside `invoke_tool` (C++ has no reflection).
