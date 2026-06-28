#pragma once

#include <string>
#include <vector>
#include <map>
#include <set>
#include <optional>
#include <regex>

#include "types.hpp"

namespace ocli {

extern const std::vector<std::string>            KNOWN_TOOL_NAMES;
extern const std::map<std::string, std::string>  PY_REQUIRED_ARG;
extern const std::set<std::string>               ZERO_ARG_TOOLS;
extern const std::map<std::string, std::string>  TOOL_PRIMARY_ARG;
extern const std::set<std::string>               NUMERIC_TOOL_ARGS;
extern const std::regex                          COMPLETION_RE;
extern const std::regex                          CONTINUATION_PROMISE_RE;

std::vector<std::string> extract_json_objects(const std::string& text);
json                     coerce_arg(const std::string& key, const json& value);

std::vector<json> parse_xml_tool_calls(const std::string& text);
std::vector<json> parse_named_xml_tool_calls(const std::string& text,
                                             const std::vector<std::string>* known_tools = nullptr);
long              balanced_paren_end(const std::string& s, size_t open_idx);
std::optional<json> coerce_call_args(const std::string& inner, const std::string& name);
std::vector<json> parse_pythonic_tool_calls(const std::string& text,
                                            const std::vector<std::string>& known_tools = KNOWN_TOOL_NAMES);
std::optional<json> recover_tool_call_from_text(const std::string& text);
json                normalize_tool_call(const json& tc);

std::optional<std::string> direct_command_from_user(const std::string& text);
bool                       wants_system_and_ip(const std::string& text);
std::string                system_and_ip_command();
bool                       is_continue_request(const std::string& text);
bool                       implies_web_search(const std::string& text);
bool                       promises_next_action(const std::string& text);
std::string                infer_web_search_query(const std::string& text,
                                                  const std::string& goal = "", int prior_searches = 0);

json spawn_agents_tool_schema();

std::string unicode_escape_decode(const std::string& s);

}
