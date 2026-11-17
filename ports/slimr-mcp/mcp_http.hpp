#pragma once
#include "mcp_oauth.hpp"
#include <serve_http.hpp>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slimr {

/** Extended HTTP options for McpHttpServer; implicitly converts from ServeHttpOptions (HttpServerOptions). */
struct McpHttpOptions : ServeHttpOptions {
    std::string mcpPath{"/mcp"};
    size_t maxPendingJobs{32};
    size_t sseHighWaterBytes{32768};
    size_t sseMaxWriteBufBytes{65536};

    McpHttpOptions() = default;
    McpHttpOptions(const ServeHttpOptions &base) : ServeHttpOptions(base) {}
    McpHttpOptions(ServeHttpOptions &&base) : ServeHttpOptions(std::move(base)) {}
};

namespace mcp_detail {

inline bool accepts_streamable_http(std::string_view acceptHeader) {
    bool hasJson = false;
    bool hasSse = false;
    size_t start = 0;
    while (start <= acceptHeader.size()) {
        auto comma = acceptHeader.find(',', start);
        std::string_view part = comma == std::string_view::npos
                                    ? acceptHeader.substr(start)
                                    : acceptHeader.substr(start, comma - start);
        std::string media = serve_detail::strip_http_params(part);
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
        return serve_detail::base64_decode(inner);
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

} // namespace mcp_detail

/** Streamable HTTP MCP 2026-07-28 and OAuth resource server delegating to slimr::ServeHttpServer. */
class McpHttpServer {
  public:
    explicit McpHttpServer(PluginApp &app, McpHttpOptions opts = {})
        : app_(app), opts_(std::move(opts)) {}

    McpHttpServer(const McpHttpServer &) = delete;
    McpHttpServer &operator=(const McpHttpServer &) = delete;

    ~McpHttpServer() { stop(); }

    uint16_t boundPort() const noexcept { return boundPort_.load(std::memory_order_acquire); }

    std::string baseUrl() const {
        if (server_) {
            return server_->baseUrl();
        }
        if (!opts_.publicBaseUrl.empty()) {
            std::string b = opts_.publicBaseUrl;
            while (b.size() > 1 && b.back() == '/')
                b.pop_back();
            return b;
        }
        bool isV6 = false;
        serve_detail::is_valid_ip_literal(opts_.host, &isV6);
        return "http://" + serve_detail::format_authority_host(opts_.host, isV6) + ":" + std::to_string(boundPort());
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
        if (opts_.port && (*opts_.port < 0 || *opts_.port > 65535)) {
            if (errOut) {
                *errOut = "Invalid port: " + std::to_string(*opts_.port);
            }
            return false;
        }
        if (opts_.mcpPath.empty() || opts_.mcpPath.front() != '/') {
            if (errOut) {
                *errOut = "mcpPath must start with '/'";
            }
            return false;
        }

        ServeHttpOptions serveOpts = static_cast<const ServeHttpOptions &>(opts_);
        if (!serveOpts.port.has_value() && serveOpts.socketPath.empty()) {
            serveOpts.port = 0;
        }

        // Mount POST /mcp + 405 handlers + OAuth PRM routes via customRoutes (registered before POST /:tool).
        std::string path = normalizedMcpPath();
        serveOpts.customRoutes.push_back(CustomHttpRoute{
            .method = "POST",
            .path = path,
            .handler = [this](const HttpRouteRequest &req) {
                try {
                    return handleMcpRoute(req);
                } catch (const std::exception &e) {
                    return HttpRouteResponse::json(
                        500, *app_.makeErrorResponse(std::nullopt, McpErrorCode::InternalError, redact_sensitive(e.what())));
                } catch (...) {
                    return HttpRouteResponse::json(500, parse_json<Json>("{\"error\":\"internal_error\"}").value_or(json_obj()));
                }
            },
        });

        auto methodNotAllowed = [](const HttpRouteRequest &) -> HttpRouteResponse {
            HttpRouteResponse r;
            r.status = 405;
            r.contentType = "application/json";
            r.headers["Allow"] = "POST";
            r.body = "{\"error\":\"method_not_allowed\"}";
            return r;
        };
        for (const char *m : {"GET", "DELETE", "PUT", "PATCH"}) {
            serveOpts.customRoutes.push_back(CustomHttpRoute{
                .method = m,
                .path = path,
                .handler = methodNotAllowed,
            });
        }

        std::string metaPrimary = normalizedMetadataPath();
        std::string metaSuffixed = metaPrimary + path;
        auto metaHandler = [this](const HttpRouteRequest &) -> HttpRouteResponse {
            try {
                Json doc = opts_.oauth.metadata.toJson(mcpEndpointUrl());
                return HttpRouteResponse::json(200, doc);
            } catch (const std::exception &e) {
                Json err = json_obj();
                err["error"] = "internal_error";
                err["message"] = redact_sensitive(e.what());
                return HttpRouteResponse::json(500, err);
            } catch (...) {
                return HttpRouteResponse::json(500, parse_json<Json>("{\"error\":\"internal_error\"}").value_or(json_obj()));
            }
        };
        serveOpts.customRoutes.push_back(CustomHttpRoute{
            .method = "GET",
            .path = metaPrimary,
            .handler = metaHandler,
        });
        if (metaSuffixed != metaPrimary) {
            serveOpts.customRoutes.push_back(CustomHttpRoute{
                .method = "GET",
                .path = metaSuffixed,
                .handler = metaHandler,
            });
        }

        server_ = std::make_unique<ServeHttpServer>(app_, std::move(serveOpts));
        if (!server_->start(errOut)) {
            server_.reset();
            return false;
        }
        boundPort_.store(server_->boundPort(), std::memory_order_release);
        running_.store(true, std::memory_order_release);
        return true;
    }

    void stop() {
        if (!running_.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        if (server_) {
            server_->stop();
        }
        boundPort_.store(0, std::memory_order_release);
    }

    int runUntilSignal() {
        int rc = server_ ? server_->runUntilSignal() : 1;
        stop();
        return rc;
    }

  private:
    std::string normalizedMcpPath() const {
        return opts_.mcpPath.empty() ? "/mcp" : opts_.mcpPath;
    }

    std::string normalizedMetadataPath() const {
        if (!opts_.oauth.metadataPath.empty())
            return opts_.oauth.metadataPath;
        return "/.well-known/oauth-protected-resource";
    }

    HttpRouteResponse makeOAuthUnauthorized(std::span<const std::string> scopes,
                                            std::string_view desc) const {
        std::string www = serve_detail::build_www_authenticate(
            resourceMetadataUrl(), "", scopes, "");
        Json body = json_obj();
        body["error"] = "unauthorized";
        body["error_description"] = std::string(desc);
        HttpRouteResponse r = HttpRouteResponse::json(401, body);
        r.headers["WWW-Authenticate"] = std::move(www);
        return r;
    }

    HttpRouteResponse makeOAuthInsufficientScope(std::span<const std::string> scopes,
                                                 std::string_view desc) const {
        std::string www = serve_detail::build_www_authenticate(
            resourceMetadataUrl(), "insufficient_scope", scopes, desc);
        Json body = json_obj();
        body["error"] = "insufficient_scope";
        body["error_description"] = std::string(desc);
        HttpRouteResponse r = HttpRouteResponse::json(403, body);
        r.headers["WWW-Authenticate"] = std::move(www);
        return r;
    }

    std::optional<std::string> validateHttpMcpHeaders(const Json &req,
                                                      const HttpRouteRequest &httpReq,
                                                      const std::string &method) const {
        std::string protoHdr = httpReq.header("MCP-Protocol-Version").value_or("");
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

        std::string methodHdr = httpReq.header("Mcp-Method").value_or("");
        if (methodHdr.empty()) {
            return "Missing required Mcp-Method header";
        }
        if (methodHdr != method) {
            return "Header mismatch: Mcp-Method '" + methodHdr + "' does not match body method '" + method + "'";
        }

        if (method == "tools/call" || method == "resources/read" || method == "prompts/get") {
            std::string rawNameHdr = httpReq.header("Mcp-Name").value_or("");
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
                        std::string hdrVal = httpReq.header(spec.headerName).value_or("");
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

    HttpRouteResponse handleMcpRoute(const HttpRouteRequest &httpReq) {
        auto ct = httpReq.header("Content-Type");
        if (!ct || !serve_detail::is_json_content_type(*ct)) {
            HttpRouteResponse r;
            r.status = 415;
            r.contentType = "application/json";
            r.body = "{\"error\":\"unsupported_media_type\"}";
            return r;
        }

        if (httpReq.body.size() > opts_.maxRequestBytes) {
            HttpRouteResponse r;
            r.status = 413;
            r.contentType = "application/json";
            r.body = "{\"error\":\"payload_too_large\"}";
            return r;
        }

        auto parsed = parse_json<Json>(httpReq.body);
        if (!parsed) {
            return HttpRouteResponse::json(
                400, *app_.makeErrorResponse(std::nullopt, McpErrorCode::ParseError, "Parse error"));
        }
        const Json &req = *parsed;
        if (!req.is_object()) {
            return HttpRouteResponse::json(
                400, *app_.makeErrorResponse(std::nullopt, McpErrorCode::InvalidRequest, "Invalid Request: expected JSON object"));
        }
        if (!req.contains("jsonrpc") || !req["jsonrpc"].is_string() || req["jsonrpc"].get<std::string>() != "2.0") {
            std::optional<Json> errId;
            if (req.contains("id") && mcp_detail::is_valid_rpc_id(req["id"]))
                errId = req["id"];
            return HttpRouteResponse::json(
                400, *app_.makeErrorResponse(errId, McpErrorCode::InvalidRequest, "Invalid Request"));
        }

        const bool hasId = req.contains("id");
        if (hasId && !mcp_detail::is_valid_rpc_id(req["id"])) {
            return HttpRouteResponse::json(
                400, *app_.makeErrorResponse(std::nullopt, McpErrorCode::InvalidRequest, "Invalid Request: id must be a non-null string or number"));
        }
        std::optional<Json> id = hasId ? std::optional<Json>(req["id"]) : std::nullopt;

        if (!req.contains("method") || !req["method"].is_string() || req["method"].get<std::string>().empty()) {
            return HttpRouteResponse::json(
                400, *app_.makeErrorResponse(id, McpErrorCode::InvalidRequest, "Invalid Request: method must be a non-empty string"));
        }
        std::string method = req["method"].get<std::string>();

        if (hasId && !mcp_detail::accepts_streamable_http(httpReq.header("Accept").value_or(""))) {
            Json err = json_obj();
            err["error"] = "not_acceptable";
            err["error_description"] = "Accept must include application/json and text/event-stream";
            return HttpRouteResponse::json(406, err);
        }

        Principal principal;
        const bool isDiscover = hasId && method == "server/discover";
        if (httpReq.unixSocket) {
            // Unix socket access is gated by socket file permissions; skip OAuth.
            principal = local_principal(app_);
        } else if (opts_.oauth.enabled && !(isDiscover && !opts_.oauth.protectDiscover)) {
            TokenValidationResult authRes = serve_detail::evaluate_bearer_auth(opts_.oauth, mcpEndpointUrl(), httpReq);
            if (authRes.status == TokenValidationResult::Status::ServiceUnavailable) {
                Json err = json_obj();
                err["error"] = "service_unavailable";
                err["error_description"] = authRes.errorDescription;
                return HttpRouteResponse::json(503, err);
            }
            if (authRes.status == TokenValidationResult::Status::InvalidToken) {
                return makeOAuthUnauthorized(authRes.requiredScopes, authRes.errorDescription);
            }
            if (authRes.status == TokenValidationResult::Status::InsufficientScope) {
                return makeOAuthInsufficientScope(authRes.requiredScopes, authRes.errorDescription);
            }
            principal = std::move(authRes.principal);
        }

        if (!hasId) {
            HttpRouteResponse r;
            r.status = 202;
            r.contentType = "application/json";
            return r;
        }

        if (method == "initialize") {
            return HttpRouteResponse::json(404, app_.dispatchMcpJson(req));
        }

        if (!req.contains("params") || !req["params"].is_object() ||
            !req["params"].contains("_meta") || !req["params"]["_meta"].is_object() ||
            !req["params"]["_meta"].contains("io.modelcontextprotocol/protocolVersion") ||
            !req["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"].is_string() ||
            !req["params"]["_meta"].contains("io.modelcontextprotocol/clientCapabilities") ||
            !req["params"]["_meta"]["io.modelcontextprotocol/clientCapabilities"].is_object()) {
            return HttpRouteResponse::json(400, app_.dispatchMcpJson(req));
        }
        if (auto hdrErr = validateHttpMcpHeaders(req, httpReq, method)) {
            return HttpRouteResponse::json(
                400, *app_.makeErrorResponse(id, McpErrorCode::HeaderMismatch, *hdrErr));
        }
        std::string reqVer = req["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"].get<std::string>();
        if (reqVer != kMcpProtocolVersion) {
            return HttpRouteResponse::json(400, app_.dispatchMcpJson(req));
        }

        if (method != "server/discover" && method != "tools/list" && method != "resources/list" &&
            method != "tools/call" && method != "resources/read") {
            return HttpRouteResponse::json(404, app_.dispatchMcpJson(req));
        }

        RequestContext ctx;
        ctx.principal = std::move(principal);
        Json resp = app_.dispatchMcpJson(req, ctx);

        int httpCode = 200;
        if (resp.contains("error") && resp["error"].is_object() &&
            resp["error"].contains("code") && resp["error"]["code"].is_int64()) {
            int rpcCode = static_cast<int>(resp["error"]["code"].get<int64_t>());
            if (rpcCode == McpErrorCode::InsufficientScope) {
                std::vector<std::string> reqScopes;
                if (resp["error"].contains("data") && resp["error"]["data"].is_object() &&
                    resp["error"]["data"].contains("requiredScopes") &&
                    resp["error"]["data"]["requiredScopes"].is_array()) {
                    for (const auto &sc : resp["error"]["data"]["requiredScopes"].get_array()) {
                        if (sc.is_string())
                            reqScopes.push_back(sc.get<std::string>());
                    }
                }
                std::string desc = resp["error"].contains("message") && resp["error"]["message"].is_string()
                                       ? resp["error"]["message"].get<std::string>()
                                       : "Insufficient scope";
                return makeOAuthInsufficientScope(reqScopes, desc);
            }
            httpCode = httpStatusForRpcError(rpcCode);
        }

        return HttpRouteResponse::json(httpCode, resp);
    }

    PluginApp &app_;
    McpHttpOptions opts_;
    std::unique_ptr<ServeHttpServer> server_;
    std::atomic<bool> running_{false};
    std::atomic<uint16_t> boundPort_{0};
};

/** Attaches standalone `serve` / `http` subcommands and CLI relay to PluginApp. */
inline void enable_http(PluginApp &app, McpHttpOptions defaults = {}) {
    enable_cli_relay(app);
    app.openApiDefaults.oauthEnabled = defaults.oauth.enabled;
    app.setHttpRunner([defaults = std::move(defaults)](ServeApp &targetBase, int argc, char **argv) -> int {
        auto &target = static_cast<PluginApp &>(targetBase);
        McpHttpOptions opts = defaults;
        std::string prog = target.key.empty() ? "app" : target.key;
        std::string subcmd = argc >= 2 ? std::string(argv[1]) : "http";
        auto usage = [&](const std::string &msg) {
            std::cerr << msg << "\n"
                      << "Usage: " << prog << " " << subcmd
                      << " [--host <addr>] [--port <n>] [--socket <path>] [--socket-mode <octal>] [--socket-group <name>]\n";
            return 2;
        };
        for (int i = 2; i < argc; ++i) {
            std::string_view arg = argv[i];
            bool known = arg == "--host" || arg == "--port" || arg == "--socket" ||
                         arg == "--socket-mode" || arg == "--socket-group";
            if (!known || i + 1 >= argc)
                return usage("Unknown or incomplete option: " + std::string(arg));
            std::string val = argv[++i];
            char *endPtr = nullptr;
            if (arg == "--host") {
                if (!serve_detail::is_valid_ip_literal(val))
                    return usage("Invalid --host value (IP literal required): " + val);
                opts.host = val;
            } else if (arg == "--port") {
                long parsedPort = std::strtol(val.c_str(), &endPtr, 10);
                if (endPtr == val.c_str() || *endPtr != '\0' || parsedPort < 0 || parsedPort > 65535)
                    return usage("Invalid --port value: " + val);
                opts.port = static_cast<int>(parsedPort);
            } else if (arg == "--socket") {
                std::string pathErr;
                if (!serve_daemon_detail::validate_sun_path(val, pathErr))
                    return usage("Invalid --socket value: " + pathErr);
                opts.socketPath = val;
            } else if (arg == "--socket-mode") {
                long mode = std::strtol(val.c_str(), &endPtr, 8);
                if (endPtr == val.c_str() || *endPtr != '\0' || mode < 0 || mode > 0777)
                    return usage("Invalid --socket-mode value (octal 0-777): " + val);
                opts.socketMode = static_cast<mode_t>(mode);
            } else {
                if (val.empty())
                    return usage("Invalid --socket-group value: empty name");
                opts.socketGroup = val;
            }
        }

        const bool hasPort = opts.port.has_value();
        const std::string socketPath = opts.socketPath;
        McpHttpServer server(target, std::move(opts));
        std::string errMsg;
        if (!server.start(&errMsg)) {
            std::cerr << "HTTP server failed to start: " << errMsg << "\n";
            return 1;
        }
        if (hasPort || socketPath.empty())
            std::cerr << prog << " listening on " << server.mcpEndpointUrl() << "\n";
        if (!socketPath.empty())
            std::cerr << prog << " listening on unix:" << socketPath << "\n";
        std::cerr << std::flush;
        return server.runUntilSignal();
    });
}

inline void enable_http(PluginApp &app, ServeHttpOptions defaults) {
    enable_http(app, McpHttpOptions(std::move(defaults)));
}

} // namespace slimr
