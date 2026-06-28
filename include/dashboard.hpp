#pragma once

// LAN web dashboard for the OCLI agent.
//
// FIXED CONTRACT (all dashboard files must agree):
//   GET  /            -> 200 text/html, body = DASHBOARD_HTML
//   GET  /api/info    -> {"model","backend","lan_url","auto","tool_access"}
//   GET  /api/history -> {"messages":[{"role","content"}...]}  (user/assistant only)
//   POST /api/chat    -> body {"message":"..."} -> {"response":"...","log":"..."}
//   POST /api/exec    -> body {"command":"..."} -> {"output":"..."}
//
// The server binds 0.0.0.0:port and reports the machine's first non-loopback
// IPv4 as lan_url = "http://<ip>:<port>".

namespace ocli {

class LOREA;  // fwd

// Starts the dashboard HTTP server on a background thread and returns
// immediately. Binds 0.0.0.0:port so the LAN can reach it.
void start_dashboard(LOREA& agent, int port = 8730);

// The full self-contained dashboard page (defined in src/dashboard_html.cpp).
extern const char* DASHBOARD_HTML;

}  // namespace ocli
