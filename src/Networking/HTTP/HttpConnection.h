/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#pragma once

#include <llhttp.h>

#include <mutex>
#include <string>
#include <vector>

#include "Networking/HTTP/HttpTypes.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

#include <openssl/ssl.h>

namespace EntropyEngine::Networking::HTTP
{

// A single HTTP/1.1 connection operating on a TCP socket.
// Sends one aggregated request and parses one aggregated response using llhttp.
class HttpConnection
{
public:
    HttpConnection();

    // Perform a single request on the given socket. Returns whether the socket can be kept alive.
    // The socket must already be connected to host:port.
    bool executeOnSocket(int sock, const HttpRequest& req, const RequestOptions& opts, HttpResponse& out);

    // Perform a single HTTPS request over an established TLS session (SSL*).
    // The SSL object must be connected and associated with a TCP socket.
    bool executeOnSsl(SSL* ssl, const HttpRequest& req, const RequestOptions& opts, HttpResponse& out);

private:
    static std::string methodToString(HttpMethod m);
    static void toLowerInPlace(std::string& s);

    // llhttp state per-call (stack allocated in execute methods)
};

}  // namespace EntropyEngine::Networking::HTTP
