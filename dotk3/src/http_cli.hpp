#pragma once
#include "runtime.hpp"
#ifndef _WIN32
#include <curl/curl.h>
#include <algorithm>
#include <cctype>
#include <memory>
#include <stdexcept>

// libcurl-backed HTTP(S) client for the Requests module; HTTP and HTTPS only, bounded size/time.
class CurlHttp {
    struct Transfer {
        std::string body, reason;
        std::vector<std::pair<std::string, std::string>> headers;
        size_t limit = 0, headerBytes = 0;
        bool bodyExceeded = false, headersExceeded = false;
    };
    struct HeaderList {
        curl_slist *head = nullptr;
        ~HeaderList() { curl_slist_free_all(head); }
        void add(const std::string &line) {
            curl_slist *next = curl_slist_append(head, line.c_str());
            if (!next) throw std::bad_alloc();
            head = next;
        }
    };
    static size_t write(char *data, size_t size, size_t count, void *target) {
        auto &transfer = *static_cast<Transfer *>(target); size_t bytes = size * count;
        if (bytes > transfer.limit - transfer.body.size()) { transfer.bodyExceeded = true; return 0; }
        transfer.body.append(data, bytes); return bytes;
    }
    static size_t header(char *data, size_t size, size_t count, void *target) {
        auto &transfer = *static_cast<Transfer *>(target); size_t bytes = size * count;
        transfer.headerBytes += bytes;
        if (transfer.headerBytes > 64 * 1024 || transfer.headers.size() > 256) { transfer.headersExceeded = true; return 0; }
        std::string line(data, bytes);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.rfind("HTTP/", 0) == 0) {
            transfer.headers.clear(); transfer.reason.clear();
            size_t first = line.find(' '), second = first == std::string::npos ? first : line.find(' ', first + 1);
            if (second != std::string::npos) transfer.reason = line.substr(second + 1);
        } else if (size_t colon = line.find(':'); colon != std::string::npos && colon > 0) {
            size_t start = colon + 1; while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) start++;
            transfer.headers.emplace_back(line.substr(0, colon), line.substr(start));
        }
        return bytes;
    }
    static void setOption(CURL *handle, CURLoption option, long value) { if (curl_easy_setopt(handle, option, value) != CURLE_OK) throw std::runtime_error("HTTP client option is unsupported"); }
public:
    static dk3::HttpResponse perform(const dk3::HttpRequest &request) {
        static const CURLcode initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (initialized != CURLE_OK) throw std::runtime_error("HTTP client initialization failed");
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(curl_easy_init(), curl_easy_cleanup);
        if (!handle) throw std::runtime_error("HTTP client initialization failed");
        CURL *curl = handle.get(); Transfer transfer; transfer.limit = request.maxResponseBytes;
        char failure[CURL_ERROR_SIZE] = {0};
        HeaderList headers; headers.add("Expect:");
        for (const auto &entry : request.headers) headers.add(entry.second.empty() ? entry.first + ";" : entry.first + ": " + entry.second);
        curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers.head);
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, failure);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &CurlHttp::write);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &CurlHttp::header);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &transfer);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "dotk3-requests/1");
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
        setOption(curl, CURLOPT_NOSIGNAL, 1L);
        setOption(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(request.timeoutMillis));
        setOption(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(std::min<uint64_t>(request.timeoutMillis, 10000)));
        setOption(curl, CURLOPT_FOLLOWLOCATION, request.maxRedirects ? 1L : 0L);
        setOption(curl, CURLOPT_MAXREDIRS, static_cast<long>(request.maxRedirects));
#if LIBCURL_VERSION_NUM >= 0x075500
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
        setOption(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
        setOption(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
        if (request.method == "HEAD") setOption(curl, CURLOPT_NOBODY, 1L);
        else if (request.method == "POST") setOption(curl, CURLOPT_POST, 1L);
        else if (request.method != "GET" || !request.body.empty()) curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request.method.c_str());
        if (request.method != "HEAD" && (request.method == "POST" || !request.body.empty())) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.data());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
        }
        CURLcode result = curl_easy_perform(curl);
        if (transfer.bodyExceeded) throw std::length_error("HTTP response exceeds host byte limit");
        if (transfer.headersExceeded) throw std::length_error("HTTP response headers exceed host limit");
        if (result != CURLE_OK) throw std::runtime_error(std::string("HTTP request failed: ") + (failure[0] ? failure : curl_easy_strerror(result)));
        long status = 0; char *effective = nullptr;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective);
        dk3::HttpResponse response; response.status = static_cast<int>(status);
        response.reason = std::move(transfer.reason); response.url = effective ? effective : request.url;
        response.body = std::move(transfer.body); response.headers = std::move(transfer.headers);
        return response;
    }
};
#endif
