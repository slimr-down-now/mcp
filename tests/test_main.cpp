#define SLIMR_TEST_MAIN
#include "mcp.hpp"
#include "test.hpp"
#ifdef SLIMR_MCP_HAS_HTTP
#include "mcp_http.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <thread>

using namespace slimr;

struct AddInput {
    Field<int, "First operand"> a;
    Field<int, "Second operand"> b;
};

struct SecretInput {
    Field<std::string, "Message"> msg;
};

namespace {

Json make_meta(std::string_view version = kMcpProtocolVersion,
               Json caps = json_obj()) {
    Json meta = json_obj();
    meta["io.modelcontextprotocol/protocolVersion"] = std::string(version);
    meta["io.modelcontextprotocol/clientCapabilities"] = std::move(caps);
    return meta;
}

Json make_req(Json id,
              std::string_view method,
              Json params = json_obj(),
              std::string_view version = kMcpProtocolVersion,
              Json caps = json_obj()) {
    params["_meta"] = make_meta(version, std::move(caps));
    Json req = json_obj();
    req["jsonrpc"] = "2.0";
    req["id"] = std::move(id);
    req["method"] = std::string(method);
    req["params"] = std::move(params);
    return req;
}

} // namespace

TEST_SUITE("MCP 2026-07-28 Protocol, Context & Stdio") {
    TEST_CASE("PluginApp server/discover and tools/list") {
        PluginApp app("test-app", "1.2.3");
        app.instructions = "Test instructions for agent";
        app.resources.push_back(Resource{
            .uri = "test://notes",
            .name = "test_resource",
            .description = "Test resource text",
            .mimeType = "text/plain",
            .load = []() { return "Hello from resource!"; },
            .ttlMs = 60000,
            .cacheScope = "public"});

        app.tool<AddInput>("add", "Adds two numbers", [](const AddInput &in) {
            Json res = json_obj();
            res["sum"] = in.a.get() + in.b.get();
            return res;
        });

        // server/discover
        Json discoverResp = app.dispatchMcpJson(make_req(1, "server/discover"));
        CHECK(discoverResp["jsonrpc"].get<std::string>() == "2.0");
        CHECK(discoverResp["id"].get<int64_t>() == 1);
        const Json &dr = discoverResp["result"];
        CHECK(dr["resultType"].get<std::string>() == "complete");
        CHECK(dr["supportedVersions"].get_array().size() == 1);
        CHECK(dr["supportedVersions"][0].get<std::string>() == "2026-07-28");
        CHECK(dr["capabilities"].contains("tools"));
        CHECK(dr["capabilities"].contains("resources"));
        CHECK(dr["_meta"]["io.modelcontextprotocol/serverInfo"]["name"].get<std::string>() == "test-app");
        CHECK(dr["_meta"]["io.modelcontextprotocol/serverInfo"]["version"].get<std::string>() == "1.2.3");
        CHECK(dr["instructions"].get<std::string>() == "Test instructions for agent");
        CHECK(dr["ttlMs"].get<int64_t>() >= 0);
        CHECK(dr["cacheScope"].get<std::string>() == "public");

        // tools/list (must include 'add', 'mcp_resource_read', resultType, and CacheableResult hints)
        Json listResp = app.dispatchMcpJson(make_req(2, "tools/list"));
        const Json &lr = listResp["result"];
        CHECK(lr["resultType"].get<std::string>() == "complete");
        CHECK(lr["ttlMs"].get<int64_t>() >= 0);
        CHECK(lr["cacheScope"].get<std::string>() == "public");
        auto tools = lr["tools"].get_array();
        CHECK(tools.size() == 2);

        bool foundAdd = false;
        bool foundResourceRead = false;
        for (const auto &t : tools) {
            std::string tName = t["name"].get<std::string>();
            if (tName == "add")
                foundAdd = true;
            if (tName == "mcp_resource_read")
                foundResourceRead = true;
        }
        CHECK(foundAdd);
        CHECK(foundResourceRead);
    }

    TEST_CASE("Rejects legacy initialize, ping, invalid meta, and unsupported protocol versions") {
        PluginApp app("strict-app", "2.0.0");

        // Legacy initialize -> -32601 Method not found with July-only guidance
        Json initReq = json_obj();
        initReq["jsonrpc"] = "2.0";
        initReq["id"] = 1;
        initReq["method"] = "initialize";
        initReq["params"]["protocolVersion"] = "2025-11-25";
        Json initResp = app.dispatchMcpJson(initReq);
        CHECK(initResp["error"]["code"].get<int64_t>() == McpErrorCode::MethodNotFound);
        CHECK(initResp["error"]["message"].get<std::string>().find("2026-07-28") != std::string::npos);

        // ping removed in July 2026 -> -32601
        Json pingResp = app.dispatchMcpJson(make_req(2, "ping"));
        CHECK(pingResp["error"]["code"].get<int64_t>() == McpErrorCode::MethodNotFound);

        // Unsupported protocol version -> -32022 with supported[] and requested
        Json unsupResp = app.dispatchMcpJson(make_req(3, "server/discover", json_obj(), "2025-11-25"));
        CHECK(unsupResp["error"]["code"].get<int64_t>() == McpErrorCode::UnsupportedProtocolVersion);
        CHECK(unsupResp["error"]["data"]["requested"].get<std::string>() == "2025-11-25");
        CHECK(unsupResp["error"]["data"]["supported"][0].get<std::string>() == "2026-07-28");

        // Missing _meta -> -32602
        Json noMeta = json_obj();
        noMeta["jsonrpc"] = "2.0";
        noMeta["id"] = 4;
        noMeta["method"] = "tools/list";
        noMeta["params"] = json_obj();
        Json noMetaResp = app.dispatchMcpJson(noMeta);
        CHECK(noMetaResp["error"]["code"].get<int64_t>() == McpErrorCode::InvalidParams);

        // Missing clientCapabilities -> -32602
        Json noCaps = json_obj();
        noCaps["jsonrpc"] = "2.0";
        noCaps["id"] = 5;
        noCaps["method"] = "tools/list";
        noCaps["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"] = "2026-07-28";
        Json noCapsResp = app.dispatchMcpJson(noCaps);
        CHECK(noCapsResp["error"]["code"].get<int64_t>() == McpErrorCode::InvalidParams);

        // null id -> -32600 Invalid Request
        Json nullId = make_req(Json{}, "tools/list");
        nullId["id"] = nullptr;
        Json nullIdResp = app.dispatchMcpJson(nullId);
        CHECK(nullIdResp["error"]["code"].get<int64_t>() == McpErrorCode::InvalidRequest);
    }

    TEST_CASE("tools/call: moved PluginApp safety, media blocks, errors, and unknown tool") {
        auto buildApp = []() {
            PluginApp local("moved-app", "1.0.0");
            local.tool<AddInput>("add", "Adds two numbers", [](const AddInput &in) {
                Json res = json_obj();
                res["sum"] = in.a.get() + in.b.get();
                return res;
            });
            local.tool<std::monostate>("get_image", "Returns a mock image block", [](const std::monostate &) {
                return McpResult::image("iVBORw0KGgoAAAANSUhEUg==", "image/png");
            });
            local.tool<std::monostate>("fail_tool", "Throws a runtime error", [](const std::monostate &) -> McpResult {
                throw std::runtime_error("Boom with Bearer super-secret-token");
            });
            return local;
        };
        PluginApp app = buildApp();

        Json addParams = json_obj();
        addParams["name"] = "add";
        addParams["arguments"]["a"] = 20;
        addParams["arguments"]["b"] = 22;
        Json addResp = app.dispatchMcpJson(make_req(10, "tools/call", std::move(addParams)));
        CHECK(addResp["result"]["resultType"].get<std::string>() == "complete");
        CHECK(addResp["result"]["isError"].get<bool>() == false);
        CHECK(addResp["result"]["content"][0]["text"].get<std::string>().find("\"sum\":42") != std::string::npos);

        Json imgParams = json_obj();
        imgParams["name"] = "get_image";
        imgParams["arguments"] = json_obj();
        Json imgResp = app.dispatchMcpJson(make_req(11, "tools/call", std::move(imgParams)));
        CHECK(imgResp["result"]["content"][0]["type"].get<std::string>() == "image");
        CHECK(imgResp["result"]["content"][0]["mimeType"].get<std::string>() == "image/png");

        // Exceptions stay CallToolResult{isError:true} and redact Bearer secrets
        Json failParams = json_obj();
        failParams["name"] = "fail_tool";
        failParams["arguments"] = json_obj();
        Json failResp = app.dispatchMcpJson(make_req(12, "tools/call", std::move(failParams)));
        CHECK(failResp["result"]["resultType"].get<std::string>() == "complete");
        CHECK(failResp["result"]["isError"].get<bool>() == true);
        std::string errTxt = failResp["result"]["content"][0]["text"].get<std::string>();
        CHECK(errTxt.find("super-secret-token") == std::string::npos);
        CHECK(errTxt.find("[REDACTED]") != std::string::npos);

        // Unknown tool -> -32602
        Json missingParams = json_obj();
        missingParams["name"] = "no_such_tool";
        missingParams["arguments"] = json_obj();
        Json missingResp = app.dispatchMcpJson(make_req(13, "tools/call", std::move(missingParams)));
        CHECK(missingResp["error"]["code"].get<int64_t>() == McpErrorCode::InvalidParams);
    }

    TEST_CASE("Resources list, read, unknown URI -32602, and mcp_resource_read fallback") {
        PluginApp app("res-app");
        app.resources.push_back(Resource{
            .uri = "docs://intro",
            .name = "intro",
            .description = "Introduction doc",
            .mimeType = "text/markdown",
            .load = []() { return "# Intro Header"; },
            .ttlMs = 30000,
            .cacheScope = "private"});

        Json listResp = app.dispatchMcpJson(make_req(20, "resources/list"));
        CHECK(listResp["result"]["resultType"].get<std::string>() == "complete");
        CHECK(listResp["result"]["ttlMs"].get<int64_t>() >= 0);
        CHECK(listResp["result"]["resources"][0]["uri"].get<std::string>() == "docs://intro");

        Json readParams = json_obj();
        readParams["uri"] = "docs://intro";
        Json readResp = app.dispatchMcpJson(make_req(21, "resources/read", std::move(readParams)));
        CHECK(readResp["result"]["resultType"].get<std::string>() == "complete");
        CHECK(readResp["result"]["ttlMs"].get<int64_t>() == 30000);
        CHECK(readResp["result"]["cacheScope"].get<std::string>() == "private");
        CHECK(readResp["result"]["contents"][0]["text"].get<std::string>() == "# Intro Header");

        Json missingReadParams = json_obj();
        missingReadParams["uri"] = "docs://missing";
        Json missingReadResp = app.dispatchMcpJson(make_req(22, "resources/read", std::move(missingReadParams)));
        CHECK(missingReadResp["error"]["code"].get<int64_t>() == McpErrorCode::InvalidParams);

        Json fallbackParams = json_obj();
        fallbackParams["name"] = "mcp_resource_read";
        fallbackParams["arguments"]["uri"] = "docs://intro";
        Json toolCallResp = app.dispatchMcpJson(make_req(23, "tools/call", std::move(fallbackParams)));
        CHECK(toolCallResp["result"]["isError"].get<bool>() == false);
        CHECK(toolCallResp["result"]["content"][0]["text"].get<std::string>() == "# Intro Header");
    }

    TEST_CASE("RequestContext propagation: Principal, scopes, MRTR input_required, and -32021") {
        PluginApp app("ctx-app");
        app.tool<std::monostate>(
            "whoami",
            "Returns caller principal",
            ToolOptions{
                .annotations = ToolAnnotations{.readOnlyHint = true},
                .requiredScopes = {"profile:read"}},
            [](const std::monostate &, const RequestContext &ctx) {
                Json j = json_obj();
                j["subject"] = ctx.principal.subject;
                j["version"] = ctx.protocolVersion;
                return j;
            });

        app.tool<std::monostate>(
            "needs_elicit",
            "Demonstrates MRTR and client capability enforcement",
            [](const std::monostate &, const RequestContext &ctx) -> McpResult {
                ctx.requireClientCapability("elicitation");
                if (!ctx.inputResponses.has_value()) {
                    Json reqs = json_obj();
                    Json el = json_obj();
                    el["method"] = "elicitation/create";
                    reqs["confirm"] = std::move(el);
                    return McpResult::inputRequired(std::move(reqs), "step-1");
                }
                return McpResult::text("confirmed:" + ctx.requestState.value_or(""));
            });

        // Authenticated principal with scope
        RequestContext okCtx;
        okCtx.principal.authenticated = true;
        okCtx.principal.subject = "user-alice";
        okCtx.principal.scopes = {"profile:read"};

        Json whoParams = json_obj();
        whoParams["name"] = "whoami";
        whoParams["arguments"] = json_obj();
        Json whoResp = app.dispatchMcpJson(make_req(30, "tools/call", whoParams), okCtx);
        CHECK(whoResp["result"]["isError"].get<bool>() == false);
        CHECK(whoResp["result"]["content"][0]["text"].get<std::string>().find("user-alice") != std::string::npos);

        // Insufficient scope rejects before handler
        RequestContext badCtx;
        badCtx.principal.authenticated = true;
        badCtx.principal.subject = "user-bob";
        badCtx.principal.scopes = {"other:scope"};
        Json deniedResp = app.dispatchMcpJson(make_req(31, "tools/call", whoParams), badCtx);
        CHECK(deniedResp["error"]["code"].get<int64_t>() == McpErrorCode::InsufficientScope);

        // Missing client capability -> -32021
        Json elicitParams = json_obj();
        elicitParams["name"] = "needs_elicit";
        elicitParams["arguments"] = json_obj();
        Json noCapResp = app.dispatchMcpJson(make_req(32, "tools/call", elicitParams));
        CHECK(noCapResp["error"]["code"].get<int64_t>() == McpErrorCode::MissingRequiredClientCapability);
        CHECK(noCapResp["error"]["data"]["requiredCapabilities"].contains("elicitation"));

        // With elicitation capability -> input_required first, then complete on retry
        Json clientCaps = json_obj();
        clientCaps["elicitation"] = json_obj();
        Json mrtrResp = app.dispatchMcpJson(make_req(33, "tools/call", elicitParams, kMcpProtocolVersion, clientCaps));
        CHECK(mrtrResp["result"]["resultType"].get<std::string>() == "input_required");
        CHECK(mrtrResp["result"]["requestState"].get<std::string>() == "step-1");

        Json retryParams = elicitParams;
        retryParams["requestState"] = "step-1";
        retryParams["inputResponses"]["confirm"] = json_obj();
        Json doneResp = app.dispatchMcpJson(make_req(34, "tools/call", retryParams, kMcpProtocolVersion, clientCaps));
        CHECK(doneResp["result"]["resultType"].get<std::string>() == "complete");
        CHECK(doneResp["result"]["content"][0]["text"].get<std::string>() == "confirmed:step-1");
    }

    TEST_CASE("Stdio transport: progress/logging gating, cancellation, and EOF exit") {
        PluginApp app("stdio-app");
        std::mutex m;
        std::condition_variable cv;
        bool started = false;
        std::atomic<bool> observedCancel{false};

        app.tool<std::monostate>(
            "work",
            "Long operation observing progress/logs/cancellation",
            ToolOptions{.threadSafe = true},
            [&](const std::monostate &, const RequestContext &ctx) {
                ctx.emitProgress(1, 2, "halfway");
                ctx.emitLog("debug", Json("should be suppressed at info"), "test");
                ctx.emitLog("warning", Json("token=my-secret-123"), "test");
                {
                    std::lock_guard lk(m);
                    started = true;
                }
                cv.notify_all();
                for (int i = 0; i < 200; ++i) {
                    if (ctx.cancellation.isCancelled()) {
                        observedCancel.store(true);
                        return McpResult::error("cancelled");
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                return McpResult::text("finished");
            });

        // 1) Normal run with progressToken + logLevel=warning + quick request (test without sleep if fast? Run quick cancel test separately)
        // Here test quick complete request + redaction via another tool:
        app.tool<SecretInput>("echo_sec", "Logs and returns", [](const SecretInput &in, const RequestContext &ctx) {
            ctx.emitProgress(1, 1, "done");
            ctx.emitLog("debug", Json("hidden debug"), "unit");
            ctx.emitLog("error", Json("Bearer abcdef123456"), "unit");
            return in.msg.get();
        });

        Json req1 = make_req("r1", "tools/call", []() {
            Json p = json_obj();
            p["name"] = "echo_sec";
            p["arguments"]["msg"] = "ok";
            return p;
        }());
        req1["params"]["_meta"]["progressToken"] = "pt-1";
        req1["params"]["_meta"]["io.modelcontextprotocol/logLevel"] = "info";

        std::istringstream in1(slimr::write_json(req1) + "\n");
        std::ostringstream out1;
        std::ostringstream err1;
        app.serveMcpStdio(in1, out1, err1);

        std::string outStr = out1.str();
        CHECK(outStr.find("notifications/progress") != std::string::npos);
        CHECK(outStr.find("notifications/message") != std::string::npos);
        CHECK(outStr.find("hidden debug") == std::string::npos);
        CHECK(outStr.find("abcdef123456") == std::string::npos);
        CHECK(outStr.find("[REDACTED]") != std::string::npos);
        CHECK(outStr.find("\"id\":\"r1\"") != std::string::npos);

        // 2) In-flight cancellation via notifications/cancelled using synchronized stream buffer
        struct SyncLineBuf : public std::streambuf {
            std::mutex mu;
            std::condition_variable cv;
            std::string buf;
            bool closed{false};
            void pushLine(std::string_view line) {
                std::lock_guard lk(mu);
                buf.append(line);
                buf.push_back('\n');
                cv.notify_all();
            }
            void close() {
                std::lock_guard lk(mu);
                closed = true;
                cv.notify_all();
            }
            int_type underflow() override {
                std::unique_lock lk(mu);
                cv.wait(lk, [&]() { return !buf.empty() || closed; });
                if (buf.empty())
                    return traits_type::eof();
                return traits_type::to_int_type(buf.front());
            }
            int_type uflow() override {
                std::unique_lock lk(mu);
                cv.wait(lk, [&]() { return !buf.empty() || closed; });
                if (buf.empty())
                    return traits_type::eof();
                char c = buf.front();
                buf.erase(buf.begin());
                return traits_type::to_int_type(c);
            }
        } pipeBuf;

        std::istream pipeIn(&pipeBuf);
        std::ostringstream out2;
        std::ostringstream err2;
        std::thread srv([&]() { app.serveMcpStdio(pipeIn, out2, err2); });

        Json workReq = make_req("cancel-me", "tools/call", []() {
            Json p = json_obj();
            p["name"] = "work";
            p["arguments"] = json_obj();
            return p;
        }());
        workReq["params"]["_meta"]["progressToken"] = "pt-cancel";
        pipeBuf.pushLine(slimr::write_json(workReq));

        {
            std::unique_lock lk(m);
            CHECK(cv.wait_for(lk, std::chrono::seconds(2), [&]() { return started; }));
        }

        Json cancelNotif = json_obj();
        cancelNotif["jsonrpc"] = "2.0";
        cancelNotif["method"] = "notifications/cancelled";
        cancelNotif["params"]["requestId"] = "cancel-me";
        cancelNotif["params"]["reason"] = "user aborted";
        pipeBuf.pushLine(slimr::write_json(cancelNotif));
        pipeBuf.close();
        srv.join();

        CHECK(observedCancel.load() == true);
        CHECK(out2.str().find("\"id\":\"cancel-me\"") == std::string::npos);
    }

    TEST_CASE("CLI direct invocation and redaction helper") {
        PluginApp app("cli-app");
        app.tool<AddInput>("add", "Adds two numbers", [](const AddInput &in) {
            Json res = json_obj();
            res["result"] = in.a.get() + in.b.get();
            return res;
        });

        char arg0[] = "cli-app";
        char arg1[] = "add";
        char arg2[] = "{\"a\":15,\"b\":27}";
        char *argv[] = {arg0, arg1, arg2, nullptr};

        std::stringstream ss;
        auto *oldBuf = std::cout.rdbuf(ss.rdbuf());
        int code = app.run(3, argv);
        std::cout.rdbuf(oldBuf);

        CHECK(code == 0);
        CHECK(ss.str().find("\"result\":42") != std::string::npos);

        std::string red = redact_sensitive("Authorization: Bearer topsecret123 https://cb?code=abc123&state=ok");
        CHECK(red.find("topsecret123") == std::string::npos);
        CHECK(red.find("abc123") == std::string::npos);
        CHECK(red.find("state=ok") != std::string::npos);
    }

    TEST_CASE("PluginApp setBugsink populates bugsinkDsn and release") {
        PluginApp app("test-app", "1.0.0");
        CHECK(app.bugsinkDsn.empty());
        app.setBugsink("https://key@bugsink.example.com/1");
        CHECK(app.bugsinkDsn == "https://key@bugsink.example.com/1");
        REQUIRE(app.bugsink() != nullptr);
        CHECK(app.bugsink()->release() == "test-app@1.0.0");

        app.setBugsink("https://key@bugsink.example.com/1", "BUGSINK_DSN", "custom@2.0.0");
        REQUIRE(app.bugsink() != nullptr);
        CHECK(app.bugsink()->release() == "custom@2.0.0");
    }

    TEST_CASE("PluginApp --version, required env vars, and --skip-checks") {
        unsetenv("SLIMR_TEST_REQ_A");
        unsetenv("SLIMR_TEST_REQ_B");

        PluginApp app("env-app", "2.3.4");
        app.requireEnv("SLIMR_TEST_REQ_A", "API key")
            .requireEnv("SLIMR_TEST_REQ_B", "Workspace ID");
        app.tool<std::monostate>("ping", "Ping", [](const std::monostate &) { return "pong"; });

        auto run_args = [&](std::initializer_list<const char *> args,
                            std::string *outOut = nullptr,
                            std::string *errOut = nullptr) {
            std::vector<std::string> storage;
            std::vector<char *> argv;
            for (const char *s : args)
                storage.emplace_back(s);
            for (auto &s : storage)
                argv.push_back(s.data());

            std::ostringstream outBuf;
            std::ostringstream errBuf;
            auto *oldOut = std::cout.rdbuf(outBuf.rdbuf());
            auto *oldErr = std::cerr.rdbuf(errBuf.rdbuf());
            int rc = app.run(static_cast<int>(argv.size()), argv.data());
            std::cout.rdbuf(oldOut);
            std::cerr.rdbuf(oldErr);
            if (outOut)
                *outOut = outBuf.str();
            if (errOut)
                *errOut = errBuf.str();
            return rc;
        };

        // --version and --help succeed even when required env vars are missing
        std::string verOut;
        CHECK(run_args({"env-app", "--version"}, &verOut) == 0);
        CHECK(verOut == "env-app 2.3.4\n");
        CHECK(run_args({"env-app", "--help"}) == 0);

        // Missing env vars fail and list every missing var name and purpose
        std::string errMsg;
        CHECK(run_args({"env-app", "ping"}, nullptr, &errMsg) == 1);
        CHECK(errMsg.find("SLIMR_TEST_REQ_A") != std::string::npos);
        CHECK(errMsg.find("API key") != std::string::npos);
        CHECK(errMsg.find("SLIMR_TEST_REQ_B") != std::string::npos);
        CHECK(errMsg.find("Workspace ID") != std::string::npos);

        // Empty env var is treated as missing; partial env reports only missing one
        setenv("SLIMR_TEST_REQ_A", "valA", 1);
        setenv("SLIMR_TEST_REQ_B", "", 1);
        errMsg.clear();
        CHECK(run_args({"env-app", "mcp"}, nullptr, &errMsg) == 1);
        CHECK(errMsg.find("SLIMR_TEST_REQ_A") == std::string::npos);
        CHECK(errMsg.find("SLIMR_TEST_REQ_B") != std::string::npos);

        // --skip-checks bypasses env checks in leading or trailing positions
        std::string toolOut;
        CHECK(run_args({"env-app", "--skip-checks", "ping"}, &toolOut) == 0);
        CHECK(toolOut == "pong\n");
        toolOut.clear();
        CHECK(run_args({"env-app", "ping", "--skip-checks"}, &toolOut) == 0);
        CHECK(toolOut == "pong\n");

        // Populated env vars allow normal execution
        setenv("SLIMR_TEST_REQ_B", "valB", 1);
        toolOut.clear();
        CHECK(run_args({"env-app", "ping"}, &toolOut) == 0);
        CHECK(toolOut == "pong\n");

        unsetenv("SLIMR_TEST_REQ_A");
        unsetenv("SLIMR_TEST_REQ_B");
    }

    TEST_CASE("redact_json, Principal::hasAllScopes, CancellationToken, and emitLog levels") {
        // redact_json recursively redacts sensitive keys, arrays, and secret-bearing strings
        Json input = json_obj();
        input["authorization"] = "Bearer topsecret";
        input["x-api-key"] = "key-123";
        input["password"] = "hunter2";
        input["safe_url"] = "https://example.com/cb?access_token=tok1&my_token=keep&code=c1";
        input["basic_auth"] = "Basic dXNlcjpwYXNz";
        input["count"] = 42;
        Json nested = json_obj();
        nested["client_secret"] = "sec";
        nested["note"] = "hello";
        input["nested"] = std::move(nested);
        Json::array_t arr;
        arr.emplace_back("Bearer arrtok");
        arr.emplace_back(7);
        input["arr"] = json_arr(std::move(arr));

        Json redacted = redact_json(input);
        CHECK(redacted["authorization"].get<std::string>() == "[REDACTED]");
        CHECK(redacted["x-api-key"].get<std::string>() == "[REDACTED]");
        CHECK(redacted["password"].get<std::string>() == "[REDACTED]");
        CHECK(redacted["safe_url"].get<std::string>().find("tok1") == std::string::npos);
        CHECK(redacted["safe_url"].get<std::string>().find("my_token=keep") != std::string::npos);
        CHECK(redacted["basic_auth"].get<std::string>() == "Basic [REDACTED]");
        CHECK(redacted["count"].get<int64_t>() == 42);
        CHECK(redacted["nested"]["client_secret"].get<std::string>() == "[REDACTED]");
        CHECK(redacted["nested"]["note"].get<std::string>() == "hello");
        CHECK(redacted["arr"][0].get<std::string>() == "Bearer [REDACTED]");
        CHECK(redacted["arr"][1].get<int64_t>() == 7);

        // Principal::hasAllScopes overloads
        Principal p;
        p.authenticated = true;
        p.scopes = {"read", "write"};
        std::vector<std::string_view> reqSvOk{"read", "write"};
        std::vector<std::string_view> reqSvMissing{"read", "admin"};
        std::vector<std::string> reqStrOk{"read"};
        std::vector<std::string> reqStrMissing{"admin"};
        CHECK(p.hasAllScopes(reqSvOk));
        CHECK_FALSE(p.hasAllScopes(reqSvMissing));
        CHECK(p.hasAllScopes(reqStrOk));
        CHECK_FALSE(p.hasAllScopes(reqStrMissing));

        // CancellationToken & CancellationSource edge paths
        CancellationToken emptyTok;
        CHECK_FALSE(emptyTok.isCancelled());
        CHECK(emptyTok.onCancel([]() {}) == 0);
        emptyTok.removeOnCancel(0);

        CancellationSource src;
        auto tok = src.token();
        CHECK(tok.onCancel(nullptr) == 0);
        int removedCalls = 0;
        uint64_t remId = tok.onCancel([&]() { ++removedCalls; });
        CHECK(remId != 0);
        tok.removeOnCancel(remId);
        int keptCalls = 0;
        (void)tok.onCancel([&]() { ++keptCalls; });
        (void)tok.onCancel([]() { throw std::runtime_error("cb boom"); });
        src.cancel();
        src.cancel(); // idempotent second cancel
        CHECK(src.isCancelled());
        CHECK(tok.isCancelled());
        CHECK(removedCalls == 0);
        CHECK(keptCalls == 1);

        int afterCalls = 0;
        CHECK(tok.onCancel([&]() {
            ++afterCalls;
            throw std::runtime_error("late boom");
        }) == 0);
        CHECK(afterCalls == 1);

        // RequestContext::emitLog across all severity levels and gating
        std::vector<std::string> emittedLevels;
        auto ch = std::make_shared<McpNotificationChannel>([&](const Json &n) {
            if (n["method"].get<std::string>() == "notifications/message") {
                emittedLevels.push_back(n["params"]["level"].get<std::string>());
            }
        });
        RequestContext ctx;
        ctx.notificationChannel = ch;
        ctx.emitLog("error", "suppressed");
        CHECK(emittedLevels.empty());

        ctx.minLogLevel = "notice";
        for (const char *lvl : {"debug", "info", "notice", "warning", "error", "critical", "alert", "emergency", "unknown"}) {
            ctx.emitLog(lvl, "msg");
        }
        CHECK(emittedLevels == std::vector<std::string>{"notice", "warning", "error", "critical", "alert", "emergency"});

        // McpResult helpers
        Json obj = json_obj();
        obj["ok"] = true;
        auto rStructDefault = McpResult::structured(obj);
        CHECK(rStructDefault.toJson()["structuredContent"]["ok"].get<bool>() == true);
        auto rStructFallback = McpResult::structured(obj, "fallback");
        CHECK(rStructFallback.toJson()["content"][0]["text"].get<std::string>() == "fallback");
        auto rAudio = McpResult::audio("AAAA", "audio/wav");
        CHECK(rAudio.toJson()["content"][0]["type"].get<std::string>() == "audio");
        auto rInputNoState = McpResult::inputRequired(json_obj());
        CHECK(rInputNoState.toJson()["resultType"].get<std::string>() == "input_required");
        CHECK_FALSE(rInputNoState.toJson().contains("requestState"));
    }

    TEST_CASE("PluginApp tool variants, exception handling, authorization, and dispatch errors") {
        PluginApp app("edge-app", "0.1.0");
        app.defaultRequiredScopes = {"base"};
        app.authorizer = [](const Principal &p, std::string_view op, std::string &errOut) -> bool {
            if (p.subject == "deny-custom") {
                errOut = "custom deny for " + std::string(op);
                return false;
            }
            if (p.subject == "deny-empty") {
                return false;
            }
            return true;
        };

        ToolAnnotations ann;
        ann.readOnlyHint = true;
        ann.destructiveHint = false;
        ann.idempotentHint = true;
        ann.openWorldHint = false;
        app.tool<AddInput>("my_add", "Adds", ann, [](const AddInput &in) {
            return std::to_string(in.a.get() + in.b.get());
        });
        app.tool<SecretInput>("my-hyphen", "Hyphenated", [](const SecretInput &) {});
        app.tool<SecretInput>("void_tool", "Void", [](const SecretInput &) {});
        app.tool<SecretInput>("proto_err", "Proto err", [](const SecretInput &) -> McpResult {
            throw McpProtocolException(McpErrorCode::InvalidParams, "bad proto input");
        });
        app.tool<SecretInput>("std_err", "Std err", [](const SecretInput &) -> McpResult {
            throw std::runtime_error("boom Bearer secret123");
        });
        app.tool<SecretInput>("unk_err", "Unknown err", [](const SecretInput &) -> McpResult {
            throw 42;
        });

        app.resources.push_back(Resource{
            .uri = "test://ok",
            .name = "ok",
            .description = "ok",
            .mimeType = "text/plain",
            .load = []() { return "ok"; },
            .requiredScopes = {"res:read"}});
        app.resources.push_back(Resource{
            .uri = "test://proto",
            .name = "proto",
            .description = "proto",
            .mimeType = "text/plain",
            .load = []() -> std::string { throw McpProtocolException(McpErrorCode::InternalError, "res proto"); }});
        app.resources.push_back(Resource{
            .uri = "test://boom",
            .name = "boom",
            .description = "boom",
            .mimeType = "text/plain",
            .load = []() -> std::string { throw std::runtime_error("res Bearer tok"); }});

        // findTool normalization (hyphens/underscores) and tools/list annotations
        CHECK(app.findTool("my-add") != nullptr);
        CHECK(app.findTool("my_hyphen") != nullptr);
        Json listResp = app.dispatchMcpJson(make_req(1, "tools/list"));
        bool foundMyAddAnn = false;
        for (const auto &t : listResp["result"]["tools"].get_array()) {
            if (t["name"].get<std::string>() == "my_add") {
                foundMyAddAnn = t["annotations"]["readOnlyHint"].get<bool>();
            }
        }
        CHECK(foundMyAddAnn);

        // dispatchMcpJson validation errors
        CHECK(app.dispatchMcpJson(Json(123))["error"]["code"].get<int64_t>() == McpErrorCode::InvalidRequest);
        Json badVer = make_req(Json(true), "tools/list");
        CHECK(app.dispatchMcpJson(badVer)["error"]["code"].get<int64_t>() == McpErrorCode::InvalidRequest);
        Json badJsonRpc = make_req(2, "tools/list");
        badJsonRpc["jsonrpc"] = "1.0";
        Json badJsonRpcResp = app.dispatchMcpJson(badJsonRpc);
        CHECK(badJsonRpcResp["id"].get<int64_t>() == 2);
        CHECK(badJsonRpcResp["error"]["code"].get<int64_t>() == McpErrorCode::InvalidRequest);
        Json emptyMethod = make_req(3, "");
        CHECK(app.dispatchMcpJson(emptyMethod)["error"]["code"].get<int64_t>() == McpErrorCode::InvalidRequest);
        Json unknownMethod = make_req(4, "no/suchMethod");
        CHECK(app.dispatchMcpJson(unknownMethod)["error"]["code"].get<int64_t>() == McpErrorCode::MethodNotFound);

        // tools/call validation and exception paths
        Json noName = make_req(5, "tools/call", json_obj());
        CHECK(app.dispatchMcpJson(noName)["error"]["code"].get<int64_t>() == McpErrorCode::InvalidParams);

        auto callTool = [&](std::string_view name, Json args, const RequestContext &ctx = RequestContext{}) {
            Json p = json_obj();
            p["name"] = std::string(name);
            p["arguments"] = std::move(args);
            return app.dispatchMcpJson(make_req(6, "tools/call", std::move(p)), ctx);
        };
        Json msgArgs = json_obj();
        msgArgs["msg"] = "hi";
        CHECK(callTool("void_tool", msgArgs)["result"]["isError"].get<bool>() == false);
        CHECK(callTool("proto_err", msgArgs)["error"]["code"].get<int64_t>() == McpErrorCode::InvalidParams);
        Json stdErrRes = callTool("std_err", msgArgs);
        CHECK(stdErrRes["result"]["isError"].get<bool>() == true);
        CHECK(stdErrRes["result"]["content"][0]["text"].get<std::string>().find("secret123") == std::string::npos);
        CHECK(callTool("unk_err", msgArgs)["result"]["isError"].get<bool>() == true);

        // Authorization scopes and authorizer callback
        RequestContext noScopeCtx;
        noScopeCtx.principal.authenticated = true;
        CHECK(callTool("void_tool", msgArgs, noScopeCtx)["error"]["code"].get<int64_t>() == McpErrorCode::InsufficientScope);

        RequestContext customDenyCtx;
        customDenyCtx.principal.authenticated = true;
        customDenyCtx.principal.subject = "deny-custom";
        customDenyCtx.principal.scopes = {"base", "res:read"};
        CHECK(callTool("void_tool", msgArgs, customDenyCtx)["error"]["message"].get<std::string>().find("custom deny") != std::string::npos);

        RequestContext emptyDenyCtx = customDenyCtx;
        emptyDenyCtx.principal.subject = "deny-empty";
        CHECK(callTool("void_tool", msgArgs, emptyDenyCtx)["error"]["message"].get<std::string>() == "Forbidden");

        // resources/read and mcp_resource_read error paths
        CHECK(app.dispatchMcpJson(make_req(7, "resources/read", json_obj()))["error"]["code"].get<int64_t>() == McpErrorCode::InvalidParams);
        Json rProto = json_obj();
        rProto["uri"] = "test://proto";
        CHECK(app.dispatchMcpJson(make_req(8, "resources/read", rProto))["error"]["code"].get<int64_t>() == McpErrorCode::InternalError);
        Json rBoom = json_obj();
        rBoom["uri"] = "test://boom";
        Json rBoomResp = app.dispatchMcpJson(make_req(9, "resources/read", rBoom));
        CHECK(rBoomResp["error"]["code"].get<int64_t>() == McpErrorCode::InternalError);
        CHECK(rBoomResp["error"]["message"].get<std::string>().find("tok") == std::string::npos);

        Json rOk = json_obj();
        rOk["uri"] = "test://ok";
        CHECK(app.dispatchMcpJson(make_req(10, "resources/read", rOk), noScopeCtx)["error"]["code"].get<int64_t>() == McpErrorCode::InsufficientScope);

        CHECK(callTool("mcp_resource_read", json_obj())["error"]["code"].get<int64_t>() == McpErrorCode::InvalidParams);
        Json unknownUri = json_obj();
        unknownUri["uri"] = "test://missing";
        CHECK(callTool("mcp_resource_read", unknownUri)["error"]["code"].get<int64_t>() == McpErrorCode::InvalidParams);
        CHECK(callTool("mcp_resource_read", rOk, noScopeCtx)["error"]["code"].get<int64_t>() == McpErrorCode::InsufficientScope);
        CHECK(callTool("mcp_resource_read", rProto)["error"]["code"].get<int64_t>() == McpErrorCode::InternalError);
        CHECK(callTool("mcp_resource_read", rBoom)["result"]["isError"].get<bool>() == true);
    }

    TEST_CASE("Stdio transport edge cases and CLI run branches") {
        PluginApp app("cli-app", "0.2.0");
        app.tool<AddInput>("add", "Adds", [](const AddInput &in) {
            return std::to_string(in.a.get() + in.b.get());
        });
        app.tool<SecretInput>("fail", "Fails", [](const SecretInput &) {
            return McpResult::error("tool failed");
        });
        app.tool<SecretInput>("img", "Image", [](const SecretInput &) {
            return McpResult::image("AQID", "image/png");
        });
        app.tool<SecretInput>("boom", "Boom", [](const SecretInput &) -> McpResult {
            throw std::runtime_error("cli Bearer secret");
        });

        // serveMcpStdio: blank lines, malformed JSON, CRLF, and notification without id
        std::string cancelNotif = R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":99}})";
        std::istringstream in("\n   \r\nnot-json\r\n" + cancelNotif + "\n" +
                              slimr::write_json(make_req("s1", "server/discover")) + "\r\n");
        std::ostringstream out;
        std::ostringstream err;
        app.serveMcpStdio(in, out, err);
        CHECK(out.str().find("-32700") != std::string::npos);
        CHECK(out.str().find("\"id\":\"s1\"") != std::string::npos);

        // CLI run() branches with redirected cout/cerr
        auto runCli = [&](std::initializer_list<const char *> args, std::string *outBuf = nullptr, std::string *errBuf = nullptr) {
            std::vector<std::string> storage;
            std::vector<char *> argv;
            for (const char *a : args)
                storage.emplace_back(a);
            for (auto &s : storage)
                argv.push_back(s.data());

            std::ostringstream capOut;
            std::ostringstream capErr;
            struct RdbufGuard {
                std::ostream &stream;
                std::streambuf *prev;
                ~RdbufGuard() { stream.rdbuf(prev); }
            };
            RdbufGuard outGuard{std::cout, std::cout.rdbuf(capOut.rdbuf())};
            RdbufGuard errGuard{std::cerr, std::cerr.rdbuf(capErr.rdbuf())};
            int rc = app.run(static_cast<int>(argv.size()), argv.data());
            if (outBuf)
                *outBuf = capOut.str();
            if (errBuf)
                *errBuf = capErr.str();
            return rc;
        };

        std::string o;
        std::string e;
        CHECK(runCli({"cli-app", "--help"}, &o) == 0);
        CHECK(o.find("Available tools:") != std::string::npos);
        CHECK(runCli({"cli-app", "http"}, nullptr, &e) == 1);
        CHECK(e.find("HTTP transport is not enabled") != std::string::npos);
        CHECK(runCli({"cli-app", "unknown"}, &o, &e) == 1);
        CHECK(e.find("Unknown tool") != std::string::npos);
        CHECK(runCli({"cli-app", "add", "{bad"}, nullptr, &e) == 1);
        CHECK(e.find("Invalid JSON arguments") != std::string::npos);
        CHECK(runCli({"cli-app", "fail", R"({"msg":"x"})"}, nullptr, &e) == 1);
        CHECK(e.find("tool failed") != std::string::npos);
        CHECK(runCli({"cli-app", "img", R"({"msg":"x"})"}, &o) == 0);
        CHECK(o.find("image/png") != std::string::npos);
        CHECK(runCli({"cli-app", "boom", R"({"msg":"x"})"}, nullptr, &e) == 1);
        CHECK(e.find("Bearer [REDACTED]") != std::string::npos);
    }
}

#ifdef SLIMR_MCP_HAS_HTTP

namespace {

struct TcpFd {
    int fd{-1};
    TcpFd() = default;
    explicit TcpFd(int f) : fd(f) {}
    TcpFd(const TcpFd &) = delete;
    TcpFd &operator=(const TcpFd &) = delete;
    TcpFd(TcpFd &&o) noexcept : fd(o.fd) { o.fd = -1; }
    TcpFd &operator=(TcpFd &&o) noexcept {
        if (this != &o) {
            reset();
            fd = o.fd;
            o.fd = -1;
        }
        return *this;
    }
    ~TcpFd() { reset(); }
    void reset() {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }
};

struct RawHttpReply {
    int status{0};
    std::map<std::string, std::string> headers;
    std::string body;
};

TcpFd connect_loopback(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return TcpFd{};
    struct timeval tv{};
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return TcpFd{};
    }
    return TcpFd(fd);
}

bool send_all(int fd, std::string_view data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (n <= 0)
            return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

std::string build_http_request(std::string_view method,
                               std::string_view path,
                               uint16_t port,
                               const std::vector<std::pair<std::string, std::string>> &headers,
                               std::string_view body) {
    std::ostringstream os;
    os << method << " " << path << " HTTP/1.1\r\n";
    bool hasHost = false;
    bool hasConn = false;
    for (const auto &[k, v] : headers) {
        std::string kl = mcp_detail::ascii_lower(k);
        if (kl == "host")
            hasHost = true;
        if (kl == "connection")
            hasConn = true;
    }
    if (!hasHost)
        os << "Host: 127.0.0.1:" << port << "\r\n";
    if (!hasConn)
        os << "Connection: close\r\n";
    for (const auto &[k, v] : headers) {
        os << k << ": " << v << "\r\n";
    }
    if (!body.empty() || method == "POST" || method == "PUT") {
        os << "Content-Length: " << body.size() << "\r\n";
    }
    os << "\r\n";
    os << body;
    return os.str();
}

RawHttpReply parse_http_reply(std::string_view raw) {
    RawHttpReply reply;
    auto hdrEnd = raw.find("\r\n\r\n");
    std::string_view hdrBlock = hdrEnd == std::string_view::npos ? raw : raw.substr(0, hdrEnd);
    std::string_view bodyPart = hdrEnd == std::string_view::npos ? std::string_view{} : raw.substr(hdrEnd + 4);

    auto lineEnd = hdrBlock.find("\r\n");
    std::string_view statusLine = lineEnd == std::string_view::npos ? hdrBlock : hdrBlock.substr(0, lineEnd);
    auto sp1 = statusLine.find(' ');
    if (sp1 != std::string_view::npos) {
        reply.status = std::atoi(std::string(statusLine.substr(sp1 + 1)).c_str());
    }
    size_t pos = lineEnd == std::string_view::npos ? hdrBlock.size() : lineEnd + 2;
    while (pos < hdrBlock.size()) {
        auto next = hdrBlock.find("\r\n", pos);
        std::string_view line = next == std::string_view::npos ? hdrBlock.substr(pos) : hdrBlock.substr(pos, next - pos);
        auto col = line.find(':');
        if (col != std::string_view::npos) {
            std::string k = mcp_detail::ascii_lower(line.substr(0, col));
            std::string_view v = line.substr(col + 1);
            while (!v.empty() && v.front() == ' ')
                v.remove_prefix(1);
            reply.headers[k] = std::string(v);
        }
        if (next == std::string_view::npos)
            break;
        pos = next + 2;
    }

    if (auto it = reply.headers.find("transfer-encoding");
        it != reply.headers.end() && mcp_detail::ascii_lower(it->second).find("chunked") != std::string::npos) {
        std::string decoded;
        size_t p = 0;
        while (p < bodyPart.size()) {
            auto crlf = bodyPart.find("\r\n", p);
            if (crlf == std::string_view::npos)
                break;
            std::string szHex(bodyPart.substr(p, crlf - p));
            auto semi = szHex.find(';');
            if (semi != std::string::npos)
                szHex.resize(semi);
            size_t chunkLen = std::strtoull(szHex.c_str(), nullptr, 16);
            p = crlf + 2;
            if (chunkLen == 0)
                break;
            if (p + chunkLen > bodyPart.size()) {
                decoded.append(bodyPart.substr(p));
                break;
            }
            decoded.append(bodyPart.substr(p, chunkLen));
            p += chunkLen + 2;
        }
        reply.body = std::move(decoded);
    } else {
        reply.body = std::string(bodyPart);
    }
    return reply;
}

RawHttpReply http_exchange(uint16_t port,
                           std::string_view method,
                           std::string_view path,
                           const std::vector<std::pair<std::string, std::string>> &headers,
                           std::string_view body) {
    TcpFd fd = connect_loopback(port);
    if (fd.fd < 0)
        return {};
    std::string rawReq = build_http_request(method, path, port, headers, body);
    if (!send_all(fd.fd, rawReq))
        return {};
    std::string rawResp;
    char buf[4096];
    for (;;) {
        ssize_t n = ::recv(fd.fd, buf, sizeof(buf), 0);
        if (n <= 0)
            break;
        rawResp.append(buf, static_cast<size_t>(n));
    }
    return parse_http_reply(rawResp);
}

std::string read_http_message(int fd) {
    std::string raw;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd pfd{fd, POLLIN, 0};
        if (::poll(&pfd, 1, 50) < 0)
            break;
        if (!(pfd.revents & (POLLIN | POLLHUP)))
            continue;
        char buf[8192];
        const auto n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
            break;
        raw.append(buf, static_cast<size_t>(n));
        if (raw.find("\r\n\r\n") == std::string::npos)
            continue;
        const auto reply = parse_http_reply(raw);
        if (reply.headers.contains("transfer-encoding")) {
            if (raw.ends_with("\r\n0\r\n\r\n"))
                break;
        } else if (auto it = reply.headers.find("content-length"); it != reply.headers.end()) {
            if (reply.body.size() == std::stoull(it->second))
                break;
        }
    }
    return raw;
}

std::vector<std::pair<std::string, std::string>> mcp_headers(
    std::string_view method,
    std::string_view mcpName = "",
    std::string_view bearer = "") {
    std::vector<std::pair<std::string, std::string>> h{
        {"Content-Type", "application/json"},
        {"Accept", "application/json, text/event-stream"},
        {"MCP-Protocol-Version", "2026-07-28"},
        {"Mcp-Method", std::string(method)},
    };
    if (!mcpName.empty())
        h.emplace_back("Mcp-Name", std::string(mcpName));
    if (!bearer.empty())
        h.emplace_back("Authorization", "Bearer " + std::string(bearer));
    return h;
}

} // namespace

TEST_SUITE("MCP Streamable HTTP & OAuth") {
    TEST_CASE("Streamable HTTP bind validation, discovery, headers, x-mcp-header, and Base64 sentinel") {
        PluginApp app("http-app", "3.1.0");
        app.instructions = "HTTP test app";
        app.tool<AddInput>("add", "Adds two numbers", [](const AddInput &in) {
            Json j = json_obj();
            j["sum"] = in.a.get() + in.b.get();
            return j;
        });

        // Tool with x-mcp-header parameter & non-ASCII name
        ToolDef hdrTool;
        hdrTool.name = "sql_exec";
        hdrTool.description = "Runs query in region";
        hdrTool.inputSchema = *parse_json<Json>(R"({
            "type": "object",
            "properties": {
                "region": { "type": "string", "x-mcp-header": true },
                "count": { "type": "integer", "x-mcp-header": "Count" }
            },
            "required": ["region"],
            "additionalProperties": false
        })");
        hdrTool.handler = [](const Json &args, const RequestContext &) {
            return McpResult::text(args["region"].get<std::string>());
        };
        app.registerTool(std::move(hdrTool));

        app.tool<std::monostate>("UTF8_☕", "Unicode tool", [](const std::monostate &) {
            return "coffee";
        });

        // Invalid IP host fails before start
        {
            HttpServerOptions badOpts;
            badOpts.host = "not-an-ip";
            badOpts.port = 0;
            McpHttpServer badSrv(app, badOpts);
            std::string err;
            CHECK(badSrv.start(&err) == false);
            CHECK(err.find("Invalid bind host") != std::string::npos);
        }
        // Default options bind 0.0.0.0:0 directly
        {
            HttpServerOptions defOpts;
            CHECK(defOpts.host == "0.0.0.0");
            CHECK(defOpts.port == 0);
            McpHttpServer defSrv(app, defOpts);
            std::string err;
            REQUIRE(defSrv.start(&err));
            CHECK(defSrv.boundPort() > 0);
            defSrv.stop();
        }

        HttpServerOptions opts;
        opts.host = "127.0.0.1";
        opts.port = 0;
        McpHttpServer srv(app, opts);
        std::string err;
        REQUIRE(srv.start(&err));
        const uint16_t port = srv.boundPort();
        CHECK(port > 0);

        // GET /mcp -> 405 Method Not Allowed
        auto getMcp = http_exchange(port, "GET", "/mcp", {}, "");
        CHECK(getMcp.status == 405);

        // Arbitrary Origin -> 200 OK
        auto anyOrigin = http_exchange(
            port, "POST", "/mcp",
            {
                {"Origin", "https://any.example"},
                {"Content-Type", "application/json"},
                {"Accept", "application/json, text/event-stream"},
                {"MCP-Protocol-Version", "2026-07-28"},
                {"Mcp-Method", "server/discover"},
            },
            slimr::write_json(make_req(1, "server/discover")));
        CHECK(anyOrigin.status == 200);

        // Arbitrary Host -> 200 OK
        auto anyHost = http_exchange(
            port, "POST", "/mcp",
            {
                {"Host", "any.example:" + std::to_string(port)},
                {"Content-Type", "application/json"},
                {"Accept", "application/json, text/event-stream"},
                {"MCP-Protocol-Version", "2026-07-28"},
                {"Mcp-Method", "server/discover"},
            },
            slimr::write_json(make_req(1, "server/discover")));
        CHECK(anyHost.status == 200);

        // Missing Accept text/event-stream -> 406
        auto badAccept = http_exchange(
            port, "POST", "/mcp",
            {
                {"Content-Type", "application/json"},
                {"Accept", "application/json"},
            },
            slimr::write_json(make_req(1, "server/discover")));
        CHECK(badAccept.status == 406);

        // Valid server/discover -> 200
        auto disc = http_exchange(port, "POST", "/mcp", mcp_headers("server/discover"),
                                  slimr::write_json(make_req(1, "server/discover")));
        CHECK(disc.status == 200);
        auto discJson = parse_json<Json>(disc.body);
        REQUIRE(discJson.has_value());
        CHECK((*discJson)["result"]["supportedVersions"][0].get<std::string>() == "2026-07-28");

        // Legacy initialize -> 404 Not Found with -32601
        Json initReq = json_obj();
        initReq["jsonrpc"] = "2.0";
        initReq["id"] = 2;
        initReq["method"] = "initialize";
        auto initRep = http_exchange(port, "POST", "/mcp", mcp_headers("initialize"), slimr::write_json(initReq));
        CHECK(initRep.status == 404);
        CHECK(parse_json<Json>(initRep.body).value()["error"]["code"].get<int64_t>() == McpErrorCode::MethodNotFound);

        // Notification POST -> 202 Accepted empty body
        Json notif = json_obj();
        notif["jsonrpc"] = "2.0";
        notif["method"] = "notifications/initialized";
        auto notifRep = http_exchange(port, "POST", "/mcp", {{"Content-Type", "application/json"}}, slimr::write_json(notif));
        CHECK(notifRep.status == 202);
        CHECK(notifRep.body.empty());

        // Header mismatch: wrong Mcp-Method -> 400 -32020
        auto mismatchMethod = http_exchange(port, "POST", "/mcp", mcp_headers("tools/call", "add"),
                                            slimr::write_json(make_req(3, "tools/list")));
        CHECK(mismatchMethod.status == 400);
        CHECK(parse_json<Json>(mismatchMethod.body).value()["error"]["code"].get<int64_t>() == McpErrorCode::HeaderMismatch);

        // Unsupported version in both header & body -> 400 -32022
        auto unsupHeaders = mcp_headers("server/discover");
        unsupHeaders[2].second = "2025-11-25";
        auto unsupRep = http_exchange(port, "POST", "/mcp", unsupHeaders,
                                      slimr::write_json(make_req(4, "server/discover", json_obj(), "2025-11-25")));
        CHECK(unsupRep.status == 400);
        CHECK(parse_json<Json>(unsupRep.body).value()["error"]["code"].get<int64_t>() == McpErrorCode::UnsupportedProtocolVersion);

        // Base64-encoded Mcp-Name sentinel (=?base64?...?=)
        std::string b64Name = "=?base64?" + mcp_detail::base64_encode("UTF8_☕") + "?=";
        Json uParams = json_obj();
        uParams["name"] = "UTF8_☕";
        uParams["arguments"] = json_obj();
        auto utfRep = http_exchange(port, "POST", "/mcp", mcp_headers("tools/call", b64Name),
                                    slimr::write_json(make_req(5, "tools/call", uParams)));
        CHECK(utfRep.status == 200);
        CHECK(parse_json<Json>(utfRep.body).value()["result"]["content"][0]["text"].get<std::string>() == "coffee");

        // x-mcp-header validation: 42 == 42.0 numeric equality, missing header -> 400 -32020
        Json sqlParams = json_obj();
        sqlParams["name"] = "sql_exec";
        sqlParams["arguments"]["region"] = "us-west1";
        sqlParams["arguments"]["count"] = 42;
        auto sqlMissing = http_exchange(port, "POST", "/mcp", mcp_headers("tools/call", "sql_exec"),
                                        slimr::write_json(make_req(6, "tools/call", sqlParams)));
        CHECK(sqlMissing.status == 400);
        CHECK(parse_json<Json>(sqlMissing.body).value()["error"]["code"].get<int64_t>() == McpErrorCode::HeaderMismatch);

        auto sqlHdrs = mcp_headers("tools/call", "sql_exec");
        sqlHdrs.emplace_back("Mcp-Param-Region", "us-west1");
        sqlHdrs.emplace_back("Mcp-Param-Count", "42.0");
        auto sqlOk = http_exchange(port, "POST", "/mcp", sqlHdrs,
                                   slimr::write_json(make_req(7, "tools/call", sqlParams)));
        CHECK(sqlOk.status == 200);
        CHECK(parse_json<Json>(sqlOk.body).value()["result"]["content"][0]["text"].get<std::string>() == "us-west1");

        srv.stop();
    }

    TEST_CASE("Streamable HTTP incremental SSE, disconnect cancellation, worker lane isolation, and slow reader") {
        PluginApp app("stream-app");
        std::mutex stepMu;
        std::condition_variable stepCv;
        bool step1Sent = false;
        bool allowStep2 = false;
        std::atomic<bool> blockCancelled{false};
        std::atomic<bool> releaseWorkers{false};

        app.tool<std::monostate>(
            "stream_steps",
            "Emits incremental SSE before second step",
            ToolOptions{.threadSafe = true},
            [&](const std::monostate &, const RequestContext &ctx) {
                ctx.emitProgress(1, 2, "step-one");
                {
                    std::lock_guard lk(stepMu);
                    step1Sent = true;
                }
                stepCv.notify_all();
                {
                    std::unique_lock lk(stepMu);
                    stepCv.wait_for(lk, std::chrono::seconds(3), [&]() {
                        return allowStep2 || ctx.cancellation.isCancelled();
                    });
                }
                ctx.emitProgress(2, 2, "step-two");
                return "all-done";
            });

        app.tool<std::monostate>(
            "wait_cancel",
            "Waits until cancelled or released",
            ToolOptions{.threadSafe = true},
            [&](const std::monostate &, const RequestContext &ctx) {
                for (int i = 0; i < 400; ++i) {
                    if (ctx.cancellation.isCancelled()) {
                        blockCancelled.store(true);
                        return "cancelled";
                    }
                    if (releaseWorkers.load())
                        return "released";
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                return "timeout";
            });

        app.tool<std::monostate>(
            "flood",
            "Produces many progress events to verify bounded SSE buffer on slow reader",
            ToolOptions{.threadSafe = true},
            [&](const std::monostate &, const RequestContext &ctx) {
                std::string pad(2048, 'x');
                for (int i = 0; i < 120; ++i) {
                    if (ctx.cancellation.isCancelled())
                        break;
                    ctx.emitProgress(i + 1, 120, pad);
                }
                return "flood-done";
            });

        HttpServerOptions opts;
        opts.host = "127.0.0.1";
        opts.port = 0;
        opts.workerThreads = 2;
        opts.maxPendingJobs = 2;
        opts.sseHighWaterBytes = 4096;
        opts.sseMaxWriteBufBytes = 16384;
        McpHttpServer srv(app, opts);
        REQUIRE(srv.start());
        const uint16_t port = srv.boundPort();

        // 1) Verify SSE step 1 arrives incrementally BEFORE step 2 is unlocked!
        {
            TcpFd fd = connect_loopback(port);
            REQUIRE(fd.fd >= 0);
            Json p = json_obj();
            p["name"] = "stream_steps";
            p["arguments"] = json_obj();
            Json r = make_req("sse-1", "tools/call", p);
            r["params"]["_meta"]["progressToken"] = "pt-sse";
            REQUIRE(send_all(fd.fd, build_http_request("POST", "/mcp", port, mcp_headers("tools/call", "stream_steps"), slimr::write_json(r))));

            {
                std::unique_lock lk(stepMu);
                REQUIRE(stepCv.wait_for(lk, std::chrono::seconds(2), [&]() { return step1Sent; }));
            }

            // Read available socket data right now while step 2 is still blocked!
            std::string soFar;
            char buf[4096];
            for (int attempt = 0; attempt < 40; ++attempt) {
                struct pollfd pfd{fd.fd, POLLIN, 0};
                if (::poll(&pfd, 1, 50) > 0) {
                    ssize_t n = ::recv(fd.fd, buf, sizeof(buf), 0);
                    if (n > 0)
                        soFar.append(buf, static_cast<size_t>(n));
                    if (soFar.find("step-one") != std::string::npos)
                        break;
                }
            }
            CHECK(soFar.find("text/event-stream") != std::string::npos);
            CHECK(soFar.find("step-one") != std::string::npos);
            CHECK(soFar.find("all-done") == std::string::npos);

            {
                std::lock_guard lk(stepMu);
                allowStep2 = true;
            }
            stepCv.notify_all();

            for (;;) {
                ssize_t n = ::recv(fd.fd, buf, sizeof(buf), 0);
                if (n <= 0)
                    break;
                soFar.append(buf, static_cast<size_t>(n));
            }
            CHECK(soFar.find("step-two") != std::string::npos);
            CHECK(soFar.find("all-done") != std::string::npos);
        }

        // 2) Saturate workers and check fast discovery stays responsive + disconnect cancels worker
        std::vector<TcpFd> heldConns;
        Json waitP = json_obj();
        waitP["name"] = "wait_cancel";
        waitP["arguments"] = json_obj();
        for (int i = 0; i < 4; ++i) {
            TcpFd fd = connect_loopback(port);
            REQUIRE(fd.fd >= 0);
            REQUIRE(send_all(fd.fd, build_http_request("POST", "/mcp", port, mcp_headers("tools/call", "wait_cancel"),
                                                       slimr::write_json(make_req(100 + i, "tools/call", waitP)))));
            heldConns.push_back(std::move(fd));
        }
        // Give loop a tiny moment to enqueue all 4
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // 5th tool call hits bounded queue -> 503 Service Unavailable
        auto busyRep = http_exchange(port, "POST", "/mcp", mcp_headers("tools/call", "wait_cancel"),
                                     slimr::write_json(make_req(105, "tools/call", waitP)));
        CHECK(busyRep.status == 503);

        // Unrelated server/discover still succeeds cleanly without stalling behind saturated tool queue!
        auto fastDisc = http_exchange(port, "POST", "/mcp", mcp_headers("server/discover"),
                                      slimr::write_json(make_req(106, "server/discover")));
        CHECK(fastDisc.status == 200);

        // Dropping client socket triggers cancellation
        heldConns.clear();
        for (int i = 0; i < 40 && !blockCancelled.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        CHECK(blockCancelled.load() == true);

        // 3) Slow reader still completes cleanly without crashing under sseHighWaterBytes cap
        Json floodP = json_obj();
        floodP["name"] = "flood";
        floodP["arguments"] = json_obj();
        Json floodR = make_req(200, "tools/call", floodP);
        floodR["params"]["_meta"]["progressToken"] = "pt-flood";
        auto floodRep = http_exchange(port, "POST", "/mcp", mcp_headers("tools/call", "flood"),
                                      slimr::write_json(floodR));
        CHECK(floodRep.status == 200);
        CHECK(floodRep.body.find("flood-done") != std::string::npos);

        srv.stop();
    }

    TEST_CASE("OAuth Resource Server: RFC 9728 metadata, 401/403 challenges, AS fixture, and fail-closed checks") {
        PluginApp app("oauth-app", "1.0.0");
        app.tool<std::monostate>(
            "public_read",
            "Requires mcp:read",
            ToolOptions{.requiredScopes = {"mcp:read"}},
            [](const std::monostate &, const RequestContext &ctx) {
                return "sub=" + ctx.principal.subject;
            });
        app.tool<std::monostate>(
            "admin_write",
            "Requires mcp:write",
            ToolOptions{.requiredScopes = {"mcp:write"}},
            [](const std::monostate &) {
                return "admin-ok";
            });

        // Simulated test Authorization Server mounted via customRoutes
        std::mutex authMu;
        std::map<std::string, std::string> issuedCodes;
        std::map<std::string, Principal> activeTokens;

        HttpServerOptions opts;
        opts.host = "127.0.0.1";
        opts.port = 0;
        opts.oauth.enabled = true;
        opts.oauth.expectedIssuers = {"https://auth.example.test"};
        opts.oauth.defaultScopes = {"mcp:read"};
        opts.oauth.metadata.scopesSupported = {"mcp:read", "mcp:write"};
        opts.oauth.metadata.authorizationServers = {"https://auth.example.test"};

        opts.oauth.validateToken = [&](const TokenValidationContext &tctx) {
            std::lock_guard lk(authMu);
            TokenValidationResult res;
            auto it = activeTokens.find(std::string(tctx.token));
            if (it == activeTokens.end()) {
                res.status = TokenValidationResult::Status::InvalidToken;
                res.errorDescription = "Unknown token";
                return res;
            }
            res.status = TokenValidationResult::Status::Valid;
            res.principal = it->second;
            return res;
        };

        opts.customRoutes.push_back(CustomHttpRoute{
            .method = "GET",
            .path = "/test-as/authorize",
            .handler = [&](const HttpRouteRequest &req) {
                std::lock_guard lk(authMu);
                std::string code = "code-123";
                issuedCodes[code] = req.query("client_id").value_or("opencode");
                return HttpRouteResponse::json(200, *parse_json<Json>("{\"code\":\"code-123\"}"));
            }});
        opts.customRoutes.push_back(CustomHttpRoute{
            .method = "POST",
            .path = "/test-as/token",
            .handler = [&](const HttpRouteRequest &req) {
                std::lock_guard lk(authMu);
                auto parsed = parse_json<Json>(req.body);
                if (!parsed || !parsed->contains("code") ||
                    issuedCodes.find((*parsed)["code"].get<std::string>()) == issuedCodes.end()) {
                    return HttpRouteResponse::json(400, *parse_json<Json>("{\"error\":\"invalid_grant\"}"));
                }
                Principal p;
                p.authenticated = true;
                p.subject = "alice";
                p.clientId = issuedCodes[(*parsed)["code"].get<std::string>()];
                p.issuer = "https://auth.example.test";
                p.audience = req.query("resource").value_or("");
                p.scopes = {"mcp:read"};
                p.expiresAt = std::chrono::system_clock::now() + std::chrono::hours(1);
                activeTokens["tok-alice-read"] = p;
                Json out = json_obj();
                out["access_token"] = "tok-alice-read";
                out["token_type"] = "Bearer";
                out["scope"] = "mcp:read";
                return HttpRouteResponse::json(200, out);
            }});

        McpHttpServer srv(app, opts);
        REQUIRE(srv.start());
        const uint16_t port = srv.boundPort();
        const std::string resourceUrl = srv.mcpEndpointUrl();

        // 1) Protected Resource Metadata discoverable without bearer token
        auto prm = http_exchange(port, "GET", "/.well-known/oauth-protected-resource", {}, "");
        CHECK(prm.status == 200);
        Json prmJson = parse_json<Json>(prm.body).value();
        CHECK(prmJson["resource"].get<std::string>() == resourceUrl);
        CHECK(prmJson["authorization_servers"][0].get<std::string>() == "https://auth.example.test");

        // 2) Unauthenticated request -> 401 with WWW-Authenticate resource_metadata & scope
        auto unauth = http_exchange(port, "POST", "/mcp", mcp_headers("server/discover"),
                                    slimr::write_json(make_req(1, "server/discover")));
        CHECK(unauth.status == 401);
        CHECK(unauth.headers["www-authenticate"].find("resource_metadata=") != std::string::npos);
        CHECK(unauth.headers["www-authenticate"].find("scope=\"mcp:read\"") != std::string::npos);

        // 3) Authorize + exchange code via test AS customRoutes
        auto authCodeRep = http_exchange(port, "GET", "/test-as/authorize?client_id=test-cli", {}, "");
        CHECK(authCodeRep.status == 200);
        auto tokRep = http_exchange(port, "POST", "/test-as/token?resource=" + resourceUrl,
                                    {{"Content-Type", "application/json"}}, "{\"code\":\"code-123\"}");
        CHECK(tokRep.status == 200);
        CHECK(parse_json<Json>(tokRep.body).value()["access_token"].get<std::string>() == "tok-alice-read");

        // 4) Authorized read tool succeeds with principal identity
        Json pubP = json_obj();
        pubP["name"] = "public_read";
        pubP["arguments"] = json_obj();
        auto okCall = http_exchange(port, "POST", "/mcp",
                                    mcp_headers("tools/call", "public_read", "tok-alice-read"),
                                    slimr::write_json(make_req(2, "tools/call", pubP)));
        CHECK(okCall.status == 200);
        CHECK(parse_json<Json>(okCall.body).value()["result"]["content"][0]["text"].get<std::string>() == "sub=alice");

        // 5) Missing tool scope -> 403 insufficient_scope step-up challenge BEFORE execution
        Json admP = json_obj();
        admP["name"] = "admin_write";
        admP["arguments"] = json_obj();
        auto stepUp = http_exchange(port, "POST", "/mcp",
                                    mcp_headers("tools/call", "admin_write", "tok-alice-read"),
                                    slimr::write_json(make_req(3, "tools/call", admP)));
        CHECK(stepUp.status == 403);
        CHECK(stepUp.headers["www-authenticate"].find("error=\"insufficient_scope\"") != std::string::npos);
        CHECK(stepUp.headers["www-authenticate"].find("mcp:write") != std::string::npos);

        // 6) Query-string token forbidden -> 401
        auto qsTok = http_exchange(port, "POST", "/mcp?access_token=tok-alice-read",
                                   mcp_headers("server/discover"),
                                   slimr::write_json(make_req(4, "server/discover")));
        CHECK(qsTok.status == 401);

        // 7) Expired token, wrong issuer, wrong audience all fail closed (401)
        {
            std::lock_guard lk(authMu);
            Principal expired;
            expired.authenticated = true;
            expired.subject = "eve";
            expired.issuer = "https://auth.example.test";
            expired.audience = resourceUrl;
            expired.scopes = {"mcp:read"};
            expired.expiresAt = std::chrono::system_clock::now() - std::chrono::seconds(10);
            activeTokens["tok-expired"] = expired;

            Principal wrongIss = expired;
            wrongIss.expiresAt = std::chrono::system_clock::now() + std::chrono::hours(1);
            wrongIss.issuer = "https://evil-issuer.test";
            activeTokens["tok-wrong-iss"] = wrongIss;

            Principal wrongAud = wrongIss;
            wrongAud.issuer = "https://auth.example.test";
            wrongAud.audience = "http://127.0.0.1:9999/other";
            activeTokens["tok-wrong-aud"] = wrongAud;
        }
        CHECK(http_exchange(port, "POST", "/mcp", mcp_headers("server/discover", "", "tok-expired"),
                            slimr::write_json(make_req(5, "server/discover")))
                  .status == 401);
        CHECK(http_exchange(port, "POST", "/mcp", mcp_headers("server/discover", "", "tok-wrong-iss"),
                            slimr::write_json(make_req(6, "server/discover")))
                  .status == 401);
        CHECK(http_exchange(port, "POST", "/mcp", mcp_headers("server/discover", "", "tok-wrong-aud"),
                            slimr::write_json(make_req(7, "server/discover")))
                  .status == 401);

        srv.stop();
    }

    TEST_CASE("HTTP shutdown delivers natural JSON and SSE completion and rejects new admissions") {
        for (const bool stream : {false, true}) {
            CAPTURE(stream);
            PluginApp app("natural-stop");
            std::atomic<bool> started{false};
            std::atomic<bool> release{false};
            std::atomic<bool> cancelled{false};
            const std::string payload(256 * 1024, 'x');
            app.tool<std::monostate>(
                "finish", "Completes during shutdown", ToolOptions{.threadSafe = true},
                [&](const std::monostate &, const RequestContext &ctx) {
                    if (stream)
                        ctx.emitProgress(1, 2, "started");
                    started.store(true);
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                    while (!release.load() && std::chrono::steady_clock::now() < deadline) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    cancelled.store(ctx.cancellation.isCancelled());
                    return payload;
                });
            HttpServerOptions opts;
            opts.shutdownTimeoutMs = 2000;
            opts.sseMaxWriteBufBytes = 1024 * 1024;
            McpHttpServer srv(app, opts);
            std::string err;
            REQUIRE(srv.start(&err));
            const auto port = srv.boundPort();
            REQUIRE(port != 0);

            TcpFd probe = connect_loopback(port);
            REQUIRE(probe.fd >= 0);
            auto headers = mcp_headers("server/discover");
            headers.emplace_back("Connection", "keep-alive");
            const auto discovery = build_http_request(
                "POST", "/mcp", port, headers, slimr::write_json(make_req(1, "server/discover")));
            REQUIRE(send_all(probe.fd, discovery));
            REQUIRE(parse_http_reply(read_http_message(probe.fd)).status == 200);

            TcpFd conn = connect_loopback(port);
            REQUIRE(conn.fd >= 0);
            Json params = json_obj();
            params["name"] = "finish";
            params["arguments"] = json_obj();
            auto req = make_req(42, "tools/call", params);
            if (stream)
                req["params"]["_meta"]["progressToken"] = "natural-stop";
            REQUIRE(send_all(conn.fd, build_http_request(
                                          "POST", "/mcp", port, mcp_headers("tools/call", "finish"), slimr::write_json(req))));
            for (int i = 0; i < 1000 && !started.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            REQUIRE(started.load());

            std::jthread stopping([&]() { srv.stop(); });
            int admissionStatus = 0;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            while (std::chrono::steady_clock::now() < deadline) {
                if (!send_all(probe.fd, discovery))
                    break;
                admissionStatus = parse_http_reply(read_http_message(probe.fd)).status;
                if (admissionStatus != 200)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            CHECK(admissionStatus == 503);
            const auto newConnReply = http_exchange(port, "POST", "/mcp", mcp_headers("server/discover"),
                                                    slimr::write_json(make_req(2, "server/discover")));
            CHECK(newConnReply.status == 503);
            release.store(true);
            const auto raw = read_http_message(conn.fd);
            stopping.join();
            CHECK(connect_loopback(port).fd < 0);
            CHECK_FALSE(cancelled.load());
            const auto reply = parse_http_reply(raw);
            CHECK(reply.status == 200);
            if (stream) {
                CHECK(reply.headers.at("content-type") == "text/event-stream");
                CHECK(raw.ends_with("\r\n0\r\n\r\n"));
                const auto lastEvent = reply.body.rfind("data: ");
                REQUIRE(lastEvent != std::string::npos);
                const auto final = parse_json<Json>(reply.body.substr(lastEvent + 6));
                REQUIRE(final.has_value());
                CHECK((*final)["id"].get<int64_t>() == 42);
                CHECK((*final)["result"]["content"][0]["text"].get<std::string>() == payload);
            } else {
                CHECK(reply.body.size() == std::stoull(reply.headers.at("content-length")));
                const auto final = parse_json<Json>(reply.body);
                REQUIRE(final.has_value());
                CHECK((*final)["id"].get<int64_t>() == 42);
                CHECK((*final)["result"]["content"][0]["text"].get<std::string>() == payload);
            }
            CHECK(srv.boundPort() == 0);
            srv.stop();
            REQUIRE(srv.start(&err));
            CHECK(http_exchange(srv.boundPort(), "POST", "/mcp", mcp_headers("server/discover"),
                                slimr::write_json(make_req(2, "server/discover")))
                      .status == 200);
            srv.stop();
        }
    }

    TEST_CASE("HTTP shutdown bounds flushing when a client stops reading") {
        PluginApp app("slow-shutdown");
        const std::string payload(8 * 1024 * 1024, 'x');
        app.tool<std::monostate>("large", "Produces a buffered response",
                                 [&](const std::monostate &) { return payload; });
        HttpServerOptions opts;
        opts.shutdownTimeoutMs = 0;
        opts.sseMaxWriteBufBytes = 16 * 1024 * 1024;
        McpHttpServer srv(app, opts);
        std::string err;
        REQUIRE(srv.start(&err));
        TcpFd conn = connect_loopback(srv.boundPort());
        REQUIRE(conn.fd >= 0);
        const int receiveBuffer = 4096;
        REQUIRE(::setsockopt(conn.fd, SOL_SOCKET, SO_RCVBUF, &receiveBuffer, sizeof(receiveBuffer)) == 0);
        Json params = json_obj();
        params["name"] = "large";
        params["arguments"] = json_obj();
        REQUIRE(send_all(conn.fd, build_http_request(
                                      "POST", "/mcp", srv.boundPort(), mcp_headers("tools/call", "large"),
                                      slimr::write_json(make_req(1, "tools/call", params)))));
        pollfd ready{conn.fd, POLLIN, 0};
        REQUIRE(::poll(&ready, 1, 3000) > 0);
        char header[128]{};
        const auto n = ::recv(conn.fd, header, sizeof(header), MSG_PEEK);
        REQUIRE(n > 0);
        REQUIRE(std::string_view(header, static_cast<size_t>(n)).starts_with("HTTP/1.1 200"));
        const auto start = std::chrono::steady_clock::now();
        srv.stop();
        const auto elapsed = std::chrono::steady_clock::now() - start;
        CHECK(elapsed >= std::chrono::milliseconds(40));
        CHECK(elapsed < std::chrono::seconds(2));
    }

    TEST_CASE("enable_http CLI validation and graceful stop on active SSE stream") {
        // 1) CLI validation errors return non-zero cleanly
        {
            PluginApp cliApp("cli-http", "1.0.0");
            enable_http(cliApp, HttpServerOptions{.port = 0});

            auto run_args = [&](std::initializer_list<const char *> args) {
                std::vector<std::string> storage;
                std::vector<char *> argv;
                for (const char *s : args)
                    storage.emplace_back(s);
                for (auto &s : storage)
                    argv.push_back(s.data());
                return cliApp.run(static_cast<int>(argv.size()), argv.data());
            };

            CHECK(run_args({"cli-http", "http", "--port", "not-a-number"}) == 2);
            CHECK(run_args({"cli-http", "http", "--port", "70000"}) == 2);
            CHECK(run_args({"cli-http", "http", "--unknown-flag"}) == 2);
            CHECK(run_args({"cli-http", "http", "--allow-ephemeral-port"}) == 2);
            CHECK(run_args({"cli-http", "http", "--host", "not-an-ip"}) == 1);
            CHECK(run_args({"cli-http", "--skip-checks", "http", "--host", "not-an-ip"}) == 1);
            CHECK(run_args({"cli-http", "http", "--host", "not-an-ip", "--skip-checks"}) == 1);
        }

        // 2) Graceful stop drains and cancels an open active SSE stream within shutdown timeout
        PluginApp app("graceful-app", "1.0.0");
        std::atomic<bool> started{false};
        std::atomic<bool> cancelled{false};
        std::atomic<int> queuedCalls{0};

        app.tool<std::monostate>(
            "hang_stream",
            "Emits one progress event then waits until cancelled",
            ToolOptions{.threadSafe = true},
            [&](const std::monostate &, const RequestContext &ctx) {
                ctx.emitProgress(1, 2, "started");
                started.store(true);
                while (!ctx.cancellation.isCancelled()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                cancelled.store(true);
                return "cancelled";
            });
        app.tool<std::monostate>(
            "queued", "Must not run after the shutdown deadline", ToolOptions{.threadSafe = true},
            [&](const std::monostate &) {
                queuedCalls.fetch_add(1);
                return "unexpected";
            });

        HttpServerOptions opts;
        opts.host = "127.0.0.1";
        opts.port = 0;
        opts.shutdownTimeoutMs = 200;
        opts.workerThreads = 1;
        opts.maxPendingJobs = 1;

        McpHttpServer srv(app, opts);
        std::string err;
        REQUIRE(srv.start(&err));
        const uint16_t port = srv.boundPort();
        REQUIRE(port != 0);

        TcpFd conn(connect_loopback(port));
        REQUIRE(conn.fd >= 0);

        Json p = json_obj();
        p["name"] = "hang_stream";
        p["arguments"] = json_obj();
        Json req = make_req(99, "tools/call", p);
        req["params"]["_meta"]["progressToken"] = "pt-graceful";
        REQUIRE(send_all(conn.fd,
                         build_http_request("POST", "/mcp", port, mcp_headers("tools/call", "hang_stream"),
                                            slimr::write_json(req))));

        for (int i = 0; i < 60 && !started.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        REQUIRE(started.load() == true);

        TcpFd pending = connect_loopback(port);
        TcpFd overflow = connect_loopback(port);
        REQUIRE(pending.fd >= 0);
        REQUIRE(overflow.fd >= 0);
        p["name"] = "queued";
        const auto queuedRequest = build_http_request(
            "POST", "/mcp", port, mcp_headers("tools/call", "queued"),
            slimr::write_json(make_req(100, "tools/call", p)));
        REQUIRE(send_all(pending.fd, queuedRequest));
        REQUIRE(send_all(overflow.fd, queuedRequest));
        // Whichever request arrives second must see a full queue, proving
        // another job is pending behind the active stream before stop().
        pollfd ready[]{{pending.fd, POLLIN, 0}, {overflow.fd, POLLIN, 0}};
        REQUIRE(::poll(ready, 2, 1000) > 0);
        const int rejected = (ready[0].revents & POLLIN) ? pending.fd : overflow.fd;
        CHECK(parse_http_reply(read_http_message(rejected)).status == 503);

        const auto t0 = std::chrono::steady_clock::now();
        srv.stop();
        const auto elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

        CHECK(cancelled.load() == true);
        CHECK(queuedCalls.load() == 0);
        CHECK(elapsedMs < 2000);
    }

    TEST_CASE("Streamable HTTP options validation, error status lanes, custom routes, and runUntilSignal") {
        PluginApp app("http-edge", "1.0.0");
        app.tool<AddInput>("add", "Adds", [](const AddInput &in) {
            return std::to_string(in.a.get() + in.b.get());
        });

        // Option validation failures
        {
            HttpServerOptions badPort;
            badPort.host = "127.0.0.1";
            badPort.port = -1;
            McpHttpServer srv(app, badPort);
            std::string err;
            CHECK_FALSE(srv.start(&err));
            CHECK(err.find("Invalid port") != std::string::npos);

            badPort.port = 70000;
            McpHttpServer srvHigh(app, badPort);
            CHECK_FALSE(srvHigh.start(&err));
            CHECK(err.find("Invalid port") != std::string::npos);

            HttpServerOptions badPath;
            badPath.host = "127.0.0.1";
            badPath.port = 0;
            badPath.mcpPath = "mcp";
            McpHttpServer srvPath(app, badPath);
            CHECK_FALSE(srvPath.start(&err));
            CHECK(err.find("mcpPath") != std::string::npos);
        }

        // Custom routes, error status lanes (405/415/413/400/406/404/500), and runUntilSignal
        HttpServerOptions opts;
        opts.host = "127.0.0.1";
        opts.port = 0;
        opts.maxRequestBytes = 512;
        opts.customRoutes.push_back(CustomHttpRoute{
            .method = "GET",
            .path = "/html",
            .handler = [](const HttpRouteRequest &r) {
                auto q = r.query("q");
                return HttpRouteResponse::html(200, "<p>" + q.value_or("") + "</p>");
            }});
        opts.customRoutes.push_back(CustomHttpRoute{
            .method = "GET",
            .path = "/redir",
            .handler = [](const HttpRouteRequest &) {
                return HttpRouteResponse::redirect("https://example.com/next", 302);
            }});
        opts.customRoutes.push_back(CustomHttpRoute{
            .method = "*",
            .path = "/any",
            .handler = [](const HttpRouteRequest &r) {
                if (r.query("boom").has_value()) {
                    throw std::runtime_error("route Bearer secret");
                }
                if (r.query("unk").has_value()) {
                    throw 7;
                }
                Json j = json_obj();
                j["method"] = r.method;
                return HttpRouteResponse::json(200, j);
            }});

        McpHttpServer srv(app, opts);
        std::string startErr;
        REQUIRE(srv.start(&startErr));
        uint16_t port = srv.boundPort();
        REQUIRE(port != 0);

        const std::vector<std::pair<std::string, std::string>> mcpHeaders{
            {"Content-Type", "application/json"},
            {"Accept", "application/json, text/event-stream"},
            {"Connection", "close"}};

        // 405 on GET / PUT / DELETE /mcp
        for (const char *m : {"GET", "PUT", "DELETE"}) {
            auto r = http_exchange(port, m, "/mcp", {{"Connection", "close"}}, "");
            CHECK(r.status == 405);
            CHECK(r.headers["allow"] == "POST");
        }

        // 415 Unsupported Media Type
        auto r415 = http_exchange(port, "POST", "/mcp", {{"Content-Type", "text/plain"}, {"Accept", "application/json, text/event-stream"}, {"Connection", "close"}}, "{}");
        CHECK(r415.status == 415);

        // 413 Payload Too Large
        std::string huge(600, 'a');
        auto r413 = http_exchange(port, "POST", "/mcp", mcpHeaders, huge);
        CHECK(r413.status == 413);

        // 400 Parse error, non-object, bad jsonrpc, invalid id, empty method
        CHECK(http_exchange(port, "POST", "/mcp", mcpHeaders, "{bad").status == 400);
        CHECK(http_exchange(port, "POST", "/mcp", mcpHeaders, "[1,2]").status == 400);
        CHECK(http_exchange(port, "POST", "/mcp", mcpHeaders, R"({"jsonrpc":"1.0","id":1,"method":"tools/list"})").status == 400);
        CHECK(http_exchange(port, "POST", "/mcp", mcpHeaders, R"({"jsonrpc":"2.0","id":true,"method":"tools/list"})").status == 400);
        CHECK(http_exchange(port, "POST", "/mcp", mcpHeaders, R"({"jsonrpc":"2.0","id":1,"method":""})").status == 400);

        // 406 Not Acceptable when Accept lacks text/event-stream
        auto r406 = http_exchange(port, "POST", "/mcp", {{"Content-Type", "application/json"}, {"Accept", "application/json"}, {"Connection", "close"}},
                                  slimr::write_json(make_req(1, "tools/list")));
        CHECK(r406.status == 406);

        // 404 MethodNotFound and 400 UnsupportedProtocolVersion
        std::vector<std::pair<std::string, std::string>> unknownMethodHeaders = mcpHeaders;
        unknownMethodHeaders.emplace_back("MCP-Protocol-Version", std::string(kMcpProtocolVersion));
        unknownMethodHeaders.emplace_back("Mcp-Method", "no/suchMethod");
        CHECK(http_exchange(port, "POST", "/mcp", unknownMethodHeaders, slimr::write_json(make_req(2, "no/suchMethod"))).status == 404);
        CHECK(http_exchange(port, "POST", "/mcp", mcpHeaders, slimr::write_json(make_req(3, "tools/list", json_obj(), "2024-01-01"))).status == 400);

        // Custom routes: HTML + URL decoding, redirect, wildcard, 413, 500
        auto rHtml = http_exchange(port, "GET", "/html?q=a+b%20c%2Fz%ZZ", {{"Connection", "close"}}, "");
        CHECK(rHtml.status == 200);
        CHECK(rHtml.body == "<p>a b c/z%ZZ</p>");

        auto rRedir = http_exchange(port, "GET", "/redir", {{"Connection", "close"}}, "");
        CHECK(rRedir.status == 302);
        CHECK(rRedir.headers["location"] == "https://example.com/next");

        auto rAny = http_exchange(port, "POST", "/any", {{"Content-Type", "application/json"}, {"Connection", "close"}}, "{}");
        CHECK(rAny.status == 200);
        CHECK(rAny.body.find("POST") != std::string::npos);

        auto rAnyBig = http_exchange(port, "POST", "/any", {{"Content-Type", "application/json"}, {"Connection", "close"}}, huge);
        CHECK(rAnyBig.status == 413);

        auto rAnyBoom = http_exchange(port, "GET", "/any?boom=1", {{"Connection", "close"}}, "");
        CHECK(rAnyBoom.status == 500);
        CHECK(rAnyBoom.body.find("secret") == std::string::npos);

        auto rAnyUnk = http_exchange(port, "GET", "/any?unk=1", {{"Connection", "close"}}, "");
        CHECK(rAnyUnk.status == 500);

        // runUntilSignal exits promptly when stop() is called from another thread
        std::thread stopper([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            srv.stop();
        });
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(srv.runUntilSignal() == 0);
        stopper.join();
        CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() < 2000);
    }

    TEST_CASE("OAuth helpers and evaluate_bearer_auth fail-closed branches") {
        // base64_decode edge cases
        CHECK(mcp_detail::base64_decode("").value() == "");
        CHECK(mcp_detail::base64_decode("Zg==").value() == "f");
        CHECK(mcp_detail::base64_decode("Zm8=").value() == "fo");
        CHECK(mcp_detail::base64_decode("Zm9v").value() == "foo");
        CHECK(mcp_detail::base64_decode("Zm8").value() == "fo");
        CHECK_FALSE(mcp_detail::base64_decode("Zg=a").has_value());
        CHECK_FALSE(mcp_detail::base64_decode("ab$d").has_value());

        // OAuthResourceMetadata::toJson
        OAuthResourceMetadata meta;
        meta.resource = "https://example.com/mcp";
        meta.authorizationServers = {"https://auth.example.com"};
        meta.scopesSupported = {"mcp:read", "mcp:write"};
        meta.bearerMethodsSupported = {"header"};
        meta.resourceName = "Example MCP";
        meta.resourceDocumentation = "https://example.com/docs";
        Json mj = meta.toJson("https://example.com/mcp");
        CHECK(mj["resource"].get<std::string>() == "https://example.com/mcp");
        CHECK(mj["resource_name"].get<std::string>() == "Example MCP");
        CHECK(mj["resource_documentation"].get<std::string>() == "https://example.com/docs");
        CHECK(mj["bearer_methods_supported"].get_array().size() == 1);

        // evaluate_bearer_auth fail-closed branches
        OAuthResourceConfig cfg;
        cfg.enabled = true;
        cfg.expectedIssuers = {"https://auth.example.com"};
        cfg.defaultScopes = {"mcp:read"};

        HttpRouteRequest req;
        // 1. Missing validator -> ServiceUnavailable
        CHECK(mcp_detail::evaluate_bearer_auth(cfg, "https://example.com/mcp", req).status ==
              TokenValidationResult::Status::ServiceUnavailable);

        cfg.validateToken = [](const TokenValidationContext &c) -> TokenValidationResult {
            if (c.token == "throw-std")
                throw std::runtime_error("val Bearer s3cr3t");
            if (c.token == "throw-unk")
                throw 99;
            if (c.token == "invalid-empty")
                return TokenValidationResult{.status = TokenValidationResult::Status::InvalidToken};
            if (c.token == "insuff-empty")
                return TokenValidationResult{.status = TokenValidationResult::Status::InsufficientScope};
            if (c.token == "unauth") {
                TokenValidationResult r;
                r.status = TokenValidationResult::Status::Valid;
                r.principal.authenticated = false;
                return r;
            }
            if (c.token == "expired") {
                TokenValidationResult r;
                r.status = TokenValidationResult::Status::Valid;
                r.principal.authenticated = true;
                r.principal.expiresAt = std::chrono::system_clock::now() - std::chrono::seconds(10);
                return r;
            }
            if (c.token == "bad-iss") {
                TokenValidationResult r;
                r.status = TokenValidationResult::Status::Valid;
                r.principal.authenticated = true;
                r.principal.issuer = "https://evil.example.com";
                return r;
            }
            if (c.token == "bad-aud") {
                TokenValidationResult r;
                r.status = TokenValidationResult::Status::Valid;
                r.principal.authenticated = true;
                r.principal.issuer = "https://auth.example.com";
                r.principal.audience = "https://other.example.com";
                return r;
            }
            TokenValidationResult r;
            r.status = TokenValidationResult::Status::Valid;
            r.principal.authenticated = true;
            r.principal.issuer = "https://auth.example.com";
            r.principal.audience = std::string(c.expectedResource);
            r.principal.scopes = {}; // missing default scope mcp:read
            return r;
        };

        // 2. Query-string access_token rejected
        HttpRouteRequest qReq;
        qReq.queryParams["access_token"] = "tok";
        CHECK(mcp_detail::evaluate_bearer_auth(cfg, "https://example.com/mcp", qReq).status ==
              TokenValidationResult::Status::InvalidToken);

        // 3. Missing Authorization header / non-Bearer
        CHECK(mcp_detail::evaluate_bearer_auth(cfg, "https://example.com/mcp", req).status ==
              TokenValidationResult::Status::InvalidToken);
        req.headers["authorization"] = "Basic dXNlcjpwYXNz";
        CHECK(mcp_detail::evaluate_bearer_auth(cfg, "https://example.com/mcp", req).status ==
              TokenValidationResult::Status::InvalidToken);

        auto evalTok = [&](std::string tok) {
            HttpRouteRequest r;
            r.headers["authorization"] = "Bearer " + std::move(tok);
            return mcp_detail::evaluate_bearer_auth(cfg, "https://example.com/mcp", r);
        };

        CHECK(evalTok("throw-std").status == TokenValidationResult::Status::ServiceUnavailable);
        CHECK(evalTok("throw-std").errorDescription.find("s3cr3t") == std::string::npos);
        CHECK(evalTok("throw-unk").status == TokenValidationResult::Status::ServiceUnavailable);
        CHECK(evalTok("invalid-empty").errorDescription == "Invalid or expired token");
        CHECK(evalTok("insuff-empty").errorDescription == "Insufficient scope");
        CHECK(evalTok("unauth").status == TokenValidationResult::Status::InvalidToken);
        CHECK(evalTok("expired").status == TokenValidationResult::Status::InvalidToken);
        CHECK(evalTok("bad-iss").status == TokenValidationResult::Status::InvalidToken);
        CHECK(evalTok("bad-aud").status == TokenValidationResult::Status::InvalidToken);
        auto insuff = evalTok("missing-scope");
        CHECK(insuff.status == TokenValidationResult::Status::InsufficientScope);
        CHECK(insuff.requiredScopes == std::vector<std::string>{"mcp:read"});
    }
}

#endif // SLIMR_MCP_HAS_HTTP
