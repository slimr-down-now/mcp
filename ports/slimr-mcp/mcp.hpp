#pragma once
#include <serve.hpp>

#include <algorithm>
#include <any>
#include <cctype>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace slimr {

inline constexpr std::string_view kMcpProtocolVersion = "2026-07-28";

namespace McpErrorCode {
inline constexpr int ParseError = -32700;
inline constexpr int InvalidRequest = -32600;
inline constexpr int MethodNotFound = -32601;
inline constexpr int InvalidParams = -32602;
inline constexpr int InternalError = -32603;
inline constexpr int HeaderMismatch = -32020;
inline constexpr int MissingRequiredClientCapability = -32021;
inline constexpr int UnsupportedProtocolVersion = -32022;
inline constexpr int RequestCancelled = ServeErrorCode::RequestCancelled;
inline constexpr int InsufficientScope = ServeErrorCode::InsufficientScope;
} // namespace McpErrorCode

namespace mcp_detail {

inline std::string id_key(const Json &id) {
    if (id.is_string()) {
        return "s:" + id.get<std::string>();
    }
    return "n:" + slimr::write_json(id);
}

inline bool is_valid_rpc_id(const Json &id) noexcept {
    if (id.is_string())
        return true;
    if (id.is_number() && !id.is_boolean())
        return true;
    return false;
}

inline bool is_valid_progress_token(const Json &tok) noexcept {
    return is_valid_rpc_id(tok);
}

} // namespace mcp_detail

/** Protocol-level JSON-RPC exception propagated directly as a JSON-RPC error response. */
class McpProtocolException : public std::runtime_error {
  public:
    int code{McpErrorCode::InternalError};
    Json data = Json{};

    McpProtocolException(int errCode, std::string message, Json errData = Json{})
        : std::runtime_error(std::move(message)), code(errCode), data(std::move(errData)) {}
};

using McpNotificationChannel = NotificationChannel;

inline bool has_client_capability(const RequestContext &ctx, std::string_view cap) noexcept {
    return ctx.clientCapabilities.is_object() && ctx.clientCapabilities.contains(cap);
}

inline void require_client_capability(const RequestContext &ctx,
                                      std::string_view cap,
                                      const Json &requirement = json_obj()) {
    if (has_client_capability(ctx, cap))
        return;
    Json reqCaps = json_obj();
    reqCaps[std::string(cap)] = requirement;
    Json data = json_obj();
    data["requiredCapabilities"] = std::move(reqCaps);
    throw McpProtocolException(
        McpErrorCode::MissingRequiredClientCapability,
        "Client lacks required capability: " + std::string(cap),
        std::move(data));
}

struct ContentBlock {
    std::string type;     // "text", "image", "audio", "resource"
    std::string text;     // for type == "text"
    std::string data;     // base64 for image/audio
    std::string mimeType; // e.g., "image/jpeg", "audio/ogg"
};

inline Json content_to_json(const ContentBlock &c) {
    Json j = json_obj();
    j["type"] = c.type;
    if (c.type == "text") {
        j["text"] = c.text;
    } else if (c.type == "image" || c.type == "audio") {
        j["data"] = c.data;
        j["mimeType"] = c.mimeType;
    }
    return j;
}

struct McpResult {
    std::vector<ContentBlock> content;
    bool isError{false};
    std::optional<Json> structuredContent;
    bool isInputRequired{false};
    Json inputRequests = json_obj();
    std::optional<std::string> requestState;

    static McpResult text(std::string s) {
        McpResult r;
        r.content.push_back(ContentBlock{.type = "text", .text = std::move(s), .data = {}, .mimeType = {}});
        return r;
    }

    static McpResult structured(Json obj, std::string fallbackText = "") {
        McpResult r;
        std::string txt = fallbackText.empty() ? slimr::write_json(obj) : std::move(fallbackText);
        r.content.push_back(ContentBlock{.type = "text", .text = std::move(txt), .data = {}, .mimeType = {}});
        r.structuredContent = std::move(obj);
        return r;
    }

    static McpResult image(std::string base64Data, std::string mimeType) {
        McpResult r;
        r.content.push_back(ContentBlock{.type = "image", .text = {}, .data = std::move(base64Data), .mimeType = std::move(mimeType)});
        return r;
    }

    static McpResult audio(std::string base64Data, std::string mimeType) {
        McpResult r;
        r.content.push_back(ContentBlock{.type = "audio", .text = {}, .data = std::move(base64Data), .mimeType = std::move(mimeType)});
        return r;
    }

    static McpResult error(std::string msg) {
        McpResult r;
        r.isError = true;
        r.content.push_back(ContentBlock{.type = "text", .text = std::move(msg), .data = {}, .mimeType = {}});
        return r;
    }

    static McpResult inputRequired(Json requests, std::optional<std::string> state = std::nullopt) {
        McpResult r;
        r.isInputRequired = true;
        r.inputRequests = std::move(requests);
        r.requestState = std::move(state);
        return r;
    }

    Json toJson() const {
        Json j = json_obj();
        if (isInputRequired) {
            j["resultType"] = "input_required";
            if (inputRequests.is_object() && !inputRequests.empty()) {
                j["inputRequests"] = inputRequests;
            }
            if (requestState.has_value()) {
                j["requestState"] = *requestState;
            }
            return j;
        }
        j["resultType"] = "complete";
        Json::array_t arr;
        arr.reserve(content.size());
        for (const auto &c : content) {
            arr.push_back(content_to_json(c));
        }
        j["content"] = json_arr(std::move(arr));
        if (structuredContent.has_value()) {
            j["structuredContent"] = *structuredContent;
        }
        j["isError"] = isError;
        return j;
    }
};

struct Resource {
    std::string uri;
    std::string name;
    std::string description;
    std::string mimeType = "text/plain";
    std::function<std::string()> load;
    int64_t ttlMs{0};
    std::string cacheScope{"private"};
    std::vector<std::string> requiredScopes;
};

struct CachePolicy {
    int64_t discoverTtlMs{3600000};
    std::string discoverCacheScope{"public"};
    int64_t toolsListTtlMs{3600000};
    std::string toolsListCacheScope{"public"};
    int64_t resourcesListTtlMs{3600000};
    std::string resourcesListCacheScope{"public"};
};

inline ServeResult mcp_result_to_serve_result(McpResult mr, bool allowInputRequired = true) {
    if (mr.isError) {
        std::string msg;
        for (const auto &b : mr.content) {
            if (b.type == "text" && !b.text.empty()) {
                msg = b.text;
                break;
            }
        }
        if (msg.empty()) {
            msg = "Tool error";
        }
        return ServeResult::withExtension(std::move(mr), ServeResult::error(std::move(msg), 400));
    }
    if (mr.isInputRequired) {
        if (!allowInputRequired) {
            return ServeResult::withExtension(
                std::move(mr),
                ServeResult::error("Tool requires interactive input; use /mcp", 400));
        }
        Json j = mr.toJson();
        return ServeResult::withExtension(std::move(mr), ServeResult::json(std::move(j)));
    }
    if (mr.structuredContent.has_value()) {
        Json sc = *mr.structuredContent;
        return ServeResult::withExtension(std::move(mr), ServeResult::json(std::move(sc)));
    }
    if (mr.content.size() == 1 && mr.content[0].type == "text") {
        std::string txt = mr.content[0].text;
        return ServeResult::withExtension(std::move(mr), ServeResult::text(std::move(txt)));
    }
    Json::array_t arr;
    arr.reserve(mr.content.size());
    for (const auto &b : mr.content) {
        arr.push_back(content_to_json(b));
    }
    return ServeResult::withExtension(std::move(mr), ServeResult::json(json_arr(std::move(arr))));
}

inline McpResult serve_result_to_mcp_result(const ServeResult &sr) {
    if (sr.extension.has_value()) {
        if (const auto *mr = std::any_cast<McpResult>(&sr.extension)) {
            return *mr;
        }
    }
    if (sr.isError) {
        return McpResult::error(sr.errorMessage);
    }
    if (sr.isTyped) {
        return McpResult::structured(sr.data);
    }
    if (sr.isJson) {
        return McpResult::text(slimr::write_json(sr.data));
    }
    return McpResult::text(sr.textValue);
}

/** MCP 2026-07-28 protocol adapter over slimr::ServeApp. */
class PluginApp : public ServeApp {
  public:
    std::vector<Resource> resources;
    CachePolicy cachePolicy;

    PluginApp() : ServeApp() {}
    PluginApp(PluginApp &&) noexcept = default;
    PluginApp &operator=(PluginApp &&) noexcept = default;
    explicit PluginApp(std::string appKey, std::string appVer = "1.0.0")
        : ServeApp(std::move(appKey), std::move(appVer)) {}

    ServeApp &serve() noexcept { return *this; }
    const ServeApp &serve() const noexcept { return *this; }

    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method)
    PluginApp &requireEnv(std::string name, std::string purpose) {
        ServeApp::requireEnv(std::move(name), std::move(purpose));
        return *this;
    }

    template <typename InputStruct, typename Handler>
    PluginApp &tool(std::string name, std::string desc, ToolOptions opts, Handler &&h) {
        using InvokeRet = typename std::conditional_t<
            std::invocable<Handler &, const InputStruct &, const RequestContext &>,
            std::invoke_result<Handler &, const InputStruct &, const RequestContext &>,
            std::invoke_result<Handler &, const InputStruct &>>::type;
        using PlainRet = std::remove_cvref_t<InvokeRet>;

        if constexpr (std::is_same_v<PlainRet, McpResult>) {
            auto wrapped = [h = std::forward<Handler>(h)](const InputStruct &in, const RequestContext &ctx) mutable -> ServeResult {
                try {
                    McpResult mr = [&]() {
                        if constexpr (std::invocable<Handler &, const InputStruct &, const RequestContext &>) {
                            return h(in, ctx);
                        } else {
                            return h(in);
                        }
                    }();
                    return mcp_result_to_serve_result(std::move(mr), !ctx.protocolVersion.empty());
                } catch (const McpProtocolException &pe) {
                    throw ServeException(pe.code, pe.what(), pe.data, 400);
                }
            };
            ServeApp::tool<InputStruct>(std::move(name), std::move(desc), std::move(opts), std::move(wrapped));
        } else {
            auto wrapped = [h = std::forward<Handler>(h)](const InputStruct &in, const RequestContext &ctx) mutable -> InvokeRet {
                try {
                    if constexpr (std::invocable<Handler &, const InputStruct &, const RequestContext &>) {
                        return h(in, ctx);
                    } else {
                        return h(in);
                    }
                } catch (const McpProtocolException &pe) {
                    throw ServeException(pe.code, pe.what(), pe.data, 400);
                }
            };
            ServeApp::tool<InputStruct>(std::move(name), std::move(desc), std::move(opts), std::move(wrapped));
        }
        return *this;
    }

    template <typename InputStruct, typename Handler>
    PluginApp &tool(std::string name, std::string desc, ToolAnnotations annotations, Handler &&h) {
        return tool<InputStruct>(std::move(name), std::move(desc), ToolOptions{.annotations = std::move(annotations)}, std::forward<Handler>(h));
    }

    template <typename InputStruct, typename Handler>
    PluginApp &tool(std::string name, std::string desc, Handler &&h) {
        return tool<InputStruct>(std::move(name), std::move(desc), ToolOptions{}, std::forward<Handler>(h));
    }

    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method)
    PluginApp &registerTool(ToolDef def) {
        ServeApp::registerTool(std::move(def));
        return *this;
    }

    Json buildMcpResourceReadTool() const {
        Json tool = json_obj();
        tool["name"] = "mcp_resource_read";
        tool["description"] = "Read an MCP resource by URI directly through a tool call (fallback for hosts that do not support MCP resources natively).";
        Json::array_t enum_uris;
        for (const auto &r : resources) {
            enum_uris.emplace_back(r.uri);
        }
        Json uriProp = json_obj();
        uriProp["type"] = "string";
        uriProp["enum"] = json_arr(std::move(enum_uris));
        Json props = json_obj();
        props["uri"] = std::move(uriProp);
        Json inputSchema = json_obj();
        inputSchema["type"] = "object";
        inputSchema["properties"] = std::move(props);
        inputSchema["required"] = json_arr(Json::array_t{Json("uri")});
        inputSchema["additionalProperties"] = false;
        tool["inputSchema"] = std::move(inputSchema);
        tool["annotations"] = ToolAnnotations{.readOnlyHint = true, .openWorldHint = false}.toJson();
        return tool;
    }

    /** Validates HTTP-bound header expectations against the parsed body; throws nothing and returns std::nullopt when valid. */
    std::optional<Json> makeErrorResponse(const std::optional<Json> &id, int code, std::string message, Json data = Json{}) const {
        Json err = json_obj();
        err["code"] = code;
        err["message"] = std::move(message);
        if (!data.is_null() && !(data.is_object() && data.empty() && code != McpErrorCode::MissingRequiredClientCapability)) {
            err["data"] = std::move(data);
        }
        Json resp = json_obj();
        resp["jsonrpc"] = "2.0";
        if (id.has_value()) {
            resp["id"] = *id;
        }
        resp["error"] = std::move(err);
        return resp;
    }

    const Resource *findResource(std::string_view uri) const noexcept {
        for (const auto &r : resources) {
            if (r.uri == uri)
                return &r;
        }
        return nullptr;
    }

    Json dispatchMcpJson(const Json &req, const RequestContext &baseCtx = RequestContext{}) const {
        if (!req.is_object() || !req.contains("jsonrpc") || !req["jsonrpc"].is_string() ||
            req["jsonrpc"].get<std::string>() != "2.0") {
            std::optional<Json> errId;
            if (req.is_object() && req.contains("id") && mcp_detail::is_valid_rpc_id(req["id"])) {
                errId = req["id"];
            }
            return *makeErrorResponse(errId, McpErrorCode::InvalidRequest, "Invalid Request");
        }

        const bool hasId = req.contains("id");
        if (hasId && !mcp_detail::is_valid_rpc_id(req["id"])) {
            return *makeErrorResponse(std::nullopt, McpErrorCode::InvalidRequest, "Invalid Request: id must be a non-null string or number");
        }
        std::optional<Json> id = hasId ? std::optional<Json>(req["id"]) : std::nullopt;

        if (!req.contains("method") || !req["method"].is_string() || req["method"].get<std::string>().empty()) {
            return *makeErrorResponse(id, McpErrorCode::InvalidRequest, "Invalid Request: method must be a non-empty string");
        }
        std::string method = req["method"].get<std::string>();

        if (!hasId) {
            return Json{};
        }

        if (method == "initialize") {
            return *makeErrorResponse(
                id,
                McpErrorCode::MethodNotFound,
                "Method 'initialize' is not supported; this server implements MCP 2026-07-28 only (use 'server/discover' and per-request '_meta')");
        }

        if (!req.contains("params") || !req["params"].is_object()) {
            return *makeErrorResponse(id, McpErrorCode::InvalidParams, "Invalid params: params object with _meta is required");
        }
        const Json &params = req["params"];
        if (!params.contains("_meta") || !params["_meta"].is_object()) {
            return *makeErrorResponse(id, McpErrorCode::InvalidParams, "Invalid params: params._meta object is required");
        }
        const Json &meta = params["_meta"];
        if (!meta.contains("io.modelcontextprotocol/protocolVersion") ||
            !meta["io.modelcontextprotocol/protocolVersion"].is_string()) {
            return *makeErrorResponse(id, McpErrorCode::InvalidParams, "Invalid params: _meta['io.modelcontextprotocol/protocolVersion'] string is required");
        }
        std::string reqVersion = meta["io.modelcontextprotocol/protocolVersion"].get<std::string>();
        if (reqVersion != kMcpProtocolVersion) {
            Json data = json_obj();
            data["supported"] = json_arr(Json::array_t{Json(std::string(kMcpProtocolVersion))});
            data["requested"] = reqVersion;
            return *makeErrorResponse(id, McpErrorCode::UnsupportedProtocolVersion,
                                      "Unsupported protocol version: " + reqVersion,
                                      std::move(data));
        }

        if (!meta.contains("io.modelcontextprotocol/clientCapabilities") ||
            !meta["io.modelcontextprotocol/clientCapabilities"].is_object()) {
            return *makeErrorResponse(id, McpErrorCode::InvalidParams, "Invalid params: _meta['io.modelcontextprotocol/clientCapabilities'] object is required");
        }

        if (meta.contains("io.modelcontextprotocol/clientInfo") &&
            !meta["io.modelcontextprotocol/clientInfo"].is_object()) {
            return *makeErrorResponse(id, McpErrorCode::InvalidParams, "Invalid params: _meta['io.modelcontextprotocol/clientInfo'] must be an object");
        }

        RequestContext ctx = baseCtx;
        ctx.protocolVersion = reqVersion;
        ctx.requestId = id;
        ctx.requestMeta = meta;
        ctx.clientCapabilities = meta["io.modelcontextprotocol/clientCapabilities"];
        if (meta.contains("io.modelcontextprotocol/clientInfo")) {
            ctx.clientInfo = meta["io.modelcontextprotocol/clientInfo"];
        }
        if (meta.contains("progressToken")) {
            if (!mcp_detail::is_valid_progress_token(meta["progressToken"])) {
                return *makeErrorResponse(id, McpErrorCode::InvalidParams, "Invalid params: _meta.progressToken must be a string or number");
            }
            ctx.progressToken = meta["progressToken"];
        }
        if (meta.contains("io.modelcontextprotocol/logLevel")) {
            if (!meta["io.modelcontextprotocol/logLevel"].is_string() ||
                serve_detail::log_level_severity(meta["io.modelcontextprotocol/logLevel"].get<std::string>()) < 0) {
                return *makeErrorResponse(id, McpErrorCode::InvalidParams, "Invalid params: invalid _meta['io.modelcontextprotocol/logLevel']");
            }
            ctx.minLogLevel = meta["io.modelcontextprotocol/logLevel"].get<std::string>();
        }
        if (ctx.notificationChannel) {
            auto rawChannel = ctx.notificationChannel;
            auto progressToken = ctx.progressToken;
            auto minLogLevel = ctx.minLogLevel;
            ctx.notificationChannel = std::make_shared<NotificationChannel>(
                [rawChannel, progressToken, minLogLevel](const Json &ev) {
                    if (!ev.is_object() || !ev.contains("type") || !ev["type"].is_string()) {
                        rawChannel->emit(ev);
                        return;
                    }
                    const std::string type = ev["type"].get<std::string>();
                    if (type == "progress") {
                        if (!progressToken.has_value())
                            return;
                        Json params = json_obj();
                        params["progressToken"] = *progressToken;
                        if (ev.contains("progress"))
                            params["progress"] = ev["progress"];
                        if (ev.contains("total"))
                            params["total"] = ev["total"];
                        if (ev.contains("message"))
                            params["message"] = ev["message"];
                        Json notif = json_obj();
                        notif["jsonrpc"] = "2.0";
                        notif["method"] = "notifications/progress";
                        notif["params"] = std::move(params);
                        rawChannel->emit(notif);
                        return;
                    }
                    if (type == "log") {
                        if (!minLogLevel.has_value())
                            return;
                        Json params = json_obj();
                        if (ev.contains("level"))
                            params["level"] = ev["level"];
                        if (ev.contains("logger"))
                            params["logger"] = ev["logger"];
                        if (ev.contains("data"))
                            params["data"] = ev["data"];
                        Json notif = json_obj();
                        notif["jsonrpc"] = "2.0";
                        notif["method"] = "notifications/message";
                        notif["params"] = std::move(params);
                        rawChannel->emit(notif);
                        return;
                    }
                    rawChannel->emit(ev);
                });
        }
        if (params.contains("inputResponses")) {
            if (!params["inputResponses"].is_object()) {
                return *makeErrorResponse(id, McpErrorCode::InvalidParams, "Invalid params: inputResponses must be an object");
            }
            ctx.inputResponses = params["inputResponses"];
        }
        if (params.contains("requestState")) {
            if (!params["requestState"].is_string()) {
                return *makeErrorResponse(id, McpErrorCode::InvalidParams, "Invalid params: requestState must be a string");
            }
            ctx.requestState = params["requestState"].get<std::string>();
        }

        if (method == "server/discover") {
            return makeResultResponse(*id, buildDiscoverResult());
        }

        if (method == "tools/list") {
            Json::array_t toolList;
            for (const auto &[name, def] : tools()) {
                (void)name;
                Json t = json_obj();
                t["name"] = def.name;
                t["description"] = def.description;
                t["inputSchema"] = def.inputSchema;
                if (def.outputSchema.has_value()) {
                    t["outputSchema"] = *def.outputSchema;
                }
                auto ann = def.annotations.toJson();
                if (!ann.empty()) {
                    t["annotations"] = std::move(ann);
                }
                toolList.push_back(std::move(t));
            }
            if (!resources.empty() && !tools().contains("mcp_resource_read")) {
                toolList.push_back(buildMcpResourceReadTool());
            }
            Json result = json_obj();
            result["resultType"] = "complete";
            result["tools"] = json_arr(std::move(toolList));
            result["ttlMs"] = cachePolicy.toolsListTtlMs;
            result["cacheScope"] = cachePolicy.toolsListCacheScope;
            attachServerMeta(result);
            return makeResultResponse(*id, std::move(result));
        }

        if (method == "tools/call") {
            if (!params.contains("name") || !params["name"].is_string() || params["name"].get<std::string>().empty()) {
                return *makeErrorResponse(id, McpErrorCode::InvalidParams, "Invalid params: name required");
            }
            std::string toolName = params["name"].get<std::string>();
            Json args = json_obj();
            if (params.contains("arguments")) {
                if (!params["arguments"].is_object()) {
                    return *makeErrorResponse(id, McpErrorCode::InvalidParams, "Invalid params: arguments must be an object");
                }
                args = params["arguments"];
            }

            if (toolName == "mcp_resource_read") {
                if (!args.contains("uri") || !args["uri"].is_string() || args["uri"].get<std::string>().empty()) {
                    return *makeErrorResponse(id, McpErrorCode::InvalidParams, "Invalid params: uri required");
                }
                std::string uri = args["uri"].get<std::string>();
                const Resource *r = findResource(uri);
                if (!r) {
                    return *makeErrorResponse(id, McpErrorCode::InvalidParams, "Unknown resource URI: " + uri);
                }
                if (auto authErr = checkAuthorization(ctx.principal, r->requiredScopes, "resources/read:" + uri, id)) {
                    return *authErr;
                }
                std::unique_lock<std::mutex> execLock;
                if (concurrency == ConcurrencyPolicy::Serialized) {
                    execLock = std::unique_lock<std::mutex>(execMutex());
                }
                if (ctx.cancellation.isCancelled()) {
                    return *makeErrorResponse(id, McpErrorCode::RequestCancelled, "Request cancelled");
                }
                try {
                    std::string content = r->load ? r->load() : "";
                    Json result = McpResult::text(std::move(content)).toJson();
                    attachServerMeta(result);
                    return makeResultResponse(*id, std::move(result));
                } catch (const McpProtocolException &pe) {
                    return *makeErrorResponse(id, pe.code, pe.what(), pe.data);
                } catch (const ServeException &se) {
                    return *makeErrorResponse(id, se.code, se.what(), se.data);
                } catch (const std::exception &e) {
                    Json result = McpResult::error(redact_sensitive(e.what())).toJson();
                    attachServerMeta(result);
                    return makeResultResponse(*id, std::move(result));
                }
            }

            const ToolDef *def = findTool(toolName);
            if (!def) {
                return *makeErrorResponse(id, McpErrorCode::InvalidParams, "Tool not found: " + toolName);
            }
            if (auto authErr = checkAuthorization(ctx.principal, def->requiredScopes, "tools/call:" + def->name, id)) {
                return *authErr;
            }

            std::unique_lock<std::mutex> execLock;
            if (!def->threadSafe && concurrency == ConcurrencyPolicy::Serialized) {
                execLock = std::unique_lock<std::mutex>(execMutex());
            }
            if (ctx.cancellation.isCancelled()) {
                return *makeErrorResponse(id, McpErrorCode::RequestCancelled, "Request cancelled");
            }
            try {
                ServeResult callRes = def->handler(args, ctx);
                McpResult mcpRes = serve_result_to_mcp_result(callRes);
                Json result = mcpRes.toJson();
                attachServerMeta(result);
                return makeResultResponse(*id, std::move(result));
            } catch (const McpProtocolException &pe) {
                return *makeErrorResponse(id, pe.code, pe.what(), pe.data);
            } catch (const ServeException &se) {
                return *makeErrorResponse(id, se.code, se.what(), se.data);
            }
        }

        if (method == "resources/list") {
            Json::array_t resList;
            for (const auto &r : resources) {
                Json item = json_obj();
                item["uri"] = r.uri;
                item["name"] = r.name;
                item["description"] = r.description;
                item["mimeType"] = r.mimeType;
                resList.push_back(std::move(item));
            }
            Json result = json_obj();
            result["resultType"] = "complete";
            result["resources"] = json_arr(std::move(resList));
            result["ttlMs"] = cachePolicy.resourcesListTtlMs;
            result["cacheScope"] = cachePolicy.resourcesListCacheScope;
            attachServerMeta(result);
            return makeResultResponse(*id, std::move(result));
        }

        if (method == "resources/read") {
            if (!params.contains("uri") || !params["uri"].is_string() || params["uri"].get<std::string>().empty()) {
                return *makeErrorResponse(id, McpErrorCode::InvalidParams, "Invalid params: uri required");
            }
            std::string uri = params["uri"].get<std::string>();
            const Resource *r = findResource(uri);
            if (!r) {
                return *makeErrorResponse(id, McpErrorCode::InvalidParams, "Unknown resource URI: " + uri);
            }
            if (auto authErr = checkAuthorization(ctx.principal, r->requiredScopes, "resources/read:" + uri, id)) {
                return *authErr;
            }
            std::unique_lock<std::mutex> execLock;
            if (concurrency == ConcurrencyPolicy::Serialized) {
                execLock = std::unique_lock<std::mutex>(execMutex());
            }
            if (ctx.cancellation.isCancelled()) {
                return *makeErrorResponse(id, McpErrorCode::RequestCancelled, "Request cancelled");
            }
            try {
                std::string text = r->load ? r->load() : "";
                Json::array_t contents;
                Json c = json_obj();
                c["uri"] = r->uri;
                c["mimeType"] = r->mimeType;
                c["text"] = std::move(text);
                contents.push_back(std::move(c));

                Json result = json_obj();
                result["resultType"] = "complete";
                result["contents"] = json_arr(std::move(contents));
                bool mrtrRetry = ctx.inputResponses.has_value() || ctx.requestState.has_value();
                result["ttlMs"] = mrtrRetry ? 0 : r->ttlMs;
                result["cacheScope"] = mrtrRetry ? std::string("private") : r->cacheScope;
                attachServerMeta(result);
                return makeResultResponse(*id, std::move(result));
            } catch (const McpProtocolException &pe) {
                return *makeErrorResponse(id, pe.code, pe.what(), pe.data);
            } catch (const ServeException &se) {
                return *makeErrorResponse(id, se.code, se.what(), se.data);
            } catch (const std::exception &e) {
                return *makeErrorResponse(id, McpErrorCode::InternalError,
                                          "Resource load failed: " + redact_sensitive(e.what()));
            }
        }

        return *makeErrorResponse(id, McpErrorCode::MethodNotFound, "Method not found: " + method);
    }

    int run(int argc, char *argv[], std::ostream &out = std::cout, std::ostream &err = std::cerr, std::istream &in = std::cin) {
        return slimr::run_cli(*this, argc, argv, out, err, in);
    }

    void printHelp(std::ostream &os = std::cout) const {
        slimr::print_cli_help(*this, os);
    }

  private:
    Json serverInfoJson() const {
        Json info = json_obj();
        info["name"] = key.empty() ? "plugin-app" : key;
        info["version"] = version;
        return info;
    }

    void attachServerMeta(Json &resultObj) const {
        Json meta = json_obj();
        if (resultObj.contains("_meta") && resultObj["_meta"].is_object()) {
            meta = resultObj["_meta"];
        }
        meta["io.modelcontextprotocol/serverInfo"] = serverInfoJson();
        resultObj["_meta"] = std::move(meta);
    }

    Json makeResultResponse(const Json &id, Json resultObj) const {
        Json resp = json_obj();
        resp["jsonrpc"] = "2.0";
        resp["id"] = id;
        resp["result"] = std::move(resultObj);
        return resp;
    }

    Json buildDiscoverResult() const {
        Json caps = json_obj();
        if (!tools().empty() || !resources.empty()) {
            caps["tools"] = json_obj();
        }
        if (!resources.empty()) {
            caps["resources"] = json_obj();
        }
        Json result = json_obj();
        result["resultType"] = "complete";
        result["supportedVersions"] = json_arr(Json::array_t{Json(std::string(kMcpProtocolVersion))});
        result["capabilities"] = std::move(caps);
        if (!instructions.empty()) {
            result["instructions"] = instructions;
        }
        if (apiVersion > 0) {
            result["apiVersion"] = apiVersion;
        }
        result["ttlMs"] = cachePolicy.discoverTtlMs;
        result["cacheScope"] = cachePolicy.discoverCacheScope;
        attachServerMeta(result);
        return result;
    }

    std::optional<Json> checkAuthorization(const Principal &principal,
                                           std::span<const std::string> toolScopes,
                                           std::string_view operation,
                                           const std::optional<Json> &id) const {
        auto authErr = slimr::checkAuthorization(*this, principal, toolScopes, operation);
        if (!authErr) {
            return std::nullopt;
        }
        Json data = json_obj();
        if (!authErr->requiredScopes.empty()) {
            Json::array_t arr;
            for (const auto &m : authErr->requiredScopes)
                arr.emplace_back(m);
            data["requiredScopes"] = json_arr(std::move(arr));
        }
        return makeErrorResponse(id, authErr->code, authErr->message, std::move(data));
    }
};

} // namespace slimr
