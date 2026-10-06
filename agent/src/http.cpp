#include "http.hpp"

#include <httplib.h>

#include <cstdio>
#include <cstdlib>

#include "agent.hpp"
#include "inventory.hpp"
#include "json.hpp"
#include "tar.hpp"

namespace rb {

AgentState g_state;

namespace {

std::string param(const httplib::Request& req, const std::string& key) {
  return req.get_param_value(key);
}

void jsonResponse(httplib::Response& res, int status, const std::string& body) {
  res.status = status;
  res.set_content(body, "application/json");
}

void notImplemented(const httplib::Request& req, httplib::Response& res) {
  jsonResponse(res, 501, "{\"error\":\"endpoint not implemented in this build\"}");
}

bool authOk(const httplib::Request& req) {
  const auto it = req.headers.find("Authorization");
  if (it == req.headers.end()) return false;
  const std::string& v = it->second;
  if (v.rfind("Bearer ", 0) != 0) return false;
  return v.size() >= 7 && v.compare(7, std::string::npos, g_state.token) == 0;
}

struct WsGuard {
  std::string ws;
  bool held = false;
  ~WsGuard() {
    if (held) releaseWs(ws);
  }
};

}  // namespace

int runServer() {
  g_state.jobs = std::make_unique<JobRunner>(g_state.maxJobs);
  httplib::Server svr;
  svr.set_payload_max_length(static_cast<size_t>(8) * 1024 * 1024 * 1024);
  svr.set_pre_routing_handler([](const httplib::Request& req, httplib::Response& res) {
    if (!authOk(req)) {
      res.status = 401;
      res.set_content("{\"error\":\"unauthorized\"}", "application/json");
      return httplib::Server::HandlerResponse::Handled;
    }
    return httplib::Server::HandlerResponse::Unhandled;
  });
  svr.set_exception_handler(
      [](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
        std::string what = "internal error";
        if (ep) {
          try {
            std::rethrow_exception(ep);
          } catch (const std::exception& e) {
            what = e.what();
          } catch (...) {
          }
        }
        const std::string msg =
            std::string("{\"error\":\"") + jsonEscape(what) + "\"}";
        res.status = 500;
        res.set_content(msg, "application/json");
      });

  svr.Get("/inventory", [](const httplib::Request&, httplib::Response& res) {
    res.set_content(buildInventoryJson(), "application/json");
  });

  svr.Get("/manifest", [](const httplib::Request& req, httplib::Response& res) {
    const std::string ws = param(req, "ws");
    if (!validateWsName(ws)) {
      jsonResponse(res, 400, "{\"error\":\"bad workspace name\"}");
      return;
    }
    const std::filesystem::path manifestPath = wsRoot(ws) / "manifest.json";
    const std::string text = readFileToString(manifestPath);
    if (text.empty()) {
      jsonResponse(res, 404, "{\"error\":\"no workspace\"}");
      return;
    }
    res.set_content(text, "application/json");
  });

  svr.Post("/sync", [](const httplib::Request& req, httplib::Response& res,
                       const httplib::ContentReader& content_reader) {
    const std::string ws = param(req, "ws");
    if (!validateWsName(ws)) {
      jsonResponse(res, 400, "{\"error\":\"bad workspace name\"}");
      return;
    }
    WsGuard guard;
    guard.ws = ws;
    if (!acquireWs(ws)) {
      jsonResponse(res, 409, "{\"error\":\"workspace busy (job running)\"}");
      return;
    }
    guard.held = true;
    TarApplier applier(wsRoot(ws));
    const bool fed = content_reader([&](const char* data, size_t len) {
      return applier.feed(data, len);
    });
    if (!fed) {
      jsonResponse(res, 400,
                   "{\"error\":\"" + jsonEscape(applier.error()) + "\"}");
      return;
    }
    if (!applier.finish()) {
      jsonResponse(res, 400,
                   "{\"error\":\"" + jsonEscape(applier.error()) + "\"}");
      return;
    }
    const TarStats& st = applier.stats();
    const std::string body =
        "{\"ok\":true,\"applied\":" + std::to_string(st.appliedFiles) +
        ",\"bytes\":" + std::to_string(st.appliedBytes) +
        ",\"manifestFiles\":" + std::to_string(st.manifestFiles) +
        ",\"pruned\":" + std::to_string(st.prunedFiles) + "}";
    jsonResponse(res, 200, body);
  });

  svr.Delete(R"(/ws/([^/]+))",
             [](const httplib::Request& req, httplib::Response& res) {
               const std::string ws = req.matches[1];
               if (!validateWsName(ws)) {
                 jsonResponse(res, 400, "{\"error\":\"bad workspace name\"}");
                 return;
               }
               WsGuard guard;
               guard.ws = ws;
               if (!acquireWs(ws)) {
                 jsonResponse(res, 409, "{\"error\":\"workspace busy\"}");
                 return;
               }
               guard.held = true;
               std::error_code ec;
               std::filesystem::remove_all(wsRoot(ws), ec);
               jsonResponse(res, 200,
                            ec ? "{\"error\":\"remove failed\"}"
                               : "{\"removed\":true}");
             });

  svr.Post("/jobs", [](const httplib::Request& req, httplib::Response& res,
                         const httplib::ContentReader& content_reader) {
    const std::string ws = param(req, "ws");
    if (!validateWsName(ws)) {
      jsonResponse(res, 400, "{\"error\":\"bad workspace name\"}");
      return;
    }
    std::string body;
    const bool ok = content_reader([&](const char* data, size_t len) {
      body.append(data, len);
      return true;
    });
    if (!ok) {
      jsonResponse(res, 400, "{\"error\":\"body read failed\"}");
      return;
    }
    JsonValue request;
    std::string err;
    if (!parseJson(body, request, err)) {
      jsonResponse(res, 400,
                   "{\"error\":\"bad json: " + jsonEscape(err) + "\"}");
      return;
    }
    const std::string jobId = g_state.jobs->enqueue(ws, request, err);
    if (jobId.empty()) {
      jsonResponse(res, 400,
                   "{\"error\":\"" + jsonEscape(err) + "\"}");
      return;
    }
    jsonResponse(res, 201,
                 "{\"jobId\":\"" + jobId +
                     "\",\"status\":\"queued\"}");
  });

  svr.Get(R"(/jobs/([^/]+))",
          [](const httplib::Request& req, httplib::Response& res) {
            const std::string id = req.matches[1];
            JsonValue out;
            std::string err;
            if (!g_state.jobs->get(id, out, err)) {
              jsonResponse(res, 404,
                           "{\"error\":\"" + jsonEscape(err) + "\"}");
              return;
            }
            res.set_content(stringifyJson(out), "application/json");
          });

  svr.Get(R"(/jobs/([^/]+)/log)",
          [](const httplib::Request& req, httplib::Response& res) {
            const std::string id = req.matches[1];
            const std::string offsetStr = param(req, "offset");
            long long offset = 0;
            if (!offsetStr.empty()) {
              try {
                offset = std::stoll(offsetStr);
              } catch (...) {
                jsonResponse(res, 400, "{\"error\":\"bad offset\"}");
                return;
              }
            }
            std::string data;
            long long total = 0;
            std::string err;
            if (!g_state.jobs->readLog(id, offset, data, total, err)) {
              jsonResponse(res, 400,
                           "{\"error\":\"" + jsonEscape(err) + "\"}");
              return;
            }
            res.set_header("X-RB-Log-Bytes", std::to_string(total));
            res.set_content(data, "application/octet-stream");
          });

  svr.Delete(R"(/jobs/([^/]+))",
             [](const httplib::Request& req, httplib::Response& res) {
               const std::string id = req.matches[1];
               std::string err;
               if (!g_state.jobs->kill(id, err)) {
                 jsonResponse(res, 404,
                              "{\"error\":\"" + jsonEscape(err) + "\"}");
                 return;
               }
               jsonResponse(res, 200, "{\"killed\":true}");
             });

  svr.Post("/fetch-tar", notImplemented);

  std::printf("[rbagent] listening on 0.0.0.0:%d  (machine=%s, root=%s)\n", g_state.port,
              g_state.machineName.c_str(), g_state.root.c_str());
  std::fflush(stdout);
  return svr.listen("0.0.0.0", g_state.port) ? 0 : 1;
}

}  // namespace rb
