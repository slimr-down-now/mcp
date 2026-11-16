#pragma once
#include "mcp_oauth.hpp"

#include <httplib.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace slimr {

struct HttpServerOptions {
    std::string host{"0.0.0.0"};
    int port{0};
    std::string mcpPath{"/mcp"};
    std::string publicBaseUrl;
    size_t maxRequestBytes{4 * 1024 * 1024};
    uint32_t maxConnections{64};
    size_t workerThreads{4};
    size_t maxPendingJobs{32};
    size_t sseHighWaterBytes{32768};
    size_t sseMaxWriteBufBytes{65536};
    int shutdownTimeoutMs{5000};
    int keepaliveTimeoutMs{30000};
    OAuthResourceConfig oauth;
    std::vector<CustomHttpRoute> customRoutes;
};

namespace mcp_detail {

inline bool is_valid_ip_literal(std::string_view host, bool *isIpv6 = nullptr) {
    if (host.empty() || host.size() >= 64)
        return false;
    std::string tmp(host);
    struct in_addr v4{};
    if (inet_pton(AF_INET, tmp.c_str(), &v4) == 1) {
        if (isIpv6)
            *isIpv6 = false;
        return true;
    }
    struct in6_addr v6{};
    if (inet_pton(AF_INET6, tmp.c_str(), &v6) == 1) {
        if (isIpv6)
            *isIpv6 = true;
        return true;
    }
    return false;
}

inline std::string format_authority_host(std::string_view host, bool isIpv6) {
    if (isIpv6 && !host.empty() && host.front() != '[') {
        return "[" + std::string(host) + "]";
    }
    return std::string(host);
}

inline std::string strip_http_params(std::string_view v) {
    auto semi = v.find(';');
    if (semi != std::string_view::npos)
        v = v.substr(0, semi);
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t'))
        v.remove_prefix(1);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t'))
        v.remove_suffix(1);
    return ascii_lower(v);
}

inline bool is_json_content_type(std::string_view headerVal) {
    return strip_http_params(headerVal) == "application/json";
}

inline bool accepts_streamable_http(std::string_view acceptHeader) {
    bool hasJson = false;
    bool hasSse = false;
    size_t start = 0;
    while (start <= acceptHeader.size()) {
        auto comma = acceptHeader.find(',', start);
        std::string_view part = comma == std::string_view::npos
                                    ? acceptHeader.substr(start)
                                    : acceptHeader.substr(start, comma - start);
        std::string media = strip_http_params(part);
        if (media == "application/json")
            hasJson = true;
        if (media == "text/event-stream")
            hasSse = true;
        if (comma == std::string_view::npos)
            break;
        start = comma + 1;
    }
    return hasJson && hasSse;
}

/** Validates RFC 9110 field value bytes (0x21-0x7E, 0x20, 0x09) and decodes case-sensitive =?base64?<b64>?=. */
inline std::optional<std::string> decode_mcp_header_value(std::string_view raw) {
    for (unsigned char c : raw) {
        if (!((c >= 0x21 && c <= 0x7E) || c == 0x20 || c == 0x09)) {
            return std::nullopt;
        }
    }
    static constexpr std::string_view kPrefix = "=?base64?";
    static constexpr std::string_view kSuffix = "?=";
    if (raw.starts_with(kPrefix) && raw.ends_with(kSuffix) &&
        raw.size() >= kPrefix.size() + kSuffix.size()) {
        std::string_view inner = raw.substr(kPrefix.size(), raw.size() - kPrefix.size() - kSuffix.size());
        return base64_decode(inner);
    }
    return std::string(raw);
}

struct HeaderParamSpec {
    std::string paramPath;  // property name in arguments
    std::string headerName; // e.g., Mcp-Param-Region
    std::string schemaType; // "string", "integer", "number", "boolean"
};

inline std::vector<HeaderParamSpec> extract_x_mcp_headers(const Json &inputSchema) {
    std::vector<HeaderParamSpec> specs;
    if (!inputSchema.is_object() || !inputSchema.contains("properties") || !inputSchema["properties"].is_object()) {
        return specs;
    }
    for (const auto &[propName, propDef] : inputSchema["properties"].get_object()) {
        if (!propDef.is_object() || !propDef.contains("x-mcp-header"))
            continue;
        const Json &ext = propDef["x-mcp-header"];
        std::string suffix;
        if (ext.is_boolean() && ext.get<bool>()) {
            suffix = propName;
            if (!suffix.empty()) {
                suffix[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(suffix[0])));
            }
        } else if (ext.is_string()) {
            suffix = ext.get<std::string>();
        } else if (ext.is_object() && ext.contains("name") && ext["name"].is_string()) {
            suffix = ext["name"].get<std::string>();
        }
        if (suffix.empty())
            continue;
        std::string headerName = suffix.starts_with("Mcp-Param-") ? suffix : ("Mcp-Param-" + suffix);
        std::string stype = propDef.contains("type") && propDef["type"].is_string()
                                ? propDef["type"].get<std::string>()
                                : "string";
        specs.push_back(HeaderParamSpec{
            .paramPath = propName,
            .headerName = std::move(headerName),
            .schemaType = std::move(stype),
        });
    }
    return specs;
}

inline bool header_matches_json_param(std::string_view decodedHeader,
                                      const Json &jsonVal,
                                      std::string_view schemaType) {
    if (jsonVal.is_string()) {
        return decodedHeader == jsonVal.get<std::string>();
    }
    if (jsonVal.is_boolean()) {
        return (jsonVal.get<bool>() && decodedHeader == "true") ||
               (!jsonVal.get<bool>() && decodedHeader == "false");
    }
    if (jsonVal.is_number() && !jsonVal.is_boolean()) {
        std::string hdrStr(decodedHeader);
        char *endPtr = nullptr;
        double hNum = std::strtod(hdrStr.c_str(), &endPtr);
        if (endPtr == hdrStr.c_str() || *endPtr != '\0' || !std::isfinite(hNum)) {
            return false;
        }
        if (jsonVal.is_int64() && schemaType == "integer") {
            return hNum == static_cast<double>(jsonVal.get<int64_t>());
        }
        return std::abs(hNum - jsonVal.get<double>()) <= 1e-9;
    }
    return false;
}

inline std::map<std::string, std::string> parse_query_params(std::string_view rawUrl) {
    std::map<std::string, std::string> out;
    auto qpos = rawUrl.find('?');
    if (qpos == std::string_view::npos)
        return out;
    std::string_view qs = rawUrl.substr(qpos + 1);
    auto hash = qs.find('#');
    if (hash != std::string_view::npos)
        qs = qs.substr(0, hash);

    auto urlDecode = [](std::string_view s) {
        std::string res;
        res.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '+') {
                res.push_back(' ');
            } else if (s[i] == '%' && i + 2 < s.size()) {
                auto hexVal = [](char c) -> int {
                    if (c >= '0' && c <= '9')
                        return c - '0';
                    if (c >= 'a' && c <= 'f')
                        return c - 'a' + 10;
                    if (c >= 'A' && c <= 'F')
                        return c - 'A' + 10;
                    return -1;
                };
                int hi = hexVal(s[i + 1]);
                int lo = hexVal(s[i + 2]);
                if (hi >= 0 && lo >= 0) {
                    res.push_back(static_cast<char>((hi << 4) | lo));
                    i += 2;
                    continue;
                }
                res.push_back(s[i]);
            } else {
                res.push_back(s[i]);
            }
        }
        return res;
    };

    size_t pos = 0;
    while (pos <= qs.size()) {
        auto amp = qs.find('&', pos);
        std::string_view pair = amp == std::string_view::npos ? qs.substr(pos) : qs.substr(pos, amp - pos);
        if (!pair.empty()) {
            auto eq = pair.find('=');
            if (eq == std::string_view::npos) {
                out[urlDecode(pair)] = "";
            } else {
                out[urlDecode(pair.substr(0, eq))] = urlDecode(pair.substr(eq + 1));
            }
        }
        if (amp == std::string_view::npos)
            break;
        pos = amp + 1;
    }
    return out;
}

} // namespace mcp_detail

/** Bounded asynchronous Streamable HTTP and OAuth resource server for PluginApp. */
class McpHttpServer {
  public:
    explicit McpHttpServer(PluginApp &app, HttpServerOptions opts = {})
        : app_(app), opts_(std::move(opts)) {}

    McpHttpServer(const McpHttpServer &) = delete;
    McpHttpServer &operator=(const McpHttpServer &) = delete;

    ~McpHttpServer() { stop(); }

    uint16_t boundPort() const noexcept { return boundPort_.load(std::memory_order_acquire); }

    std::string baseUrl() const {
        if (!opts_.publicBaseUrl.empty()) {
            std::string b = opts_.publicBaseUrl;
            while (b.size() > 1 && b.back() == '/')
                b.pop_back();
            return b;
        }
        bool isV6 = false;
        mcp_detail::is_valid_ip_literal(opts_.host, &isV6);
        return "http://" + mcp_detail::format_authority_host(opts_.host, isV6) + ":" + std::to_string(boundPort());
    }

    std::string mcpEndpointUrl() const {
        return baseUrl() + normalizedMcpPath();
    }

    std::string resourceMetadataUrl() const {
        return baseUrl() + normalizedMetadataPath();
    }

    bool start(std::string *errOut = nullptr) {
        if (running_.load(std::memory_order_acquire)) {
            return true;
        }
        bool isV6 = false;
        if (!mcp_detail::is_valid_ip_literal(opts_.host, &isV6)) {
            if (errOut)
                *errOut = "Invalid bind host IP literal: " + opts_.host;
            return false;
        }

        if (opts_.port < 0 || opts_.port > 65535) {
            if (errOut) {
                *errOut = "Invalid port: " + std::to_string(opts_.port);
            }
            return false;
        }
        if (opts_.mcpPath.empty() || opts_.mcpPath.front() != '/') {
            if (errOut)
                *errOut = "mcpPath must start with '/'";
            return false;
        }

        svr_ = std::make_unique<httplib::Server>();
        const size_t baseThreads = std::max<size_t>(16, opts_.workerThreads + 8);
        const size_t maxThreads = std::max<size_t>(baseThreads, static_cast<size_t>(opts_.maxConnections) + 16);
        svr_->new_task_queue = [baseThreads, maxThreads]() {
            return new httplib::ThreadPool(baseThreads, maxThreads, 0);
        };
        svr_->set_payload_max_length(opts_.maxRequestBytes);
        svr_->set_keep_alive_timeout(std::max<time_t>(1, (opts_.keepaliveTimeoutMs + 999) / 1000));
        svr_->set_write_timeout(0, 500000);

        // Reject draining requests and oversized Content-Length before reading large bodies.
        svr_->set_pre_routing_handler([this](const httplib::Request &req, httplib::Response &res) {
            if (draining_.load(std::memory_order_acquire)) {
                res.set_header("Connection", "close");
                res.status = 503;
                res.set_content("{\"error\":\"server_stopping\"}", "application/json");
                return httplib::Server::HandlerResponse::Handled;
            }
            if (req.has_header("Content-Length")) {
                std::string cl = req.get_header_value("Content-Length");
                char *endPtr = nullptr;
                unsigned long long len = std::strtoull(cl.c_str(), &endPtr, 10);
                if (endPtr == cl.c_str() || *endPtr != '\0' || len > opts_.maxRequestBytes) {
                    res.status = 413;
                    res.set_content("{\"error\":\"payload_too_large\"}", "application/json");
                    return httplib::Server::HandlerResponse::Handled;
                }
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });

        // Consumer-mounted custom routes (registered before fallback PRM routes; first-match-wins).
        for (const auto &route : opts_.customRoutes) {
            registerCustomRoute(route);
        }

        // Protected Resource Metadata (RFC 9728)
        std::string metaPrimary = normalizedMetadataPath();
        std::string metaSuffixed = metaPrimary + normalizedMcpPath();
        auto metaHandler = [this](const httplib::Request &, httplib::Response &res) {
            try {
                Json doc = opts_.oauth.metadata.toJson(mcpEndpointUrl());
                sendJsonText(res, 200, slimr::write_json(doc));
            } catch (const std::exception &e) {
                sendJsonText(res, 500, "{\"error\":\"internal_error\",\"message\":\"" + redact_sensitive(e.what()) + "\"}");
            } catch (...) {
                sendJsonText(res, 500, "{\"error\":\"internal_error\"}");
            }
        };
        svr_->Get(metaPrimary, metaHandler);
        if (metaSuffixed != metaPrimary) {
            svr_->Get(metaSuffixed, metaHandler);
        }

        // Single Streamable HTTP endpoint: POST /mcp; 405 on other methods.
        std::string path = normalizedMcpPath();
        svr_->Post(path, [this](const httplib::Request &req, httplib::Response &res) {
            try {
                handleMcpPost(req, res);
            } catch (const std::exception &e) {
                sendJsonText(res, 500,
                             slimr::write_json(*app_.makeErrorResponse(
                                 std::nullopt, McpErrorCode::InternalError, redact_sensitive(e.what()))));
            } catch (...) {
                sendJsonText(res, 500, "{\"error\":\"internal_error\"}");
            }
        });
        auto methodNotAllowed = [](const httplib::Request &, httplib::Response &res) {
            sendResponse(res, 405, "application/json", "{\"error\":\"method_not_allowed\"}",
                         {{"Allow", "POST"}});
        };
        svr_->Get(path, methodNotAllowed);
        svr_->Delete(path, methodNotAllowed);
        svr_->Put(path, methodNotAllowed);

        draining_.store(false, std::memory_order_release);
        startWorkers();

        int actualPort = -1;
        if (opts_.port == 0) {
            actualPort = svr_->bind_to_any_port(opts_.host);
        } else if (svr_->bind_to_port(opts_.host, opts_.port)) {
            actualPort = opts_.port;
        }
        if (actualPort <= 0) {
            stopWorkers(true);
            svr_.reset();
            if (errOut) {
                *errOut = "Failed to bind HTTP server on " + opts_.host + ":" + std::to_string(opts_.port);
            }
            return false;
        }

        acceptThread_ = std::thread([this]() { svr_->listen_after_bind(); });
        svr_->wait_until_ready();
        if (!svr_->is_running()) {
            svr_->stop();
            if (acceptThread_.joinable()) {
                acceptThread_.join();
            }
            stopWorkers(true);
            svr_.reset();
            if (errOut) {
                *errOut = "Failed to start HTTP server on " + opts_.host + ":" + std::to_string(actualPort);
            }
            return false;
        }

        boundPort_.store(static_cast<uint16_t>(actualPort), std::memory_order_release);
        running_.store(true, std::memory_order_release);
        return true;
    }

    void stop() {
        bool wasRunning = running_.exchange(false, std::memory_order_acq_rel);
        if (!wasRunning) {
            stopWorkers(true);
            return;
        }

        draining_.store(true, std::memory_order_release);
        {
            std::lock_guard lock(workerState_->mu);
            workerState_->rejectNew = true;
        }

        // Wait up to shutdownTimeoutMs for natural completion of in-flight jobs before cancelling.
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(std::max(0, opts_.shutdownTimeoutMs));
        bool drainedNaturally = false;
        {
            std::unique_lock lock(workerState_->mu);
            drainedNaturally = workerState_->idleCv.wait_until(lock, deadline, [&]() {
                return workerState_->queue.empty() && workerState_->activeCount == 0;
            });
            for (auto &wk : workerState_->activeCancels) {
                if (auto src = wk.lock())
                    src->cancel();
            }
        }

        // Allow naturally completed responses to finish writing before svr_->stop() aborts chunked writes.
        if (drainedNaturally) {
            std::unique_lock lock(workerState_->mu);
            workerState_->responsesCv.wait_until(lock, deadline, [&]() {
                return workerState_->inFlightResponses == 0;
            });
        }

        if (svr_) {
            svr_->stop();
        }
        if (acceptThread_.joinable()) {
            acceptThread_.join();
        }
        stopWorkers(false);
        svr_.reset();
        boundPort_.store(0, std::memory_order_release);
    }

    int runUntilSignal() {
        static std::atomic<bool> *sStopFlag = nullptr;
        std::atomic<bool> stopRequested{false};
        sStopFlag = &stopRequested;

        auto sigHandler = [](int) {
            if (sStopFlag)
                sStopFlag->store(true, std::memory_order_release);
        };
        auto prevInt = std::signal(SIGINT, sigHandler);
        auto prevTerm = std::signal(SIGTERM, sigHandler);

        while (running_.load(std::memory_order_acquire) &&
               !stopRequested.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        std::signal(SIGINT, prevInt);
        std::signal(SIGTERM, prevTerm);
        sStopFlag = nullptr;
        stop();
        return 0;
    }

  private:
    struct WorkerState {
        std::mutex mu;
        std::condition_variable cv;
        std::condition_variable idleCv;
        std::condition_variable responsesCv;
        std::deque<std::function<void()>> queue;
        std::vector<std::weak_ptr<CancellationSource>> activeCancels;
        size_t activeCount{0};
        size_t inFlightResponses{0};
        bool rejectNew{false};
        bool stopping{false};
    };

    std::string normalizedMcpPath() const {
        return opts_.mcpPath.empty() ? "/mcp" : opts_.mcpPath;
    }

    std::string normalizedMetadataPath() const {
        if (!opts_.oauth.metadataPath.empty())
            return opts_.oauth.metadataPath;
        return "/.well-known/oauth-protected-resource";
    }

    static void sendResponse(httplib::Response &res,
                             int status,
                             std::string_view contentType,
                             const std::string &body,
                             std::vector<std::pair<std::string, std::string>> extraHeaders = {}) {
        res.status = status;
        for (const auto &[k, v] : extraHeaders) {
            res.set_header(k, v);
        }
        res.set_content(body, std::string(contentType));
    }

    static void sendJsonText(httplib::Response &res,
                             int status,
                             const std::string &jsonStr,
                             std::vector<std::pair<std::string, std::string>> extraHeaders = {}) {
        sendResponse(res, status, "application/json", jsonStr, std::move(extraHeaders));
    }

    static HttpRouteRequest toRouteRequest(const httplib::Request &req) {
        HttpRouteRequest out;
        out.method = req.method;
        out.path = req.path;
        out.rawQuery = req.target;
        out.queryParams = mcp_detail::parse_query_params(req.target);
        for (const auto &[k, v] : req.headers) {
            out.headers[mcp_detail::ascii_lower(k)] = v;
        }
        out.body = req.body;
        out.remoteAddr = req.remote_addr + ":" + std::to_string(req.remote_port);
        return out;
    }

    void registerCustomRoute(const CustomHttpRoute &route) {
        if (!route.handler || route.path.empty())
            return;
        auto wrap = [this, route](const httplib::Request &req, httplib::Response &res) {
            try {
                if (req.body.size() > opts_.maxRequestBytes) {
                    sendJsonText(res, 413, "{\"error\":\"payload_too_large\"}");
                    return;
                }
                HttpRouteRequest rreq = toRouteRequest(req);
                HttpRouteResponse rresp = route.handler(rreq);
                std::vector<std::pair<std::string, std::string>> hdrs;
                hdrs.reserve(rresp.headers.size());
                for (const auto &[k, v] : rresp.headers) {
                    hdrs.emplace_back(k, v);
                }
                sendResponse(res, rresp.status, rresp.contentType, rresp.body, std::move(hdrs));
            } catch (const std::exception &e) {
                sendJsonText(res, 500, "{\"error\":\"internal_error\",\"message\":\"" + redact_sensitive(e.what()) + "\"}");
            } catch (...) {
                sendJsonText(res, 500, "{\"error\":\"internal_error\"}");
            }
        };

        std::string m = mcp_detail::ascii_lower(route.method);
        if (m == "get")
            svr_->Get(route.path, wrap);
        else if (m == "post")
            svr_->Post(route.path, wrap);
        else if (m == "delete")
            svr_->Delete(route.path, wrap);
        else if (m == "put")
            svr_->Put(route.path, wrap);
        else if (m == "patch")
            svr_->Patch(route.path, wrap);
        else if (m == "options")
            svr_->Options(route.path, wrap);
        else if (m == "*") {
            svr_->Get(route.path, wrap);
            svr_->Post(route.path, wrap);
            svr_->Delete(route.path, wrap);
            svr_->Put(route.path, wrap);
            svr_->Patch(route.path, wrap);
            svr_->Options(route.path, wrap);
        }
    }

    void sendOAuthUnauthorized(httplib::Response &res,
                               std::span<const std::string> scopes,
                               std::string_view desc) const {
        std::string www = mcp_detail::build_www_authenticate(
            resourceMetadataUrl(), "", scopes, "");
        Json body = json_obj();
        body["error"] = "unauthorized";
        body["error_description"] = std::string(desc);
        sendJsonText(res, 401, slimr::write_json(body), {{"WWW-Authenticate", std::move(www)}});
    }

    void sendOAuthInsufficientScope(httplib::Response &res,
                                    std::span<const std::string> scopes,
                                    std::string_view desc) const {
        std::string www = mcp_detail::build_www_authenticate(
            resourceMetadataUrl(), "insufficient_scope", scopes, desc);
        Json body = json_obj();
        body["error"] = "insufficient_scope";
        body["error_description"] = std::string(desc);
        sendJsonText(res, 403, slimr::write_json(body), {{"WWW-Authenticate", std::move(www)}});
    }

    std::optional<std::string> validateHttpMcpHeaders(const Json &req,
                                                      const httplib::Request &httpReq,
                                                      const std::string &method) const {
        std::string protoHdr = httpReq.get_header_value("MCP-Protocol-Version");
        if (protoHdr.empty()) {
            return "Missing required MCP-Protocol-Version header";
        }
        std::string bodyVersion;
        if (req.contains("params") && req["params"].is_object() &&
            req["params"].contains("_meta") && req["params"]["_meta"].is_object() &&
            req["params"]["_meta"].contains("io.modelcontextprotocol/protocolVersion") &&
            req["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"].is_string()) {
            bodyVersion = req["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"].get<std::string>();
        }
        if (bodyVersion != protoHdr) {
            return "Header mismatch: MCP-Protocol-Version '" + protoHdr + "' does not match body '_meta.io.modelcontextprotocol/protocolVersion'";
        }

        std::string methodHdr = httpReq.get_header_value("Mcp-Method");
        if (methodHdr.empty()) {
            return "Missing required Mcp-Method header";
        }
        if (methodHdr != method) {
            return "Header mismatch: Mcp-Method '" + methodHdr + "' does not match body method '" + method + "'";
        }

        if (method == "tools/call" || method == "resources/read" || method == "prompts/get") {
            std::string rawNameHdr = httpReq.get_header_value("Mcp-Name");
            if (rawNameHdr.empty()) {
                return "Missing required Mcp-Name header for method '" + method + "'";
            }
            auto decodedName = mcp_detail::decode_mcp_header_value(rawNameHdr);
            if (!decodedName.has_value()) {
                return "Malformed Mcp-Name header";
            }
            std::string bodyName;
            if (req.contains("params") && req["params"].is_object()) {
                const Json &params = req["params"];
                if (method == "resources/read" && params.contains("uri") && params["uri"].is_string()) {
                    bodyName = params["uri"].get<std::string>();
                } else if (params.contains("name") && params["name"].is_string()) {
                    bodyName = params["name"].get<std::string>();
                }
            }
            if (*decodedName != bodyName) {
                return "Header mismatch: Mcp-Name '" + *decodedName + "' does not match body value '" + bodyName + "'";
            }

            if (method == "tools/call") {
                const ToolDef *def = app_.findTool(bodyName);
                if (def != nullptr) {
                    auto headerSpecs = mcp_detail::extract_x_mcp_headers(def->inputSchema);
                    Json args = json_obj();
                    if (req["params"].contains("arguments") && req["params"]["arguments"].is_object()) {
                        args = req["params"]["arguments"];
                    }
                    for (const auto &spec : headerSpecs) {
                        std::string hdrVal = httpReq.get_header_value(spec.headerName);
                        bool inBody = args.contains(spec.paramPath) && !args[spec.paramPath].is_null();
                        if (inBody && hdrVal.empty()) {
                            return "Missing required header " + spec.headerName + " for parameter '" + spec.paramPath + "'";
                        }
                        if (!hdrVal.empty()) {
                            auto decodedParam = mcp_detail::decode_mcp_header_value(hdrVal);
                            if (!decodedParam.has_value()) {
                                return "Malformed header " + spec.headerName;
                            }
                            if (!inBody) {
                                return "Header mismatch: unexpected " + spec.headerName + " when argument is absent or null";
                            }
                            if (!mcp_detail::header_matches_json_param(*decodedParam, args[spec.paramPath], spec.schemaType)) {
                                return "Header mismatch: " + spec.headerName + " does not match body parameter '" + spec.paramPath + "'";
                            }
                        }
                    }
                }
            }
        }
        return std::nullopt;
    }

    static int httpStatusForRpcError(int code) noexcept {
        switch (code) {
        case McpErrorCode::ParseError:
        case McpErrorCode::InvalidRequest:
        case McpErrorCode::HeaderMismatch:
        case McpErrorCode::MissingRequiredClientCapability:
        case McpErrorCode::UnsupportedProtocolVersion:
            return 400;
        case McpErrorCode::MethodNotFound:
            return 404;
        case McpErrorCode::InsufficientScope:
            return 403;
        default:
            return 200;
        }
    }

    void handleMcpPost(const httplib::Request &httpReq, httplib::Response &res) {
        if (!mcp_detail::is_json_content_type(httpReq.get_header_value("Content-Type"))) {
            sendJsonText(res, 415, "{\"error\":\"unsupported_media_type\"}");
            return;
        }

        if (httpReq.body.size() > opts_.maxRequestBytes) {
            sendJsonText(res, 413, "{\"error\":\"payload_too_large\"}");
            return;
        }

        auto parsed = parse_json<Json>(httpReq.body);
        if (!parsed) {
            sendJsonText(res, 400,
                         slimr::write_json(*app_.makeErrorResponse(std::nullopt, McpErrorCode::ParseError, "Parse error")));
            return;
        }
        const Json &req = *parsed;
        if (!req.is_object()) {
            sendJsonText(res, 400,
                         slimr::write_json(*app_.makeErrorResponse(std::nullopt, McpErrorCode::InvalidRequest, "Invalid Request: expected JSON object")));
            return;
        }
        if (!req.contains("jsonrpc") || !req["jsonrpc"].is_string() || req["jsonrpc"].get<std::string>() != "2.0") {
            std::optional<Json> errId;
            if (req.contains("id") && mcp_detail::is_valid_rpc_id(req["id"]))
                errId = req["id"];
            sendJsonText(res, 400,
                         slimr::write_json(*app_.makeErrorResponse(errId, McpErrorCode::InvalidRequest, "Invalid Request")));
            return;
        }

        const bool hasId = req.contains("id");
        if (hasId && !mcp_detail::is_valid_rpc_id(req["id"])) {
            sendJsonText(res, 400,
                         slimr::write_json(*app_.makeErrorResponse(std::nullopt, McpErrorCode::InvalidRequest, "Invalid Request: id must be a non-null string or number")));
            return;
        }
        std::optional<Json> id = hasId ? std::optional<Json>(req["id"]) : std::nullopt;

        if (!req.contains("method") || !req["method"].is_string() || req["method"].get<std::string>().empty()) {
            sendJsonText(res, 400,
                         slimr::write_json(*app_.makeErrorResponse(id, McpErrorCode::InvalidRequest, "Invalid Request: method must be a non-empty string")));
            return;
        }
        std::string method = req["method"].get<std::string>();

        if (hasId && !mcp_detail::accepts_streamable_http(httpReq.get_header_value("Accept"))) {
            sendJsonText(res, 406, "{\"error\":\"not_acceptable\",\"error_description\":\"Accept must include application/json and text/event-stream\"}");
            return;
        }

        Principal principal;
        const bool isDiscover = hasId && method == "server/discover";
        if (opts_.oauth.enabled && !(isDiscover && !opts_.oauth.protectDiscover)) {
            HttpRouteRequest rreq = toRouteRequest(httpReq);
            TokenValidationResult authRes = mcp_detail::evaluate_bearer_auth(opts_.oauth, mcpEndpointUrl(), rreq);
            if (authRes.status == TokenValidationResult::Status::ServiceUnavailable) {
                sendJsonText(res, 503, "{\"error\":\"service_unavailable\",\"error_description\":\"" + authRes.errorDescription + "\"}");
                return;
            }
            if (authRes.status == TokenValidationResult::Status::InvalidToken) {
                sendOAuthUnauthorized(res, authRes.requiredScopes, authRes.errorDescription);
                return;
            }
            if (authRes.status == TokenValidationResult::Status::InsufficientScope) {
                sendOAuthInsufficientScope(res, authRes.requiredScopes, authRes.errorDescription);
                return;
            }
            principal = std::move(authRes.principal);
        }

        if (!hasId) {
            // Notification accepted (no header-mirroring requirements in MCP 2026-07-28).
            sendResponse(res, 202, "application/json", "");
            return;
        }

        if (method == "initialize") {
            Json err = app_.dispatchMcpJson(req);
            sendJsonText(res, 404, slimr::write_json(err));
            return;
        }

        // Validate _meta envelope cleanly so UnsupportedProtocolVersion (-32022) and InvalidParams (-32602) precede/clarify header checks when needed.
        if (!req.contains("params") || !req["params"].is_object() ||
            !req["params"].contains("_meta") || !req["params"]["_meta"].is_object() ||
            !req["params"]["_meta"].contains("io.modelcontextprotocol/protocolVersion") ||
            !req["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"].is_string() ||
            !req["params"]["_meta"].contains("io.modelcontextprotocol/clientCapabilities") ||
            !req["params"]["_meta"]["io.modelcontextprotocol/clientCapabilities"].is_object()) {
            Json err = app_.dispatchMcpJson(req);
            sendJsonText(res, 400, slimr::write_json(err));
            return;
        }
        if (auto hdrErr = validateHttpMcpHeaders(req, httpReq, method)) {
            Json err = *app_.makeErrorResponse(id, McpErrorCode::HeaderMismatch, *hdrErr);
            sendJsonText(res, 400, slimr::write_json(err));
            return;
        }
        std::string reqVer = req["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"].get<std::string>();
        if (reqVer != kMcpProtocolVersion) {
            Json err = app_.dispatchMcpJson(req);
            sendJsonText(res, 400, slimr::write_json(err));
            return;
        }

        // Fast metadata lane: never blocked behind slow tool workers.
        if (method == "server/discover" || method == "tools/list" || method == "resources/list") {
            RequestContext baseCtx;
            baseCtx.principal = principal;
            Json resp = app_.dispatchMcpJson(req, baseCtx);
            sendJsonText(res, 200, slimr::write_json(resp));
            return;
        }

        if (method != "tools/call" && method != "resources/read") {
            Json err = app_.dispatchMcpJson(req);
            sendJsonText(res, 404, slimr::write_json(err));
            return;
        }

        dispatchToolOrResourceSync(httpReq, res, req, std::move(principal));
    }

    void dispatchToolOrResourceSync(const httplib::Request &httpReq,
                                    httplib::Response &res,
                                    const Json &req,
                                    Principal principal) {
        struct StreamSession {
            std::mutex mu;
            std::condition_variable cv;
            std::deque<std::string> sseEvents;
            CancellationSource cancelSrc;
            bool sseMode{false};
            bool finished{false};
            bool closed{false};
            Json finalResp;
            size_t droppedNotifications{0};
        };

        auto sess = std::make_shared<StreamSession>();
        const size_t highWater = opts_.sseHighWaterBytes;
        auto notifyChannel = std::make_shared<McpNotificationChannel>(
            [sess, highWater](const Json &notif) {
                if (sess->cancelSrc.isCancelled())
                    return;
                std::string raw = slimr::write_json(notif);
                std::erase(raw, '\n');
                std::erase(raw, '\r');
                std::string eventPayload = "data: " + raw + "\n\n";
                {
                    std::lock_guard lk(sess->mu);
                    if (sess->finished || sess->closed)
                        return;
                    sess->sseMode = true;
                    size_t queuedBytes = 0;
                    for (const auto &ev : sess->sseEvents) {
                        queuedBytes += ev.size();
                    }
                    if (queuedBytes >= highWater) {
                        ++sess->droppedNotifications;
                    } else {
                        sess->sseEvents.push_back(std::move(eventPayload));
                    }
                }
                sess->cv.notify_all();
            });

        auto job = [this, sess, req, principal = std::move(principal), notifyChannel]() mutable {
            RequestContext ctx;
            ctx.principal = std::move(principal);
            ctx.cancellation = sess->cancelSrc.token();
            ctx.notificationChannel = notifyChannel;

            Json resp;
            try {
                resp = app_.dispatchMcpJson(req, ctx);
            } catch (const std::exception &e) {
                std::optional<Json> rid;
                if (req.contains("id") && mcp_detail::is_valid_rpc_id(req["id"]))
                    rid = req["id"];
                resp = *app_.makeErrorResponse(rid, McpErrorCode::InternalError, redact_sensitive(e.what()));
            } catch (...) {
                std::optional<Json> rid;
                if (req.contains("id") && mcp_detail::is_valid_rpc_id(req["id"]))
                    rid = req["id"];
                resp = *app_.makeErrorResponse(rid, McpErrorCode::InternalError, "Internal error");
            }

            notifyChannel->close();

            {
                std::lock_guard lk(sess->mu);
                sess->finalResp = std::move(resp);
                sess->finished = true;
            }
            sess->cv.notify_all();
        };

        {
            std::lock_guard lock(workerState_->mu);
            if (workerState_->rejectNew || workerState_->stopping ||
                workerState_->queue.size() >= opts_.maxPendingJobs) {
                notifyChannel->close();
                sendResponse(res, 503, "application/json",
                             "{\"error\":\"server_busy\",\"error_description\":\"Worker queue full\"}",
                             {{"Retry-After", "1"}});
                return;
            }
            ++workerState_->inFlightResponses;
            std::erase_if(workerState_->activeCancels, [](const auto &w) { return w.expired(); });
            auto cancelHandle = std::make_shared<CancellationSource>(sess->cancelSrc);
            workerState_->activeCancels.push_back(cancelHandle);
            workerState_->queue.push_back([job = std::move(job), cancelHandle]() mutable {
                (void)cancelHandle;
                job();
            });
        }
        workerState_->cv.notify_one();

        auto releaseResponseSlot = [state = workerState_]() {
            std::lock_guard lock(state->mu);
            if (state->inFlightResponses > 0)
                --state->inFlightResponses;
            if (state->inFlightResponses == 0)
                state->responsesCv.notify_all();
        };

        bool startSse = false;
        bool aborted = false;
        Json respCopy;
        {
            std::unique_lock lk(sess->mu);
            while (!sess->sseMode && !sess->finished) {
                if (httpReq.is_connection_closed()) {
                    sess->cancelSrc.cancel();
                    sess->closed = true;
                    aborted = true;
                    break;
                }
                sess->cv.wait_for(lk, std::chrono::milliseconds(10));
            }
            if (!aborted) {
                startSse = sess->sseMode;
                if (!startSse && sess->finished) {
                    respCopy = std::move(sess->finalResp);
                }
            }
        }

        if (aborted) {
            releaseResponseSlot();
            return;
        }

        if (startSse) {
            res.set_header("Cache-Control", "no-cache");
            res.set_header("X-Accel-Buffering", "no");
            res.set_chunked_content_provider(
                "text/event-stream",
                [sess](size_t, httplib::DataSink &sink) {
                    std::deque<std::string> batch;
                    bool isDone = false;
                    Json finalResp;
                    {
                        std::unique_lock lk(sess->mu);
                        while (sess->sseEvents.empty() && !sess->finished) {
                            if (!sink.is_writable() || sess->cancelSrc.isCancelled()) {
                                sess->cancelSrc.cancel();
                                sess->closed = true;
                                return false;
                            }
                            sess->cv.wait_for(lk, std::chrono::milliseconds(10));
                        }
                        if (!sink.is_writable() || sess->cancelSrc.isCancelled()) {
                            sess->cancelSrc.cancel();
                            sess->closed = true;
                            return false;
                        }
                        batch.swap(sess->sseEvents);
                        if (sess->finished) {
                            isDone = true;
                            finalResp = std::move(sess->finalResp);
                        }
                    }
                    for (const auto &ev : batch) {
                        if (!sink.write(ev.data(), ev.size())) {
                            sess->cancelSrc.cancel();
                            return false;
                        }
                    }
                    if (isDone) {
                        std::string raw = slimr::write_json(finalResp);
                        std::erase(raw, '\n');
                        std::erase(raw, '\r');
                        std::string frame = "data: " + raw + "\n\n";
                        if (!sink.write(frame.data(), frame.size())) {
                            sess->cancelSrc.cancel();
                            return false;
                        }
                        sink.done();
                    }
                    return true;
                },
                [releaseResponseSlot](bool) {
                    releaseResponseSlot();
                });
            return;
        }

        if (sess->cancelSrc.isCancelled()) {
            releaseResponseSlot();
            return;
        }

        int httpCode = 200;
        if (respCopy.contains("error") && respCopy["error"].is_object() &&
            respCopy["error"].contains("code") && respCopy["error"]["code"].is_int64()) {
            int rpcCode = static_cast<int>(respCopy["error"]["code"].get<int64_t>());
            if (rpcCode == McpErrorCode::InsufficientScope) {
                std::vector<std::string> reqScopes;
                if (respCopy["error"].contains("data") && respCopy["error"]["data"].is_object() &&
                    respCopy["error"]["data"].contains("requiredScopes") &&
                    respCopy["error"]["data"]["requiredScopes"].is_array()) {
                    for (const auto &sc : respCopy["error"]["data"]["requiredScopes"].get_array()) {
                        if (sc.is_string())
                            reqScopes.push_back(sc.get<std::string>());
                    }
                }
                std::string desc = respCopy["error"].contains("message") && respCopy["error"]["message"].is_string()
                                       ? respCopy["error"]["message"].get<std::string>()
                                       : "Insufficient scope";
                sendOAuthInsufficientScope(res, reqScopes, desc);
                releaseResponseSlot();
                return;
            }
            httpCode = httpStatusForRpcError(rpcCode);
        }

        sendJsonText(res, httpCode, slimr::write_json(respCopy));
        releaseResponseSlot();
    }

    void startWorkers() {
        workerState_ = std::make_shared<WorkerState>();
        size_t n = std::max<size_t>(1, opts_.workerThreads);
        workers_.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            workers_.emplace_back([state = workerState_]() {
                for (;;) {
                    std::function<void()> fn;
                    {
                        std::unique_lock lock(state->mu);
                        state->cv.wait(lock, [&]() {
                            return state->stopping || !state->queue.empty();
                        });
                        if (state->queue.empty() && state->stopping)
                            return;
                        fn = std::move(state->queue.front());
                        state->queue.pop_front();
                        ++state->activeCount;
                    }
                    if (fn)
                        fn();
                    {
                        std::lock_guard lock(state->mu);
                        if (state->activeCount > 0)
                            --state->activeCount;
                        if (state->queue.empty() && state->activeCount == 0) {
                            state->idleCv.notify_all();
                        }
                    }
                }
            });
        }
    }

    void stopWorkers(bool cancelQueued) {
        if (!workerState_)
            return;
        {
            std::lock_guard lock(workerState_->mu);
            workerState_->stopping = true;
            if (cancelQueued) {
                workerState_->queue.clear();
            }
        }
        workerState_->cv.notify_all();
        for (auto &t : workers_) {
            if (t.joinable())
                t.join();
        }
        workers_.clear();
    }

    PluginApp &app_;
    HttpServerOptions opts_;
    std::atomic<bool> running_{false};
    std::atomic<bool> draining_{false};
    std::atomic<uint16_t> boundPort_{0};
    std::unique_ptr<httplib::Server> svr_;
    std::thread acceptThread_;
    std::shared_ptr<WorkerState> workerState_;
    std::vector<std::thread> workers_;
};

/** Attaches standalone `http [--host <addr>] [--port <n>]` subcommand to PluginApp. */
inline void enable_http(PluginApp &app, HttpServerOptions defaults = {}) {
    app.setHttpRunner([defaults = std::move(defaults)](PluginApp &target, int argc, char **argv) -> int {
        HttpServerOptions opts = defaults;
        for (int i = 2; i < argc; ++i) {
            std::string_view arg = argv[i];
            if (arg == "--host" && i + 1 < argc) {
                opts.host = argv[++i];
            } else if (arg == "--port" && i + 1 < argc) {
                std::string portStr = argv[++i];
                char *endPtr = nullptr;
                long parsedPort = std::strtol(portStr.c_str(), &endPtr, 10);
                if (endPtr == portStr.c_str() || *endPtr != '\0' || parsedPort < 0 || parsedPort > 65535) {
                    std::cerr << "Invalid --port value: " << portStr << "\n";
                    return 2;
                }
                opts.port = static_cast<int>(parsedPort);
            } else {
                std::cerr << "Unknown or incomplete HTTP option: " << arg << "\n";
                std::cerr << "Usage: " << (target.key.empty() ? "app" : target.key)
                          << " http [--host <addr>] [--port <n>]\n";
                return 2;
            }
        }

        McpHttpServer server(target, std::move(opts));
        std::string errMsg;
        if (!server.start(&errMsg)) {
            std::cerr << "HTTP server failed to start: " << errMsg << "\n";
            return 1;
        }
        std::cerr << (target.key.empty() ? "app" : target.key)
                  << " listening on " << server.mcpEndpointUrl() << "\n"
                  << std::flush;
        return server.runUntilSignal();
    });
}

} // namespace slimr
