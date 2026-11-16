#pragma once
#include "mcp.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace slimr::mcp_detail {

inline std::optional<std::string> base64_decode(std::string_view input) {
    return serve_detail::base64_decode(input);
}

inline std::string base64_encode(std::string_view input) {
    return serve_detail::base64_encode(input);
}

inline std::string join_scopes(const std::vector<std::string> &scopes) {
    return serve_detail::join_scopes(scopes);
}

inline std::optional<std::string> extract_bearer_token(std::string_view authHeader) {
    return serve_detail::extract_bearer_token(authHeader);
}

inline std::string quote_header_param(std::string_view val) {
    return serve_detail::quote_header_param(val);
}

inline std::string build_www_authenticate(std::string_view resourceMetadataUrl,
                                          std::string_view error,
                                          std::span<const std::string> scopes,
                                          std::string_view errorDescription = "") {
    return serve_detail::build_www_authenticate(resourceMetadataUrl, error, scopes, errorDescription);
}

inline TokenValidationResult evaluate_bearer_auth(const OAuthResourceConfig &cfg,
                                                  std::string_view expectedResource,
                                                  const HttpRouteRequest &req) {
    return serve_detail::evaluate_bearer_auth(cfg, expectedResource, req);
}

} // namespace slimr::mcp_detail
