#include "tether/app_icons.hpp"
#include "tether/log.hpp"
#include "tether/paths.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <fcntl.h>
#include <fstream>
#include <mutex>
#include <netdb.h>
#include <nlohmann/json.hpp>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <poll.h>
#include <set>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace tether::app_icons {

    namespace {

        constexpr const char* LOOKUP_HOST = "itunes.apple.com";
        constexpr const char* ARTWORK_HOST_SUFFIX = ".mzstatic.com";
        // Anything far bigger than a lookup answer or icon is not what we asked for.
        constexpr size_t MAX_RESPONSE_BYTES = 1024 * 1024;
        constexpr int TIMEOUT_SECONDS = 10;
        // How long before an app the store does not know is looked up again.
        constexpr auto RETRY_MISSING_AFTER = std::chrono::hours(24 * 7);

        // Apple allows about 20 lookups a minute.
        constexpr auto LOOKUP_SPACING = std::chrono::seconds(3);
        constexpr auto RATE_LIMIT_BACKOFF = std::chrono::seconds(60);

        ImageFilter g_image_filter;

        bool is_id_char(char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_';
        }

        std::filesystem::path cache_root() {
            const auto dir = paths::cache_dir();
            return dir.empty() ? dir : dir / "app-icons";
        }

        std::filesystem::path missing_marker(std::string_view app_id) {
            return cache_root() / (std::string(app_id) + ".none");
        }

        // Splits an https URL into host and path-with-query; other schemes are rejected.
        bool split_https_url(const std::string& url, std::string& host, std::string& target) {
            constexpr std::string_view scheme = "https://";
            if (url.rfind(scheme, 0) != 0)
                return false;
            const size_t slash = url.find('/', scheme.size());
            host = url.substr(scheme.size(), slash == std::string::npos ? std::string::npos : slash - scheme.size());
            target = slash == std::string::npos ? "/" : url.substr(slash);
            return !host.empty() && host.find_first_of(":@") == std::string::npos;
        }

        // A blocking TCP connection to `host`:443 that gives up after TIMEOUT_SECONDS; -1 on failure.
        int connect_to(const std::string& host) {
            addrinfo hints{};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            addrinfo* results = nullptr;
            if (getaddrinfo(host.c_str(), "443", &hints, &results) != 0 || !results)
                return -1;
            int fd = -1;
            for (addrinfo* ai = results; ai && fd < 0; ai = ai->ai_next) {
                fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, ai->ai_protocol);
                if (fd < 0)
                    continue;
                if (connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
                    pollfd pfd{fd, POLLOUT, 0};
                    int error = 0;
                    socklen_t len = sizeof(error);
                    if (errno != EINPROGRESS || poll(&pfd, 1, TIMEOUT_SECONDS * 1000) != 1 ||
                        getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) != 0 || error != 0) {
                        close(fd);
                        fd = -1;
                    }
                }
            }
            freeaddrinfo(results);
            if (fd < 0)
                return -1;
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
            const timeval timeout{TIMEOUT_SECONDS, 0};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
            return fd;
        }

        // Verifies servers against the system's CA store.
        SSL_CTX* tls_context() {
            static SSL_CTX* ctx = [] {
                SSL_CTX* c = SSL_CTX_new(TLS_client_method());
                if (c) {
                    SSL_CTX_set_min_proto_version(c, TLS1_2_VERSION);
                    SSL_CTX_set_verify(c, SSL_VERIFY_PEER, nullptr);
                    SSL_CTX_set_default_verify_paths(c);
                }
                return c;
            }();
            return ctx;
        }

        // HTTPS GET; HTTP/1.0 avoids chunked bodies. Returns the status, or 0 on transport failure.
        int https_get(const std::string& url, std::string& body) {
            std::string host, target;
            if (!split_https_url(url, host, target) || !tls_context())
                return 0;

            const int fd = connect_to(host);
            if (fd < 0) {
                debug::log(INFO, "app icons: could not reach {}", host);
                return 0;
            }
            SSL* ssl = SSL_new(tls_context());
            SSL_set_fd(ssl, fd);
            SSL_set_tlsext_host_name(ssl, host.c_str());
            SSL_set1_host(ssl, host.c_str());

            std::string raw;
            const std::string request = "GET " + target + " HTTP/1.0\r\nHost: " + host +
                                        "\r\nUser-Agent: tether\r\nAccept: */*\r\nConnection: close\r\n\r\n";
            if (SSL_connect(ssl) != 1 || SSL_write(ssl, request.data(), static_cast<int>(request.size())) <= 0) {
                debug::log(INFO,
                           "app icons: TLS to {} failed: {}",
                           host,
                           X509_verify_cert_error_string(SSL_get_verify_result(ssl)));
            } else {
                char buffer[16384];
                int n = 0;
                // A peer that closes without close_notify has still delivered the body.
                while (raw.size() <= MAX_RESPONSE_BYTES && (n = SSL_read(ssl, buffer, sizeof(buffer))) > 0)
                    raw.append(buffer, static_cast<size_t>(n));
                SSL_shutdown(ssl);
            }
            SSL_free(ssl);
            close(fd);

            if (raw.empty() || raw.size() > MAX_RESPONSE_BYTES)
                return 0;
            return parse_http_response(raw, body);
        }

        bool write_atomically(const std::filesystem::path& path, std::string_view data) {
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            const auto tmp = path.string() + ".tmp";
            {
                std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
                if (!file || !file.write(data.data(), static_cast<std::streamsize>(data.size())))
                    return false;
            }
            std::filesystem::rename(tmp, path, ec);
            if (ec) {
                std::filesystem::remove(tmp, ec);
                return false;
            }
            return true;
        }

        bool looked_up_recently(std::string_view app_id) {
            std::error_code ec;
            const auto written = std::filesystem::last_write_time(missing_marker(app_id), ec);
            if (ec)
                return false;
            return std::filesystem::file_time_type::clock::now() - written < RETRY_MISSING_AFTER;
        }

        // Stores to ask, best first: the user's own, then the default.
        std::vector<std::string> countries() {
            std::vector<std::string> out;
            for (const char* variable : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
                const char* value = getenv(variable);
                if (value && *value) {
                    if (auto country = store_country(value); !country.empty())
                        out.push_back(country);
                    break;
                }
            }
            if (std::find(out.begin(), out.end(), "us") == out.end())
                out.emplace_back("us");
            return out;
        }

        // Earliest time of the next lookup request; only the worker thread touches it.
        std::chrono::steady_clock::time_point g_next_lookup;

        // Returns false when the lookup should be retried later, so nothing is remembered.
        bool fetch(const std::string& app_id) {
            std::string artwork;
            bool not_in_store = true;
            for (const auto& country : countries()) {
                std::this_thread::sleep_until(g_next_lookup);
                std::string body;
                const int status = https_get(lookup_url(app_id, country), body);
                g_next_lookup = std::chrono::steady_clock::now() + LOOKUP_SPACING;
                if (status == 429) {
                    debug::log(INFO, "app icons: the App Store is rate limiting lookups; pausing for a minute");
                    g_next_lookup = std::chrono::steady_clock::now() + RATE_LIMIT_BACKOFF;
                }
                const Lookup outcome = lookup_outcome(status, body);
                if (outcome == Lookup::TryLater) {
                    debug::log(INFO, "app icons: lookup for {} failed (HTTP {}); will retry", app_id, status);
                    return false;
                }
                if (outcome == Lookup::Found) {
                    artwork = artwork_url(body);
                    not_in_store = false;
                    break;
                }
            }

            if (not_in_store) {
                debug::log(INFO, "app icons: the App Store has no icon for {}", app_id);
                write_atomically(missing_marker(app_id), "");
                return true;
            }

            std::string png;
            if (https_get(artwork, png) != 200 || !is_png(png)) {
                debug::log(INFO, "app icons: could not download the icon for {}", app_id);
                return false;
            }
            if (g_image_filter) {
                if (std::string shaped = g_image_filter(png); is_png(shaped))
                    png = std::move(shaped);
            }
            if (write_atomically(icon_path(app_id), png)) {
                std::error_code ec;
                std::filesystem::remove(missing_marker(app_id), ec);
                debug::log(INFO, "app icons: cached the icon for {}", app_id);
            }
            return true;
        }

        // Single lazily started worker, so an app is never looked up twice concurrently.
        struct Worker {
            std::mutex mutex;
            std::condition_variable wake;
            std::deque<std::string> queue;
            std::set<std::string> known;   // queued, in flight, or done this run
            std::set<std::string> pending; // queued or in flight
            bool started = false;

            void run() {
                for (;;) {
                    std::string app_id;
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        wake.wait(lock, [this] { return !queue.empty(); });
                        app_id = std::move(queue.front());
                        queue.pop_front();
                    }
                    const bool done = fetch(app_id);
                    std::lock_guard<std::mutex> lock(mutex);
                    pending.erase(app_id);
                    // Retried on the app's next notification.
                    if (!done)
                        known.erase(app_id);
                }
            }
        };

        Worker& worker() {
            static Worker* instance = new Worker(); // never destroyed: the detached thread outlives statics
            return *instance;
        }

    } // namespace

    bool valid_app_id(std::string_view app_id) {
        if (app_id.empty() || app_id.size() > 255 || app_id.front() == '.' || app_id.find("..") != app_id.npos)
            return false;
        return std::all_of(app_id.begin(), app_id.end(), is_id_char);
    }

    std::string lookup_url(std::string_view app_id, std::string_view country) {
        std::string url = "https://" + std::string(LOOKUP_HOST) + "/lookup?bundleId=" + std::string(app_id);
        if (!country.empty())
            url += "&country=" + std::string(country);
        return url;
    }

    std::string store_country(std::string_view locale) {
        const size_t underscore = locale.find('_');
        if (underscore == locale.npos || underscore + 3 > locale.size())
            return {};
        std::string country(locale.substr(underscore + 1, 2));
        if (!std::isalpha(static_cast<unsigned char>(country[0])) ||
            !std::isalpha(static_cast<unsigned char>(country[1])))
            return {};
        for (auto& c : country)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return country;
    }

    std::string artwork_url(std::string_view lookup_json) {
        const auto j = nlohmann::json::parse(lookup_json, nullptr, false);
        if (!j.is_object() || !j.contains("results") || !j["results"].is_array() || j["results"].empty())
            return {};
        const auto& app = j["results"][0];
        if (!app.is_object())
            return {};

        std::string url;
        for (const char* key : {"artworkUrl512", "artworkUrl100", "artworkUrl60"}) {
            if (app.contains(key) && app[key].is_string()) {
                url = app[key].get<std::string>();
                break;
            }
        }

        std::string host, target;
        if (url.empty() || !split_https_url(url, host, target))
            return {};
        const std::string_view suffix = ARTWORK_HOST_SUFFIX;
        if (host.size() <= suffix.size() || host.compare(host.size() - suffix.size(), suffix.size(), suffix) != 0)
            return {};

        // Replace the rendition name (e.g. "512x512bb.jpg") with a PNG at ICON_SIZE.
        const size_t slash = url.rfind('/');
        if (slash + 1 >= url.size())
            return {};
        const std::string size = std::to_string(ICON_SIZE);
        return url.substr(0, slash + 1) + size + "x" + size + "bb.png";
    }

    Lookup lookup_outcome(int status, std::string_view body) {
        if (status != 200)
            return Lookup::TryLater;
        return artwork_url(body).empty() ? Lookup::NotInStore : Lookup::Found;
    }

    int parse_http_response(std::string_view raw, std::string& body) {
        const size_t end_of_headers = raw.find("\r\n\r\n");
        if (end_of_headers == raw.npos || raw.rfind("HTTP/1.", 0) != 0)
            return 0;
        const size_t space = raw.find(' ');
        if (space == raw.npos || space + 4 > raw.size())
            return 0;
        int status = 0;
        for (size_t i = space + 1; i < space + 4; ++i) {
            if (!std::isdigit(static_cast<unsigned char>(raw[i])))
                return 0;
            status = status * 10 + (raw[i] - '0');
        }

        body.assign(raw.substr(end_of_headers + 4));

        // A body shorter than Content-Length means the connection was cut.
        std::string headers(raw.substr(0, end_of_headers));
        std::transform(headers.begin(), headers.end(), headers.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (const size_t at = headers.find("\r\ncontent-length:"); at != std::string::npos) {
            const size_t length = std::strtoull(headers.c_str() + at + 17, nullptr, 10);
            if (body.size() < length)
                return 0;
            body.resize(length);
        }
        return status;
    }

    bool is_png(std::string_view data) {
        static constexpr char signature[] = {'\x89', 'P', 'N', 'G', '\r', '\n', '\x1a', '\n'};
        return data.size() > sizeof(signature) && std::equal(std::begin(signature), std::end(signature), data.begin());
    }

    void apply_icon_shape(uint8_t* rgba, int width, int height, int stride) {
        if (!rgba || width <= 0 || height <= 0)
            return;
        // Superellipse |x|^n + |y|^n <= 1; n = 5 approximates the iOS icon mask.
        constexpr double EXPONENT = 5.0;
        constexpr int SAMPLES = 4; // per axis, for the anti-aliased edge
        for (int y = 0; y < height; ++y) {
            uint8_t* row = rgba + static_cast<size_t>(y) * static_cast<size_t>(stride);
            for (int x = 0; x < width; ++x) {
                int inside = 0;
                for (int sy = 0; sy < SAMPLES; ++sy) {
                    const double ny = std::fabs((y + (sy + 0.5) / SAMPLES) * 2.0 / height - 1.0);
                    const double py = std::pow(ny, EXPONENT);
                    for (int sx = 0; sx < SAMPLES; ++sx) {
                        const double nx = std::fabs((x + (sx + 0.5) / SAMPLES) * 2.0 / width - 1.0);
                        if (std::pow(nx, EXPONENT) + py <= 1.0)
                            ++inside;
                    }
                }
                if (inside == SAMPLES * SAMPLES)
                    continue;
                uint8_t& alpha = row[x * 4 + 3];
                alpha = static_cast<uint8_t>((alpha * inside + (SAMPLES * SAMPLES) / 2) / (SAMPLES * SAMPLES));
            }
        }
    }

    void set_image_filter(ImageFilter filter) { g_image_filter = std::move(filter); }

    std::filesystem::path icon_path(std::string_view app_id) {
        const auto root = cache_root();
        return root.empty() ? root : root / (std::string(app_id) + ".png");
    }

    std::string cached_icon(std::string_view app_id) {
        if (!valid_app_id(app_id))
            return {};
        const auto path = icon_path(app_id);
        std::error_code ec;
        if (path.empty() || !std::filesystem::is_regular_file(path, ec) || std::filesystem::file_size(path, ec) == 0)
            return {};
        return path.string();
    }

    void request(const std::string& app_id) {
        if (!valid_app_id(app_id) || cache_root().empty())
            return;

        auto& w = worker();
        {
            std::lock_guard<std::mutex> lock(w.mutex);
            if (!w.known.insert(app_id).second)
                return;
        }
        // Checked outside the lock because both touch the disk.
        if (!cached_icon(app_id).empty() || looked_up_recently(app_id))
            return;

        std::lock_guard<std::mutex> lock(w.mutex);
        w.queue.push_back(app_id);
        w.pending.insert(app_id);
        if (!w.started) {
            w.started = true;
            std::thread([&w] { w.run(); }).detach();
        }
        w.wake.notify_one();
    }

    bool fetching(const std::string& app_id) {
        auto& w = worker();
        std::lock_guard<std::mutex> lock(w.mutex);
        return w.pending.contains(app_id);
    }

} // namespace tether::app_icons
