#include "app/downloads/PluginFeed.h"
#include "app/downloads/CurseForge.h"
#include "app/downloads/Modrinth.h"
#include "app/system/AsyncTasks.h"
#include "app/system/Sockets.h"

#include <core/base/json.hpp>
#include <core/base/strings.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace PluginFeed {

    namespace blb = b_launcher_base;

    namespace {
        using json = nlohmann::json;
        using Clock = std::chrono::steady_clock;

        constexpr int PORT = 18801;
        constexpr size_t LIMIT = 8 * 1024 * 1024;
        // How long a client may take to send its request, and how often the worker looks for a change.
        constexpr auto CLIENT_TIMEOUT = std::chrono::seconds(3);
        constexpr auto STEP = std::chrono::milliseconds(100);
        // The page is drawn every frame; the port closes once it has not been drawn for this long.
        constexpr auto SHOWN_GRACE = std::chrono::milliseconds(500);

        std::thread worker;
        std::atomic<bool> running{false};
        std::atomic<bool> listening{false};
        std::mutex arrivedLock;
        std::string arrived;

        // The main thread only.
        bool wanted = false;
        Clock::time_point lastShown;

        bool startWinsock() {
#if defined(_WIN32)
            static bool started = [] {
                WSADATA data;
                return WSAStartup(MAKEWORD(2, 2), &data) == 0;
            }();
            return started;
#else
            return true;
#endif
        }

        SOCKET listenOn() {
            if (!startWinsock()) {
                return INVALID_SOCKET;
            }
            SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s == INVALID_SOCKET) {
                return INVALID_SOCKET;
            }
#if defined(_WIN32)
            BOOL exclusive = TRUE;
            setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*) &exclusive, sizeof(exclusive));
#else
            int reuse = 1;
            setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
            sockaddr_in address = {};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons((u_short) PORT);
            if (bind(s, (const sockaddr*) &address, sizeof(address)) != 0 || listen(s, 8) != 0) {
                closesocket(s);
                return INVALID_SOCKET;
            }
            return s;
        }

        void reply(SOCKET client) {
            const char* data = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n"
                               "Content-Length: 2\r\n\r\nok";
#if defined(MSG_NOSIGNAL)
            send(client, data, (int) std::strlen(data), MSG_NOSIGNAL);
#else
            send(client, data, (int) std::strlen(data), 0);
#endif
        }

        // The body of a POST /; anything else is answered but not read.
        bool readRequest(SOCKET client, std::string& body) {
            std::string request;
            char buffer[4096];
            size_t headEnd = std::string::npos;
            size_t length = 0;
            auto deadline = Clock::now() + CLIENT_TIMEOUT;
            while (Clock::now() < deadline && request.size() < LIMIT) {
                int n = (int) recv(client, buffer, sizeof(buffer), 0);
                if (n <= 0) {
                    return false;
                }
                request.append(buffer, (size_t) n);
                if (headEnd == std::string::npos) {
                    headEnd = request.find("\r\n\r\n");
                    if (headEnd == std::string::npos) {
                        continue;
                    }
                    if (!request.starts_with("POST / ")) {
                        return false;
                    }
                    size_t at = request.find("Content-Length:");
                    if (at != std::string::npos) {
                        length = (size_t) std::strtoul(request.c_str() + at + 15, nullptr, 10);
                    }
                }
                if (length <= LIMIT && request.size() >= headEnd + 4 + length) {
                    body = request.substr(headEnd + 4, length);
                    return true;
                }
            }
            return false;
        }

        void serveClient(SOCKET client) {
#if defined(_WIN32)
            DWORD timeout = 250;
#else
            timeval timeout = {0, 250 * 1000};
#endif
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char*) &timeout, sizeof(timeout));
            std::string body;
            bool ok = readRequest(client, body);
            reply(client);
            if (ok) {
                std::lock_guard<std::mutex> guard(arrivedLock);
                arrived = std::move(body);
            }
        }

        void serve() {
            SOCKET listener = INVALID_SOCKET;
            while (running) {
                if (!listening) {
                    if (listener != INVALID_SOCKET) {
                        closesocket(listener);
                        listener = INVALID_SOCKET;
                    }
                    std::this_thread::sleep_for(STEP / 2);
                    continue;
                }
                if (listener == INVALID_SOCKET) {
                    listener = listenOn();
                    if (listener == INVALID_SOCKET) {
                        std::this_thread::sleep_for(STEP * 5);
                        continue;
                    }
                }
                fd_set readable;
                FD_ZERO(&readable);
                FD_SET(listener, &readable);
                timeval wait = {};
                wait.tv_usec = (decltype(wait.tv_usec)) std::chrono::duration_cast<std::chrono::microseconds>(
                        STEP).count();
                if (select((int) listener + 1, &readable, nullptr, nullptr, &wait) <= 0) {
                    continue;
                }
                SOCKET client = accept(listener, nullptr, nullptr);
                if (client != INVALID_SOCKET) {
                    serveClient(client);
                    closesocket(client);
                }
            }
            if (listener != INVALID_SOCKET) {
                closesocket(listener);
            }
        }

        struct Item {
            Addons::Source source = Addons::Source::Modrinth;
            std::string id;
            std::string slug;
        };

        AsyncTasks tasks;
        bool holding = false;
        bool working = false;
        unsigned revisions = 0;
        std::vector<std::pair<Addons::Source, Addons::Hit>> list;

        bool numeric(const std::string& value) {
            return !value.empty() && value.size() < 18 &&
                    std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; });
        }

        bool fromCurseForge(const std::string& id, Addons::Hit& hit, blb::cancellation_token token) {
            auto doc = CurseForge::call("/mods/" + id, std::string(), token);
            if (!doc) {
                return false;
            }
            auto data = doc->find("data");
            if (data == doc->end() || !data->is_object()) {
                return false;
            }
            hit = CurseForge::hitFrom(*data);
            return !hit.id.empty();
        }

        bool curseSlug(const std::string& slug, Addons::Hit& hit, blb::cancellation_token token) {
            auto doc = CurseForge::call("/mods/search?gameId=432&classId=6&slug=" + blb::url_encode(slug), std::string(),
                                        token);
            if (!doc) {
                return false;
            }
            auto data = doc->find("data");
            if (data == doc->end() || !data->is_array() || data->empty()) {
                return false;
            }
            hit = CurseForge::hitFrom(data->front());
            return !hit.id.empty();
        }

        bool lookup(const Item& item, Addons::Hit& hit, blb::cancellation_token token) {
            // The id is the project's own; the slug is only a fallback, so one lookup is enough.
            std::string key = item.id.empty() ? item.slug : item.id;
            if (key.empty()) {
                return false;
            }
            if (item.source == Addons::Source::Modrinth) {
                auto doc = Modrinth::api("/project/" + blb::url_encode(key), token);
                if (!doc || !doc->is_object()) {
                    return false;
                }
                hit = Modrinth::hitFrom(*doc);
                return !hit.id.empty();
            }
            return numeric(key) ? fromCurseForge(key, hit, token) : curseSlug(key, hit, token);
        }

        void receive(std::string body) {
            json doc = json::parse(body, nullptr, false);
            if (!doc.is_array()) {
                return;
            }
            std::vector<Item> items;
            for (const json& node : doc) {
                std::string p = node.is_object() ? blb::json_text(node, "p") : std::string();
                if (p != "mr" && p != "cf") {
                    continue;
                }
                Item item;
                item.source = p == "cf" ? Addons::Source::CurseForge : Addons::Source::Modrinth;
                item.id = blb::json_text(node, "id");
                item.slug = blb::json_text(node, "slug");
                if (!item.id.empty() || !item.slug.empty()) {
                    items.push_back(std::move(item));
                }
            }
            if (items.empty()) {
                return;
            }
            unsigned mine = ++revisions;
            list.clear();
            holding = true;
            working = true;
            tasks.run(
                    [mine, items = std::move(items)](blb::cancellation_token token) -> AsyncTasks::Continuation {
                        std::vector<std::pair<Addons::Source, Addons::Hit>> hits;
                        for (const Item& item : items) {
                            Addons::Hit hit;
                            if (lookup(item, hit, token)) {
                                hits.push_back({item.source, std::move(hit)});
                            }
                        }
                        return [mine, hits = std::move(hits)]() mutable {
                            if (!holding || mine != revisions) {
                                return;
                            }
                            list = std::move(hits);
                            working = false;
                        };
                    },
                    [mine](const std::string&) -> AsyncTasks::Continuation {
                        return [mine] {
                            if (holding && mine == revisions) {
                                working = false;
                            }
                        };
                    });
        }
    }

    void shown(bool visible) {
        lastShown = Clock::now();
        wanted = visible;
        listening = visible;
        if (visible && !running) {
            running = true;
            worker = std::thread(serve);
        }
    }

    void poll() {
        tasks.poll();
        std::string body;
        {
            std::lock_guard<std::mutex> guard(arrivedLock);
            body.swap(arrived);
        }
        if (!body.empty()) {
            receive(std::move(body));
        }
        if (wanted && Clock::now() - lastShown > SHOWN_GRACE) {
            wanted = false;
            listening = false;
        }
    }

    void shutdown() {
        running = false;
        listening = false;
        if (worker.joinable()) {
            worker.join();
        }
        tasks.stop();
    }

    bool active() {
        return holding;
    }

    bool loading() {
        return working;
    }

    const std::vector<std::pair<Addons::Source, Addons::Hit>>& items() {
        return list;
    }

    unsigned revision() {
        return revisions;
    }

    void clear() {
        holding = false;
        working = false;
        list.clear();
    }
}
