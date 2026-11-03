#pragma once

#include <functional>
#include <string>
#include <unordered_map>

#include "msgpack.h"

namespace gosr
{
    // Produces the response BODY for one request's parameters.
    using Handler = std::function<msgpack::Value(const msgpack::Value& request)>;

    void RegisterHandler(const std::string& endpoint, Handler handler);
    msgpack::Value HandleRequest(const std::string& endpoint, const msgpack::Value& request);
}
