#include "render.hpp"
#include "ansi.hpp"
#include "live_view.hpp"

#include <unistd.h>
#include <sys/ioctl.h>

#include <iostream>
#include <sstream>
#include <algorithm>
#include <cstdio>
#include <random>
#include <mutex>
#include <chrono>

namespace ocli {

namespace {

const std::string PY_WS = " \t\n\r\f\v";

std::string strip_py(const std::string& s) {
    std::size_t a = s.find_first_not_of(PY_WS);
    if (a == std::string::npos) return "";
    std::size_t b = s.find_last_not_of(PY_WS);
    return s.substr(a, b - a + 1);
}

std::string rstrip_py(const std::string& s) {
    std::size_t b = s.find_last_not_of(PY_WS);
    if (b == std::string::npos) return "";
    return s.substr(0, b + 1);
}

std::string strip_newlines(const std::string& s) {
    std::size_t a = s.find_first_not_of('\n');
    if (a == std::string::npos) return "";
    std::size_t b = s.find_last_not_of('\n');
    return s.substr(a, b - a + 1);
}

bool starts_with(const std::string& s, const std::string& pre) {
    return s.size() >= pre.size() && s.compare(0, pre.size(), pre) == 0;
}

std::string rep_str(const std::string& unit, int n) {
    std::string out;
    for (int i = 0; i < n; ++i) out += unit;
    return out;
}

std::string ljust_utf8(const std::string& s, int width) {
    int len = static_cast<int>(utf8_len(s));
    if (len >= width) return s;
    return s + std::string(static_cast<std::size_t>(width - len), ' ');
}

std::string random_choice(const std::vector<std::string>& v) {
    static std::mutex m;
    std::lock_guard<std::mutex> lk(m);
    static std::mt19937 g(std::random_device{}());
    std::uniform_int_distribution<std::size_t> d(0, v.size() - 1);
    return v[d(g)];
}

std::string py_str(const json& v) {
    if (v.is_string())  return v.get<std::string>();
    if (v.is_boolean()) return v.get<bool>() ? "True" : "False";
    if (v.is_null())    return "None";
    return v.dump();
}

bool empty_val(const json& v) {
    return v.is_null() || (v.is_string() && v.get<std::string>().empty());
}

}

const std::regex CODE_FENCE_RE(R"(^\s*```([A-Za-z0-9_+.-]*)\s*$)");

namespace {

const std::regex RE_CODE(R"(`([^`]+)`)");
const std::regex RE_BI(R"(\*\*\*(.*?)\*\*\*)");
const std::regex RE_B(R"(\*\*(.*?)\*\*)");
const std::regex RE_I(R"(\*(.*?)\*)");
const std::regex RE_H3(R"(^\s{0,3}### (.*)$)", std::regex::ECMAScript | std::regex::multiline);
const std::regex RE_H2(R"(^\s{0,3}## (.*)$)",  std::regex::ECMAScript | std::regex::multiline);
const std::regex RE_H1(R"(^\s{0,3}# (.*)$)",   std::regex::ECMAScript | std::regex::multiline);
const std::regex RE_HR(R"(^\s{0,3}(?:-{3,}|\*{3,}|_{3,})\s*$)", std::regex::ECMAScript | std::regex::multiline);
const std::regex RE_BQ(R"(^\s{0,3}>\s?(.*)$)", std::regex::ECMAScript | std::regex::multiline);
const std::regex RE_BULLET(R"(^(\s*)[\*\-] )", std::regex::ECMAScript | std::regex::multiline);
const std::regex RE_NUM(R"(^(\s*)(\d+)\. )",   std::regex::ECMAScript | std::regex::multiline);

std::string hr_dashes() {
    static const std::string d = rep_str("─", 32);
    return d;
}

}

std::string render_inline(const std::string& text) {
    using C = Colors;
    std::string t = text;
    t = std::regex_replace(t, RE_CODE, std::string(C::BG_DARK) + C::WHITE + " $1 " + C::RESET);
    t = std::regex_replace(t, RE_BI,   std::string(C::BOLD) + C::ITALIC + C::TEAL + "$1" + C::RESET);
    t = std::regex_replace(t, RE_B,    std::string(C::BOLD) + C::WHITE + "$1" + C::RESET);
    t = std::regex_replace(t, RE_I,    std::string(C::ITALIC) + C::SKY + "$1" + C::RESET);
    t = std::regex_replace(t, RE_H3,   std::string("\n") + C::BOLD + C::VIOLET + "◆ $1" + C::RESET);
    t = std::regex_replace(t, RE_H2,   std::string("\n") + C::BOLD + C::SKY + "▰ $1" + C::RESET);
    t = std::regex_replace(t, RE_H1,   std::string("\n") + C::BOLD + C::TEAL + "━━ $1 ━━" + C::RESET);
    t = std::regex_replace(t, RE_HR,   std::string(C::DIM) + C::GRAY + hr_dashes() + C::RESET);
    t = std::regex_replace(t, RE_BQ,   std::string(C::TEAL) + "▎" + C::RESET + " " + C::ITALIC + C::GRAY + "$1" + C::RESET);
    t = std::regex_replace(t, RE_BULLET, std::string("$1") + C::TEAL + "• " + C::RESET);
    t = std::regex_replace(t, RE_NUM,    std::string("$1") + C::ORANGE + "$2. " + C::RESET);
    return t;
}

std::string render_text(const std::string& text) {
    using C = Colors;

    std::vector<std::string> lines;
    {
        std::string cur;
        for (char ch : text) {
            if (ch == '\n') { lines.push_back(cur); cur.clear(); }
            else cur += ch;
        }
        lines.push_back(cur);
    }
    std::vector<std::string> out;
    bool in_fence = false;
    int rule_len = std::max(24, std::min(56, term_width() - 8));
    for (const auto& line : lines) {
        std::smatch m;
        if (std::regex_match(line, m, CODE_FENCE_RE)) {
            if (!in_fence) {
                in_fence = true;
                std::string lang = m[1].str();
                std::string label = lang.empty() ? "" : (" " + lang + " ");
                std::string head = std::string(C::DIM) + C::TEAL + "╭──" + C::RESET;
                if (!label.empty())
                    head += std::string(C::BOLD) + C::TEAL + label + C::RESET;
                int fill = std::max(2, rule_len - 3 - static_cast<int>(utf8_len(label)));
                head += std::string(C::DIM) + C::TEAL + rep_str("─", fill) + C::RESET;
                out.push_back(head);
            } else {
                in_fence = false;
                out.push_back(std::string(C::DIM) + C::TEAL + "╰" + rep_str("─", rule_len - 1) + C::RESET);
            }
            continue;
        }
        if (in_fence) {
            out.push_back(std::string(C::DIM) + C::TEAL + "│" + C::RESET + " " + C::CODE + line + C::RESET);
            continue;
        }
        out.push_back(render_inline(line));
    }
    std::string joined;
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (i) joined += "\n";
        joined += out[i];
    }
    return joined;
}

void fake_loading(const std::string& msg, double duration) {
    using C = Colors;
    static const std::vector<std::string> frames =
        {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
    auto end_time = std::chrono::steady_clock::now() +
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<double>(duration));
    std::size_t i = 0;
    std::string indent = left_indent();
    while (std::chrono::steady_clock::now() < end_time) {
        int shade = FLAIR_RAMP[i % FLAIR_RAMP.size()];
        std::cout << indent << "\033[38;5;" << shade << "m"
                  << frames[i % frames.size()] << C::RESET << " "
                  << C::DIM << C::GRAY << msg << C::RESET << "\r";
        std::cout.flush();
        std::this_thread::sleep_for(std::chrono::duration<double>(0.06));
        ++i;
    }
    std::cout << indent << C::GREEN << C::BOLD << "✓︎" << C::RESET << " "
              << C::DIM << C::GRAY << msg << C::RESET << "\033[K\n";
    std::cout.flush();
}

void log_tool(const std::string& msg) { fake_loading(msg); }

void log_info(const std::string& msg) {
    using C = Colors;
    std::cout << left_indent() << ACCENT << C::BOLD << "·" << C::RESET << " "
              << C::DIM << C::GRAY << "info" << C::RESET << "  " << msg << C::RESET << "\n";
}

void log_ok(const std::string& msg) {
    using C = Colors;
    std::cout << left_indent() << C::GREEN << C::BOLD << "✦" << C::RESET << " "
              << C::DIM << C::GREEN << "done" << C::RESET << "  " << msg << C::RESET << "\n";
}

void log_warn(const std::string& msg) {
    using C = Colors;
    std::cout << left_indent() << C::YELLOW << C::BOLD << "!" << C::RESET << " "
              << C::DIM << C::GRAY << "warn" << C::RESET << "  " << msg << C::RESET << "\n";
}

std::string summarize_tool_args(const json& args) {
    using C = Colors;
    if (!args.is_object() || args.empty()) return "";
    static const std::vector<std::string> preview_keys =
        {"command", "path", "query", "pattern", "url", "repo_id", "text", "index", "status"};
    std::vector<std::string> parts;
    for (const auto& key : preview_keys) {
        if (args.contains(key) && !empty_val(args.at(key))) {
            std::string value = py_str(args.at(key));

            for (auto& ch : value) if (ch == '\n') ch = ' ';
            if (utf8_len(value) > 60) value = utf8_substr(value, 0, 57) + "…";
            parts.push_back(std::string(C::DIM) + C::GRAY + key + "=" + C::RESET + C::WHITE + value + C::RESET);
        }
    }
    int leftover = 0;
    for (auto it = args.begin(); it != args.end(); ++it) {
        const std::string& k = it.key();
        if (std::find(preview_keys.begin(), preview_keys.end(), k) == preview_keys.end()
            && !empty_val(it.value()))
            ++leftover;
    }
    if (leftover)
        parts.push_back(std::string(C::DIM) + C::GRAY + "+" + std::to_string(leftover) + " more" + C::RESET);
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out += "  ";
        out += parts[i];
    }
    return out;
}

const std::map<std::string, std::string> TOOL_ICONS = {
    {"run_cmd", "❯"}, {"test_cmd", "▷"}, {"send_input", "↳"},
    {"write_file", "✎"}, {"read_file", "⊙"}, {"list_files", "▤"},
    {"search_files", "⌕"}, {"find_files", "⌕"}, {"grep", "⌕"}, {"web_search", "⌕"},
    {"read_url", "⇣"}, {"fetch_url", "⇣"}, {"download_mlx_model", "⇣"},
    {"http_request", "⇅"},
    {"git_status", "±"}, {"git_diff", "±"},
    {"create_plan", "☰"}, {"update_task", "☰"}, {"spawn_agents", "✦"},
};

void print_tool_call(const std::string& name, const json& args) {
    using C = Colors;
    std::string detail = summarize_tool_args(args);
    auto it = TOOL_ICONS.find(name);
    std::string icon = (it != TOOL_ICONS.end()) ? it->second : "*";
    std::string line = std::string(left_indent()) + ACCENT + C::BOLD + icon + " " + name + C::RESET;
    if (!detail.empty()) line += "  " + detail;
    std::cout << line << "\n";
}

const std::regex TOOL_MARKUP_RE(
    "<tool_call>[\\s\\S]*?</tool_call>"
    "|<function\\s*=[\\s\\S]*?</function>"
    "|<tools>[\\s\\S]*?</tools>"
    "|<tool_call>[\\s\\S]*$"
    "|<function\\s*=[\\s\\S]*$"
    "|<tools\\s*>[\\s\\S]*$"
    "|</?tools\\s*>"
    "|</?tool_call\\s*>");

namespace {
const std::regex RE_JSON_FENCE("```json\\s*\\{[\\s\\S]*?\\}\\s*```");
const std::regex RE_THOUGHT("<thought>[\\s\\S]*?(?:</thought>|$)");
const std::regex RE_THINK("<think>[\\s\\S]*?(?:</think>|$)");
const std::regex RE_THOUGHT_LINE(R"(^\s*(?:Thought|THINKING)\s*:.*$)",
                                 std::regex::ECMAScript | std::regex::multiline);
}

std::string strip_tool_markup(const std::string& text) {
    if (text.empty()) return text;
    std::string cleaned = std::regex_replace(text, TOOL_MARKUP_RE, "");
    cleaned = std::regex_replace(cleaned, RE_JSON_FENCE, "");
    return strip_py(cleaned);
}

std::string visible_answer(const std::string& text) {
    std::string cleaned = strip_tool_markup(text);
    cleaned = std::regex_replace(cleaned, RE_THOUGHT, "");
    cleaned = std::regex_replace(cleaned, RE_THINK, "");
    cleaned = std::regex_replace(cleaned, RE_THOUGHT_LINE, "");
    return strip_py(cleaned);
}

void print_diff(const std::vector<std::string>& diff_lines) {
    using C = Colors;
    if (diff_lines.empty()) return;
    std::string indent = left_indent();
    int width = term_width();
    int added = 0, removed = 0;
    for (const auto& line : diff_lines) {
        if (starts_with(line, "+") && !starts_with(line, "+++")) ++added;
        if (starts_with(line, "-") && !starts_with(line, "---")) ++removed;
    }
    std::string header_file;
    for (const auto& line : diff_lines) {
        if (starts_with(line, "+++ ")) {
            header_file = starts_with(line, "+++ b/") ? strip_py(utf8_substr(line, 6))
                                                      : strip_py(utf8_substr(line, 4));
            break;
        }
    }
    std::string subtitle = "+" + std::to_string(added) + " -" + std::to_string(removed)
                         + (header_file.empty() ? "" : (" · " + header_file));
    std::cout << "\n" << indent << frame_title("DIFF", C::MAGENTA, subtitle.c_str()) << "\n";

    auto tinted_row = [&](const std::string& marker, const std::string& body,
                          const char* fg, const char* bg) -> std::string {
        std::string text = marker + " " + body;
        int padn = std::max(0, width - 2 - static_cast<int>(clean_len(text)));
        std::string pad(static_cast<std::size_t>(padn), ' ');
        return indent + "  " + bg + fg + text + pad + C::RESET;
    };

    for (const auto& line : diff_lines) {
        std::string stripped = rstrip_py(line);
        if (starts_with(line, "+++") || starts_with(line, "---")) {
            std::cout << indent << "  " << C::DIM << C::GRAY << stripped << C::RESET << "\n";
        } else if (starts_with(line, "+")) {
            std::cout << tinted_row("+", utf8_substr(stripped, 1), C::DIFF_ADD, C::BG_ADD) << "\n";
        } else if (starts_with(line, "-")) {
            std::cout << tinted_row("-", utf8_substr(stripped, 1), C::DIFF_DEL, C::BG_DEL) << "\n";
        } else if (starts_with(line, "@@")) {
            std::cout << indent << "  " << C::BG_DARK << C::TEAL << C::BOLD
                      << " " << stripped << " " << C::RESET << "\n";
        } else {
            std::cout << indent << "  " << C::GRAY << stripped << C::RESET << "\n";
        }
    }
    std::cout << indent << frame_bottom(C::MAGENTA) << "\n\n";
}

const std::vector<std::string> LOGO_PHRASES = {
    "'Im, invinicble' - Mark Grayson",
    "-Code like a LOREAN",
    "-Powered by ollama and local AI models",
    "'Stand ready for my arrival, worm.' - Concquest",
    " 'MORE BATTLE, MORE GLORY, MORE WORTHY OPPONENTS!' - Battle Beast",
    "'I am Thragg the Regent of the Viltrum empire.'",
    "“ From this point forward we shall only be using our weapons to fight. No more tricks, understand that? ” - Dravik, The Rat King (BTW SHOUTOUT TO DEEPWOKEN)",
    "Code is poetry, and poetry is for everyone.",
    "“ Ah, the vainglorious folly... I shall make this agony everlasting. ” - True Heart of Enmity",
    "“ So it seems honour comes to reclaim its lost child. You fools simply misunderstand - I have found a power far greater than your impotent little Vow. ” - Zi'eer, the Fourth Prophet",
    "'Oh my god its albert einstein' - Some random dude",
    "“ …Your movements lack discipline. ” - Maestro Evengarde Rest ",
    "“ITS COMMON BALL“",
    "I... eat... your... sandwiches! I eat 'em up!",
    "'I found you, faker!','Faker? You aren't even good enough to be my fake!'",
    "'If life gives you lemons, make lemonade.'",
    "'Some protein tubes... with that white sauce'",
    "“ I am a Krulian, and one of the few known to you surface-folk. We are the closest bridge between the Gods below and your kind. The next step in the evolutionary chain you could say... But I really must be off, it was a pleasure dancing with you as always. ” ― The Ferryman",
    "'Im sorry for not being creative enough to come up with more phrases' - mont127",
    "'The only way to do great work is to love what you do.' - Steve Jobs",
    "'Code is like humor. When you have to explain it, its bad.' - Cory House",
    "'Shout out to realmaitreal (Or hafliss) for helping with macndcheese'",
    "'Its better to say something then just to stay silent' - I heard that somewhere dunno where",
    "If AI is your power, then what are you without it?",
    "*Some random cool line from a show i watched*",
    "'Sometimes I dream of saving the world, saving everyone from the invisible hand' - Elliot Alderson",
    "'Are you the strongest because you are Satoru Gojo or are you Satoru Gojo because you are the strongest?' - Geto Suguru",
    "'Stand users attract each other same goes for lonely people' ",
    "'The world is not beautiful, therefore it is.' - Kino's Journey",
    "'You cant like the sky without seeing the clouds' ",
    "'The world is full of nice people. If you can't find one, be one.' - Nishimiya Shouko",
    "'AI slop? As long as it makes the code Im fine with it.'",
    "'Cool quote'",
};

std::optional<std::string> pick_logo_phrase() {
    if (LOGO_PHRASES.empty()) return std::nullopt;
    return random_choice(LOGO_PHRASES);
}

namespace {

const std::vector<int> LORE_GRADIENT  = {156, 120, 84, 48, 50, 51};
const std::vector<int> A_GRADIENT     = {51, 51, 50, 50, 51, 51};
const std::vector<int> LOGO_GRADIENT  = {156, 84, 48};

const std::vector<std::string> LOGO_LORE_FULL = {
    "██╗      ██████╗ ██████╗ ███████╗",
    "██║     ██╔═══██╗██╔══██╗██╔════╝",
    "██║     ██║   ██║██████╔╝█████╗  ",
    "██║     ██║   ██║██╔══██╗██╔══╝  ",
    "███████╗╚██████╔╝██║  ██║███████╗",
    "╚══════╝ ╚═════╝ ╚═╝  ╚═╝╚══════╝",
};
const std::vector<std::string> LOGO_A_FULL = {
    " █████╗ ",
    "██╔══██╗",
    "███████║",
    "██╔══██║",
    "██║  ██║",
    "╚═╝  ╚═╝",
};
const std::vector<std::string> LOGO_LORE_MINI = {
    "╦   ╔═╗ ╦═╗ ╔═",
    "║   ║ ║ ╠╦╝ ╠═",
    "╩══ ╚═╝ ╩╚═ ╚═",
};
const std::vector<std::string> LOGO_A_MINI = {
    " ╔═╗",
    " ╠═╣",
    " ╩ ╩",
};

const char* FLOWER_ART_RAW = R"FLOWER(
            .:--:....
          .---------------.
         .:==------------:
        :======----------:               ..:----=.
     .-=========--------=:         ..:-==========-.
    :=============-----:=:      .:-===============.
  .=================--:.=:     .==================:
 .-------------=======..=:      .=================-.
        .:-=:   .=-..=-.=:       -=================:
      .======-.  .==.--.=:       -=================-
     .======-:==: .==-=.=:    ..:==================-.
       .-==+**..==:.-==-=-...-==:.==================.
       .=******-..==-:::::::=:..:===================:
   ..+**********===:::::::::==--:....:==============-
  .-*************=--:=:::::::-======================-
   .---+****++****:==::::::::.              ..-====-:
    .------------=:.:=:-::--====-.
     .-----------..-=====.====:.:===-..
      .---------:.====-==:-=.:==.  .:====:
       .---------=-==.===-.=: .:==:=====..
        .-------=.==--=.-=.==..-======.
         ..-----..========--=======-.
            :-:   .-=======:=====:.
)FLOWER";

std::vector<std::string> build_flower_lines() {
    std::string s = strip_newlines(std::string(FLOWER_ART_RAW));
    std::vector<std::string> lines;
    std::string cur;
    for (char ch : s) {
        if (ch == '\n') { lines.push_back(rstrip_py(cur)); cur.clear(); }
        else cur += ch;
    }
    lines.push_back(rstrip_py(cur));
    return lines;
}

const std::vector<std::string> LOGO_FLOWER_LINES = build_flower_lines();

const int LOCKUP_GAP = 4;
const std::vector<int> FLOWER_GREEN = {154, 118, 82, 46};
const std::vector<int> FLOWER_BLUE  = {45, 39, 33, 27};
const int FLOWER_CENTER_SHADE = 226;

const int FLOWER_CENTER_BOX[4] = {11, 16, 17, 29};
const int FLOWER_BLUE_BOX[4]   = {3, 16, 29, 999};

int grad_at(const std::vector<int>& g, int index) {
    int i = std::min(index, static_cast<int>(g.size()) - 1);
    if (i < 0) i = 0;
    return g[static_cast<std::size_t>(i)];
}

}

std::string center_pad_logo(int cols, int text_len) {
    int n = std::max(0, (cols - text_len) / 2);
    return std::string(static_cast<std::size_t>(n), ' ');
}

int term_rows(int def) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
        return ws.ws_row;
    return def;
}

std::vector<std::string> lorea_full_lines(int cols) {
    using C = Colors;
    int width = static_cast<int>(utf8_len(LOGO_LORE_FULL[0]) + utf8_len(LOGO_A_FULL[0]));
    std::string pad = center_pad_logo(cols, width);
    std::vector<std::string> lines;
    for (std::size_t index = 0; index < LOGO_LORE_FULL.size(); ++index) {
        int shade = grad_at(LORE_GRADIENT, static_cast<int>(index));
        int a_shade = grad_at(A_GRADIENT, static_cast<int>(index));
        lines.push_back(
            pad + "\033[38;5;" + std::to_string(shade) + "m" + C::BOLD + LOGO_LORE_FULL[index]
            + "\033[38;5;" + std::to_string(a_shade) + "m" + LOGO_A_FULL[index] + C::RESET);
    }
    return lines;
}

int flower_width() {
    int w = 0;
    for (const auto& ln : LOGO_FLOWER_LINES) w = std::max(w, static_cast<int>(utf8_len(ln)));
    return w;
}

int lockup_width() {
    return flower_width() + LOCKUP_GAP
         + static_cast<int>(utf8_len(LOGO_LORE_FULL[0]) + utf8_len(LOGO_A_FULL[0]));
}

int flower_color_code(int row, int col) {
    if (FLOWER_CENTER_BOX[0] <= row && row < FLOWER_CENTER_BOX[1]
        && FLOWER_CENTER_BOX[2] <= col && col < FLOWER_CENTER_BOX[3])
        return FLOWER_CENTER_SHADE;
    if (FLOWER_BLUE_BOX[0] <= row && row < FLOWER_BLUE_BOX[1]
        && FLOWER_BLUE_BOX[2] <= col && col < FLOWER_BLUE_BOX[3])
        return FLOWER_BLUE[static_cast<std::size_t>(row) % FLOWER_BLUE.size()];
    return FLOWER_GREEN[static_cast<std::size_t>(row) % FLOWER_GREEN.size()];
}

std::string color_flower_line(const std::string& fline, int row) {
    std::vector<std::string> chars = utf8_chars(fline);
    std::string out;
    int cur = -1;
    std::string buf;
    for (std::size_t col = 0; col < chars.size(); ++col) {
        int code = flower_color_code(row, static_cast<int>(col));
        if (code != cur) {
            if (!buf.empty())
                out += "\033[38;5;" + std::to_string(cur) + "m" + buf;
            cur = code;
            buf = chars[col];
        } else {
            buf += chars[col];
        }
    }
    if (!buf.empty())
        out += "\033[38;5;" + std::to_string(cur) + "m" + buf;
    return std::string(Colors::BOLD) + out + Colors::RESET;
}

std::vector<std::string> logo_lockup_lines(int cols) {
    using C = Colors;
    int flower_w = flower_width();
    int fh = static_cast<int>(LOGO_FLOWER_LINES.size());
    int lh = static_cast<int>(LOGO_LORE_FULL.size());
    std::string pad = center_pad_logo(cols, lockup_width());
    int start = std::max(0, (fh - lh) / 2);
    std::vector<std::string> lines;
    for (int i = 0; i < fh; ++i) {
        std::string left = color_flower_line(ljust_utf8(LOGO_FLOWER_LINES[i], flower_w), i);
        int j = i - start;
        std::string right;
        if (0 <= j && j < lh) {
            int lshade = grad_at(LORE_GRADIENT, j);
            int ashade = grad_at(A_GRADIENT, j);
            right = "\033[38;5;" + std::to_string(lshade) + "m" + C::BOLD + LOGO_LORE_FULL[j]
                  + "\033[38;5;" + std::to_string(ashade) + "m" + LOGO_A_FULL[j] + C::RESET;
        }
        lines.push_back(pad + left + rep_str(" ", LOCKUP_GAP) + right);
    }
    return lines;
}

std::vector<std::string> logo_lines(const std::optional<std::string>& phrase) {
    using C = Colors;
    int cols = term_cols();
    std::string tagline = "LOREA · AI coding assistant for the terminal";
    std::string sub = "local models  ·  shell  ·  files  ·  web  ·  autonomous coding";
    std::vector<std::string> out;
    out.push_back("");

    int full_width = static_cast<int>(utf8_len(LOGO_LORE_FULL[0]) + utf8_len(LOGO_A_FULL[0]));
    int mini_width = static_cast<int>(utf8_len(LOGO_LORE_MINI[0]) + utf8_len(LOGO_A_MINI[0]));
    int flower_height = static_cast<int>(LOGO_FLOWER_LINES.size());
    int lockup_w = LOGO_FLOWER_LINES.empty() ? 0 : lockup_width();

    if (!LOGO_FLOWER_LINES.empty() && cols >= lockup_w + 2 && term_rows() >= flower_height + 4) {
        auto lk = logo_lockup_lines(cols);
        out.insert(out.end(), lk.begin(), lk.end());
    } else if (cols >= full_width + 2) {
        auto fl = lorea_full_lines(cols);
        out.insert(out.end(), fl.begin(), fl.end());
    } else if (cols >= mini_width + 2) {
        std::string art_pad = center_pad_logo(cols, mini_width);
        for (std::size_t index = 0; index < LOGO_LORE_MINI.size(); ++index) {
            int shade = grad_at(LOGO_GRADIENT, static_cast<int>(index));
            out.push_back(
                art_pad + "\033[38;5;" + std::to_string(shade) + "m" + C::BOLD + LOGO_LORE_MINI[index]
                + "\033[38;5;51m" + LOGO_A_MINI[index] + C::RESET);
        }
    } else {
        out.push_back(center_pad_logo(cols, 5) + C::BOLD + "\033[38;5;46mLORE\033[38;5;51mA" + C::RESET);
    }

    {
        std::string label = " LOREA ";
        int inner = static_cast<int>(utf8_len(label));
        std::string bar = rep_str("─", inner);
        std::string pill_pad = center_pad_logo(cols, inner + 2);
        out.push_back("");
        out.push_back(pill_pad + C::DIM + C::GREEN + "╭" + bar + "╮" + C::RESET);
        out.push_back(pill_pad + C::DIM + C::GREEN + "│" + C::RESET +
                      C::BOLD + C::GREEN + label + C::RESET +
                      C::DIM + C::GREEN + "│" + C::RESET);
        out.push_back(pill_pad + C::DIM + C::GREEN + "╰" + bar + "╯" + C::RESET);
    }

    if (phrase && !phrase->empty()) {
        int avail = std::max(8, cols - 2);
        std::string shown = (static_cast<int>(clean_len(*phrase)) <= avail)
                                ? *phrase
                                : utf8_substr(*phrase, 0, avail - 1) + "…";
        out.push_back("");
        out.push_back(center_pad_logo(cols, static_cast<int>(clean_len(shown)))
                      + C::DIM + C::ITALIC + ACCENT + shown + C::RESET);
    }
    out.push_back("");
    if (cols >= static_cast<int>(utf8_len(tagline)))
        out.push_back(center_pad_logo(cols, static_cast<int>(utf8_len(tagline)))
                      + C::DIM + C::GRAY + tagline + C::RESET);
    if (cols >= static_cast<int>(utf8_len(sub)))
        out.push_back(center_pad_logo(cols, static_cast<int>(utf8_len(sub)))
                      + C::DIM + C::GRAY + sub + C::RESET);
    return out;
}

void print_logo(const std::optional<std::string>& phrase) {
    for (const auto& line : logo_lines(phrase))
        std::cout << line << "\n";
    std::cout << "\n";
}

namespace {
const std::vector<std::string> GLOW_SHADES = {
    "\033[38;5;220m", "\033[38;5;226m", "\033[38;5;214m", "\033[38;5;228m",
    "\033[38;5;208m", "\033[38;5;229m", "\033[38;5;184m", "\033[97m"};
}

std::string gold_gradient(const std::string& text) {
    std::vector<std::string> chars = utf8_chars(text);
    std::string out;
    for (std::size_t i = 0; i < chars.size(); ++i)
        out += GLOW_SHADES[i % GLOW_SHADES.size()] + chars[i];
    return out + Colors::RESET;
}

void glow_text(const std::string& text, int cycles, double delay) {
    using C = Colors;
    if (!isatty(STDOUT_FILENO)) {
        std::cout << "  " << C::AMBER << C::BOLD << text << C::RESET << "\n";
        return;
    }
    for (int c = 0; c < cycles; ++c) {
        for (const auto& shade : GLOW_SHADES) {
            std::cout << "\r  " << shade << C::BOLD << text << C::RESET << "\033[K";
            std::cout.flush();
            std::this_thread::sleep_for(std::chrono::duration<double>(delay));
        }
    }
    std::cout << "\r  " << C::AMBER << C::BOLD << text << C::RESET << "\033[K\n";
    std::cout.flush();
}

namespace {
const std::vector<std::string> SPIN_FRAMES =
    {"⣾", "⣽", "⣻", "⢿", "⡿", "⣟", "⣯", "⣷"};
const std::vector<std::string> SPIN_PULSES = {
    "syncing context", "sampling tokens", "routing tools", "stream warming",
    "planning tool calls", "reading context", "loading model"};
const int SPIN_PULSE_HOLD = 64;
const int SPIN_PHRASE_HOLD = 50;
}

Spinner::Spinner(std::string msg) : msg_(std::move(msg)) {
    current_pulse_ = random_choice(SPIN_PULSES);
}

Spinner::~Spinner() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void Spinner::start() {
    running_ = true;
    thread_ = std::thread(&Spinner::spin, this);
}

void Spinner::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void Spinner::spin() {
    using C = Colors;
    int i = 0;
    started_at_ = std::chrono::steady_clock::now();
    std::optional<std::string> phrase = pick_logo_phrase();
    bool two_line = phrase.has_value();
    while (running_) {
        double elapsed = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - started_at_).count();
        const std::string& frame = SPIN_FRAMES[static_cast<std::size_t>(i) % SPIN_FRAMES.size()];
        int fshade = FLAIR_RAMP[static_cast<std::size_t>(i) % FLAIR_RAMP.size()];
        int wave_pos = i % 8;
        std::string bar;
        for (int c = 0; c < 8; ++c) {
            int dist = ((c - wave_pos) % 8 + 8) % 8;
            if (dist <= 1) {
                int shade = FLAIR_RAMP[static_cast<std::size_t>(i + c) % FLAIR_RAMP.size()];
                bar += "\033[38;5;" + std::to_string(shade) + "m" + C::BOLD + "▰";
            } else {
                bar += std::string(C::DIM) + C::GRAY + "▱";
            }
        }
        bar += C::RESET;
        if (i && i % SPIN_PULSE_HOLD == 0) {
            std::vector<std::string> choices;
            for (const auto& p : SPIN_PULSES)
                if (p != current_pulse_) choices.push_back(p);
            if (choices.empty()) choices = SPIN_PULSES;
            current_pulse_ = random_choice(choices);
        }
        std::string pulse = current_pulse_;

        char ebuf[16];
        std::snprintf(ebuf, sizeof(ebuf), "%04.1f", elapsed);
        std::string line =
            std::string(left_indent()) + "\033[38;5;" + std::to_string(fshade) + "m" + C::BOLD
            + frame + C::RESET + " "
            + bar + "  "
            + C::WHITE + msg_ + C::RESET + "  "
            + C::DIM + C::GRAY + pulse + " · " + ebuf + "s" + C::RESET;

        if (live_active()) {
            live_set_status_line(line);
        } else if (two_line) {
            if (i && i % SPIN_PHRASE_HOLD == 0) {
                auto np = pick_logo_phrase();
                if (np) phrase = np;
            }
            int avail = std::max(8, term_cols() - LEFT_MARGIN - 2);
            std::string ptext = (static_cast<int>(utf8_len(*phrase)) <= avail)
                                    ? *phrase
                                    : utf8_substr(*phrase, 0, avail - 1) + "…";
            std::string phrase_line =
                std::string(left_indent()) + C::DIM + C::ITALIC + ACCENT + ptext + C::RESET;
            std::cout << "\r\033[2K" << line << "\n\033[2K" << phrase_line << "\033[1A\r";
            std::cout.flush();
        } else {
            std::cout << "\r\033[2K" << line;
            std::cout.flush();
        }
        last_len_ = static_cast<int>(clean_len(line));
        std::this_thread::sleep_for(std::chrono::duration<double>(0.08));
        ++i;
    }
    if (live_active()) {
        live_set_status_line("");
    } else if (two_line) {
        std::cout << "\r\033[2K\n\033[2K\033[1A\r";
        std::cout.flush();
    } else {
        std::cout << "\r\033[2K";
        std::cout.flush();
    }
}

}
