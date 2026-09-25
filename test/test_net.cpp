#include <gtest/gtest.h>

#include <tether/bluetooth/config.hpp>
#include <tether/bluetooth/monitor.hpp>
#include <tether/client.hpp>
#include <tether/crypto.hpp>
#include <tether/event_loop.hpp>
#include <tether/log.hpp>
#include <tether/net.hpp>
#include <tether/wayland.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <netinet/in.h>
#include <sstream>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

    std::string unique_test_dir(const std::string& prefix) {
        return (std::filesystem::temp_directory_path() / (prefix + "_" + std::to_string(getpid()))).string();
    }

    class ScopedEnvVar {
    public:
        ScopedEnvVar(const std::string& key, const std::string& value) : key_(key), had_original_(false) {
            const char* current = std::getenv(key_.c_str());
            if (current != nullptr) {
                original_value_ = current;
                had_original_ = true;
            }
            if (value.empty()) {
                unsetenv(key_.c_str());
            } else {
                setenv(key_.c_str(), value.c_str(), 1);
            }
        }

        ~ScopedEnvVar() {
            if (had_original_) {
                setenv(key_.c_str(), original_value_.c_str(), 1);
            } else {
                unsetenv(key_.c_str());
            }
        }

    private:
        std::string key_;
        std::string original_value_;
        bool had_original_;
    };

    // Helper RAII class to ensure proper cleanup
    class CleanupGuard {
    public:
        CleanupGuard(const std::string& path) : path_(path) {}
        ~CleanupGuard() noexcept {
            try {
                std::filesystem::remove_all(path_);
            } catch (...) {
            }
        }

    private:
        std::string path_;
    };

    // Helper RAII class to manage process lifecycle
    class EventLoopGuard {
    public:
        explicit EventLoopGuard(tether::EpollEventLoop& loop) : loop_(loop), thread_([&loop] { loop.run(); }) {}
        ~EventLoopGuard() {
            loop_.post([this] { loop_.stop(); });
            thread_.join();
        }

    private:
        tether::EpollEventLoop& loop_;
        std::thread thread_;
    };

    class ProcessGuard {
    public:
        ProcessGuard(pid_t pid) : pid_(pid) {}
        ~ProcessGuard() {
            if (pid_ <= 0)
                return;

            kill(pid_, SIGKILL);
            int status = 0;
            waitpid(pid_, &status, 0);
            pid_ = -1;
        }

    private:
        pid_t pid_;
    };

    // A daemon an init system starts has no XDG_RUNTIME_DIR, yet has to share a socket with login shells.
    TEST(RuntimeDirTest, FallsBackToRunUserWhenXdgRuntimeDirIsUnset) {
        ScopedEnvVar unset("XDG_RUNTIME_DIR", "");
        const std::string run_user = "/run/user/" + std::to_string(getuid());
        struct stat st{};
        if (lstat(run_user.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == getuid())
            EXPECT_EQ(tether::get_runtime_dir(), run_user + "/tether");
        else
            EXPECT_THROW(tether::get_runtime_dir(), std::runtime_error);
    }

    // The single-instance lock fd is deliberately leaked so the OS holds it for the
    // process lifetime. Without O_CLOEXEC any helper the daemon exec's (btmgmt, the
    // pair dialog) inherits it and keeps the lock alive after the daemon is gone,
    // which permanently blocks restarts with "tetherd is already running".
    TEST(SingleInstanceLockTest, ExecedHelpersDoNotKeepTheLockAliveAfterTheDaemonExits) {
        const std::string runtime_dir = unique_test_dir("tether_lock_test");
        CleanupGuard cleanup_guard(runtime_dir);
        std::filesystem::remove_all(runtime_dir);
        std::filesystem::create_directories(runtime_dir);
        ScopedEnvVar xdg_runtime_dir("XDG_RUNTIME_DIR", runtime_dir);

        int pipefd[2];
        ASSERT_EQ(pipe(pipefd), 0);

        const pid_t daemon = fork();
        ASSERT_GE(daemon, 0);
        if (daemon == 0) {
            close(pipefd[0]);
            try {
                tether::ensure_single_instance();
            } catch (...) {
                _exit(1);
            }

            const pid_t helper = fork();
            if (helper == 0) {
                execlp("sleep", "sleep", "30", nullptr);
                _exit(127);
            }
            if (helper < 0)
                _exit(1);

            if (write(pipefd[1], &helper, sizeof(helper)) != static_cast<ssize_t>(sizeof(helper)))
                _exit(1);
            close(pipefd[1]);
            _exit(0);
        }

        close(pipefd[1]);
        pid_t helper = -1;
        ASSERT_EQ(read(pipefd[0], &helper, sizeof(helper)), static_cast<ssize_t>(sizeof(helper)));
        close(pipefd[0]);

        ProcessGuard helper_guard(helper);

        int wstatus = 0;
        ASSERT_EQ(waitpid(daemon, &wstatus, 0), daemon);
        ASSERT_TRUE(WIFEXITED(wstatus));
        ASSERT_EQ(WEXITSTATUS(wstatus), 0);

        // Between fork and exec the helper holds a plain duplicate of the lock fd;
        // O_CLOEXEC only takes effect once the exec completes.
        const std::string comm_path = "/proc/" + std::to_string(helper) + "/comm";
        bool execed = false;
        for (int attempt = 0; attempt < 200 && !execed; ++attempt) {
            std::ifstream comm(comm_path);
            std::string name;
            if (comm && std::getline(comm, name) && name == "sleep")
                execed = true;
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_TRUE(execed) << "helper never exec'd; cannot tell the lock apart from a failed exec";

        const std::string lock_path = tether::get_runtime_dir() + "/tetherd.lock";
        const int fd = open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        ASSERT_GE(fd, 0);

        int rc = -1;
        for (int attempt = 0; attempt < 200 && rc != 0; ++attempt) {
            rc = flock(fd, LOCK_EX | LOCK_NB);
            if (rc != 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        EXPECT_EQ(rc, 0) << "the exec'd helper still holds the daemon lock (errno " << errno << ")";
        close(fd);
    }

    // Captures std::cerr under the same mutex debug::log() writes with, so the
    // loop thread cannot tear a line while the test reads the buffer.
    class CerrCapture {
    public:
        CerrCapture() {
            std::lock_guard<std::mutex> lock(debug::log_mutex);
            previous_ = std::cerr.rdbuf(buffer_.rdbuf());
        }
        ~CerrCapture() { restore(); }

        std::string str() {
            std::lock_guard<std::mutex> lock(debug::log_mutex);
            return buffer_.str();
        }

        void restore() {
            std::lock_guard<std::mutex> lock(debug::log_mutex);
            if (previous_) {
                std::cerr.rdbuf(previous_);
                previous_ = nullptr;
            }
        }

    private:
        std::ostringstream buffer_;
        std::streambuf* previous_ = nullptr;
    };

    TEST(TcpServerTest, ReportsWhyATlsHandshakeFailed) {
        const std::string home = unique_test_dir("tether_net_test");
        CleanupGuard cleanup_guard(home);
        std::filesystem::remove_all(home);
        std::filesystem::create_directories(home);
        ScopedEnvVar home_env("HOME", home);
        ASSERT_TRUE(tether::Crypto::instance().init());

        constexpr int port = 45134;
        tether::EpollEventLoop loop;
        tether::TcpServer server(loop, port);
        if (!server.start())
            GTEST_SKIP() << "port " << port << " is unavailable";

        CerrCapture captured;
        std::thread loop_thread([&loop] { loop.run(); });

        const int client = socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_GE(client, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ASSERT_EQ(connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);

        const char garbage[] = "this is not a tls client hello\n";
        ASSERT_GT(write(client, garbage, sizeof(garbage) - 1), 0);
        shutdown(client, SHUT_RDWR);
        close(client);

        std::string log;
        for (int attempt = 0; attempt < 200; ++attempt) {
            log = captured.str();
            if (log.find("TLS handshake failed") != std::string::npos)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        loop.post([&loop] { loop.stop(); });
        loop_thread.join();
        captured.restore();

        EXPECT_NE(log.find("TLS handshake failed"), std::string::npos) << "captured log:\n" << log;
        EXPECT_NE(log.find("127.0.0.1"), std::string::npos) << "captured log:\n" << log;
    }

    // The daemon advertises AAAA records over mDNS, so it has to accept on IPv6.
    TEST(TcpServerTest, AcceptsIpv6Clients) {
        const std::string home = unique_test_dir("tether_net_v6_test");
        CleanupGuard cleanup_guard(home);
        std::filesystem::remove_all(home);
        std::filesystem::create_directories(home);
        ScopedEnvVar home_env("HOME", home);
        ASSERT_TRUE(tether::Crypto::instance().init());

        constexpr int port = 45135;
        tether::EpollEventLoop loop;
        tether::TcpServer server(loop, port);
        if (!server.start())
            GTEST_SKIP() << "port " << port << " is unavailable";

        CerrCapture captured;
        std::thread loop_thread([&loop] { loop.run(); });

        const int client = socket(AF_INET6, SOCK_STREAM, 0);
        bool connected = false;
        if (client >= 0) {
            sockaddr_in6 addr{};
            addr.sin6_family = AF_INET6;
            addr.sin6_port = htons(port);
            addr.sin6_addr = in6addr_loopback;
            connected = connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
        }

        std::string log;
        if (connected) {
            const char garbage[] = "this is not a tls client hello\n";
            EXPECT_GT(write(client, garbage, sizeof(garbage) - 1), 0);
            shutdown(client, SHUT_RDWR);

            for (int attempt = 0; attempt < 200; ++attempt) {
                log = captured.str();
                if (log.find("TLS handshake failed") != std::string::npos)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        if (client >= 0)
            close(client);

        loop.post([&loop] { loop.stop(); });
        loop_thread.join();
        captured.restore();

        if (!connected)
            GTEST_SKIP() << "no IPv6 loopback on this host";

        EXPECT_NE(log.find("TLS handshake failed"), std::string::npos) << "captured log:\n" << log;
        EXPECT_NE(log.find("::1"), std::string::npos) << "captured log:\n" << log;
    }

    std::string read_socket_line(int fd) {
        std::string line;
        for (char byte = 0; byte != '\n';) {
            const ssize_t count = recv(fd, &byte, 1, 0);
            if (count <= 0)
                return {};
            line.push_back(byte);
        }
        return line;
    }

    TEST(UnixServerTest, BluetoothResultsPreserveOperationId) {
        const std::string runtime_dir = unique_test_dir("tether_unix_operation_test");
        CleanupGuard cleanup_guard(runtime_dir);
        std::filesystem::remove_all(runtime_dir);
        std::filesystem::create_directories(runtime_dir);
        ScopedEnvVar xdg_runtime_dir("XDG_RUNTIME_DIR", runtime_dir);

        tether::EpollEventLoop loop;
        tether::TcpServer tcp_server(loop, 0);
        tether::UnixServer unix_server(loop, tcp_server);
        ASSERT_TRUE(unix_server.start());
        EventLoopGuard loop_guard(loop);

        const int client = socket(AF_UNIX, SOCK_STREAM, 0);
        ASSERT_GE(client, 0);
        timeval timeout{2, 0};
        ASSERT_EQ(setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)), 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::snprintf(
            address.sun_path, sizeof(address.sun_path), "%s", (tether::get_runtime_dir() + "/tetherd.sock").c_str());
        ASSERT_EQ(connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);

        const std::string subscribe = "{\"command\":\"subscribe\"}\n";
        ASSERT_EQ(write(client, subscribe.data(), subscribe.size()), static_cast<ssize_t>(subscribe.size()));
        ASSERT_FALSE(read_socket_line(client).empty());

        const std::string pair =
            "{\"command\":\"bt_pair\",\"address\":\"38:9C:B2:42:3F:E7\",\"operation_id\":\"web-pair-1\"}\n";
        ASSERT_EQ(write(client, pair.data(), pair.size()), static_cast<ssize_t>(pair.size()));

        nlohmann::json result;
        for (int event = 0; event < 8; ++event) {
            const std::string line = read_socket_line(client);
            if (line.empty())
                break;
            try {
                auto candidate = nlohmann::json::parse(line);
                if (candidate.value("command", "") == "bt_pair_result") {
                    result = std::move(candidate);
                    break;
                }
            } catch (...) {
            }
        }
        ASSERT_FALSE(result.is_null());
        EXPECT_EQ(result.value("operation_id", ""), "web-pair-1");

        const std::string unpair =
            "{\"command\":\"bt_unpair\",\"address\":\"38:9C:B2:42:3F:E7\",\"operation_id\":\"web-unpair-1\"}\n";
        ASSERT_EQ(write(client, unpair.data(), unpair.size()), static_cast<ssize_t>(unpair.size()));

        result = nullptr;
        for (int event = 0; event < 8; ++event) {
            const std::string line = read_socket_line(client);
            if (line.empty())
                break;
            try {
                auto candidate = nlohmann::json::parse(line);
                if (candidate.value("command", "") == "bt_unpair_result") {
                    result = std::move(candidate);
                    break;
                }
            } catch (...) {
            }
        }
        close(client);

        ASSERT_FALSE(result.is_null());
        EXPECT_EQ(result.value("operation_id", ""), "web-unpair-1");
    }

    TEST(UnixServerTest, FileSendCompletionEchoesOptionalOperationId) {
        const std::string runtime_dir = unique_test_dir("tether_unix_file_send_test");
        CleanupGuard cleanup_guard(runtime_dir);
        std::filesystem::remove_all(runtime_dir);
        std::filesystem::create_directories(runtime_dir);
        ScopedEnvVar xdg_runtime_dir("XDG_RUNTIME_DIR", runtime_dir);

        tether::EpollEventLoop loop;
        tether::TcpServer tcp_server(loop, 0);
        tether::UnixServer unix_server(loop, tcp_server);
        ASSERT_TRUE(unix_server.start());
        EventLoopGuard loop_guard(loop);

        const int client = socket(AF_UNIX, SOCK_STREAM, 0);
        ASSERT_GE(client, 0);
        timeval timeout{2, 0};
        ASSERT_EQ(setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)), 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::snprintf(
            address.sun_path, sizeof(address.sun_path), "%s", (tether::get_runtime_dir() + "/tetherd.sock").c_str());
        ASSERT_EQ(connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);

        const std::string subscribe = "{\"command\":\"subscribe\"}\n";
        ASSERT_EQ(write(client, subscribe.data(), subscribe.size()), static_cast<ssize_t>(subscribe.size()));
        ASSERT_FALSE(read_socket_line(client).empty());

        for (const std::string operation_id : {"web-file-1", ""}) {
            nlohmann::json send = {{"command", "send_file"}, {"path", runtime_dir + "/missing.txt"}};
            if (!operation_id.empty())
                send["operation_id"] = operation_id;
            const std::string payload = send.dump() + "\n";
            ASSERT_EQ(write(client, payload.data(), payload.size()), static_cast<ssize_t>(payload.size()));

            nlohmann::json result;
            for (int attempt = 0; attempt < 8; ++attempt) {
                const std::string line = read_socket_line(client);
                if (line.empty())
                    break;
                try {
                    auto candidate = nlohmann::json::parse(line);
                    if (candidate.value("command", "") == "file_send_complete") {
                        result = std::move(candidate);
                        break;
                    }
                } catch (...) {
                }
            }
            ASSERT_FALSE(result.is_null());
            EXPECT_FALSE(result.value("success", true));
            if (operation_id.empty())
                EXPECT_FALSE(result.contains("operation_id"));
            else
                EXPECT_EQ(result.value("operation_id", ""), operation_id);
        }

        // The subscribed socket stays open, but an oversized runtime path makes
        // the worker's internal Client::connect fail without spawning a daemon.
        {
            ScopedEnvVar unconnectable_runtime("XDG_RUNTIME_DIR", runtime_dir + "/" + std::string(110, 'x'));
            const std::string payload = nlohmann::json{{"command", "send_file"},
                                                       {"path", runtime_dir + "/missing.txt"},
                                                       {"operation_id", "web-connect-failure"}}
                                            .dump() +
                                        "\n";
            ASSERT_EQ(write(client, payload.data(), payload.size()), static_cast<ssize_t>(payload.size()));
            const std::string line = read_socket_line(client);
            ASSERT_FALSE(line.empty()) << "internal connect failure must produce a terminal event";
            const auto result = nlohmann::json::parse(line);
            EXPECT_EQ(result.value("command", ""), "file_send_complete");
            EXPECT_FALSE(result.value("success", true));
            EXPECT_FALSE(result.value("message", "").empty());
            EXPECT_EQ(result.value("operation_id", ""), "web-connect-failure");
        }
        close(client);
    }

    TEST(UnixServerTest, MessageSendResultEchoesOptionalOperationId) {
        const std::string runtime_dir = unique_test_dir("tether_unix_message_send_test");
        CleanupGuard cleanup_guard(runtime_dir);
        std::filesystem::remove_all(runtime_dir);
        std::filesystem::create_directories(runtime_dir);
        ScopedEnvVar xdg_runtime_dir("XDG_RUNTIME_DIR", runtime_dir);

        tether::EpollEventLoop loop;
        tether::TcpServer tcp_server(loop, 0);
        tether::UnixServer unix_server(loop, tcp_server);
        ASSERT_TRUE(unix_server.start());
        EventLoopGuard loop_guard(loop);

        const int client = socket(AF_UNIX, SOCK_STREAM, 0);
        ASSERT_GE(client, 0);
        timeval timeout{2, 0};
        ASSERT_EQ(setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)), 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::snprintf(
            address.sun_path, sizeof(address.sun_path), "%s", (tether::get_runtime_dir() + "/tetherd.sock").c_str());
        ASSERT_EQ(connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);

        const std::string subscribe = "{\"command\":\"subscribe\"}\n";
        ASSERT_EQ(write(client, subscribe.data(), subscribe.size()), static_cast<ssize_t>(subscribe.size()));
        ASSERT_FALSE(read_socket_line(client).empty());

        for (const std::string operation_id : {"web-message-1", ""}) {
            nlohmann::json send = {{"command", "bt_send_message"}, {"thread", "invalid"}, {"body", "test"}};
            if (!operation_id.empty())
                send["operation_id"] = operation_id;
            const std::string payload = send.dump() + "\n";
            ASSERT_EQ(write(client, payload.data(), payload.size()), static_cast<ssize_t>(payload.size()));

            nlohmann::json result;
            for (int attempt = 0; attempt < 8; ++attempt) {
                const std::string line = read_socket_line(client);
                if (line.empty())
                    break;
                try {
                    auto candidate = nlohmann::json::parse(line);
                    if (candidate.value("command", "") == "bt_send_result") {
                        result = std::move(candidate);
                        break;
                    }
                } catch (...) {
                }
            }
            ASSERT_FALSE(result.is_null());
            EXPECT_FALSE(result.value("success", true));
            EXPECT_EQ(result.value("thread", ""), "invalid");
            if (operation_id.empty())
                EXPECT_FALSE(result.contains("operation_id"));
            else
                EXPECT_EQ(result.value("operation_id", ""), operation_id);
        }
        close(client);
    }

    TEST(ControlProtocolTest, AdvertisesVersionedCapabilities) {
        const auto info = tether::build_protocol_info();
        EXPECT_EQ(info.value("command", ""), "protocol_info");
        EXPECT_EQ(info.value("version", 0), 1);
        const auto& capabilities = info.at("capabilities");
        EXPECT_NE(std::find(capabilities.begin(), capabilities.end(), "files"), capabilities.end());
        EXPECT_EQ(std::find(capabilities.begin(), capabilities.end(), "files.upload"), capabilities.end());
        EXPECT_EQ(std::find(capabilities.begin(), capabilities.end(), "calls") != capabilities.end(),
                  tether::bluetooth::load_config().calls_enabled && tether::bluetooth::g_bluez &&
                      tether::bluetooth::g_bluez->running());
        EXPECT_EQ(std::find(capabilities.begin(), capabilities.end(), "clipboard") != capabilities.end(),
                  tether::g_wayland && tether::g_wayland->clipboard_available());
    }

    TEST(ClientTest, ConnectsToAnIpv6Literal) {
        const std::string home = unique_test_dir("tether_client_v6_test");
        CleanupGuard cleanup_guard(home);
        std::filesystem::remove_all(home);
        std::filesystem::create_directories(home);
        ScopedEnvVar home_env("HOME", home);
        ASSERT_TRUE(tether::Crypto::instance().init());

        constexpr int port = 45136;
        tether::EpollEventLoop loop;
        tether::TcpServer server(loop, port);
        if (!server.start())
            GTEST_SKIP() << "port " << port << " is unavailable";

        std::thread loop_thread([&loop] { loop.run(); });

        tether::Client client;
        const bool connected = client.connect("::1", port);
        client.disconnect();

        tether::Client bogus;
        EXPECT_FALSE(bogus.connect("not-an-address", port));

        loop.post([&loop] { loop.stop(); });
        loop_thread.join();

        if (!connected)
            GTEST_SKIP() << "no IPv6 loopback on this host";
    }

    size_t open_fd_count() {
        size_t count = 0;
        for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd"))
            (void)entry, ++count;
        return count;
    }

    // A dial that never lands must not strand a socket or a client-table entry;
    // every teardown path funnels through drop_client().
    TEST(TcpServerTest, DialToAClosedPortLeaksNothing) {
        const std::string home = unique_test_dir("tether_dial_fail_test");
        CleanupGuard cleanup_guard(home);
        std::filesystem::remove_all(home);
        std::filesystem::create_directories(home);
        ScopedEnvVar home_env("HOME", home);
        ASSERT_TRUE(tether::Crypto::instance().init());

        constexpr int port = 45137;
        constexpr int dead_port = 45138;
        tether::EpollEventLoop loop;
        tether::TcpServer server(loop, port);
        if (!server.start())
            GTEST_SKIP() << "port " << port << " is unavailable";

        CerrCapture captured;
        std::thread loop_thread([&loop] { loop.run(); });

        const size_t before = open_fd_count();
        EXPECT_TRUE(server.connect_peer("127.0.0.1", dead_port, "ghost"));

        std::string log;
        for (int attempt = 0; attempt < 200; ++attempt) {
            log = captured.str();
            if (log.find("Failed to dial") != std::string::npos)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        loop.post([&loop] { loop.stop(); });
        loop_thread.join();
        captured.restore();

        EXPECT_NE(log.find("Failed to dial"), std::string::npos) << "captured log:\n" << log;
        EXPECT_LE(open_fd_count(), before);
    }

    // A dial that lands nowhere must still reach the UI. Without a verdict the
    // GTK pair spinner runs forever, which is what a firewalled peer looks like.
    TEST(TcpServerTest, DialFailureReportsAReason) {
        const std::string home = unique_test_dir("tether_dial_reason_test");
        CleanupGuard cleanup_guard(home);
        std::filesystem::remove_all(home);
        std::filesystem::create_directories(home);
        ScopedEnvVar home_env("HOME", home);
        ASSERT_TRUE(tether::Crypto::instance().init());

        constexpr int port = 45139;
        constexpr int dead_port = 45140;
        tether::EpollEventLoop loop;
        tether::TcpServer server(loop, port);
        if (!server.start())
            GTEST_SKIP() << "port " << port << " is unavailable";

        int pair[2];
        ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
        tether::register_local_subscriber(pair[1]);

        std::thread loop_thread([&loop] { loop.run(); });
        EXPECT_TRUE(server.connect_peer("127.0.0.1", dead_port, "ghost"));

        // Nothing listens on dead_port, so the kernel answers RST: "refused",
        // which is the case a firewall is NOT responsible for.
        std::string event;
        char buf[4096];
        timeval read_timeout{10, 0};
        setsockopt(pair[0], SOL_SOCKET, SO_RCVTIMEO, &read_timeout, sizeof(read_timeout));
        const ssize_t n = read(pair[0], buf, sizeof(buf));
        if (n > 0)
            event.assign(buf, static_cast<size_t>(n));

        loop.post([&loop] { loop.stop(); });
        loop_thread.join();
        tether::unregister_local_subscriber(pair[1]);
        close(pair[0]);
        close(pair[1]);

        ASSERT_FALSE(event.empty()) << "no pair_rejected reached local subscribers";
        const auto parsed = nlohmann::json::parse(event);
        EXPECT_EQ(parsed.value("command", ""), "pair_rejected");
        EXPECT_EQ(parsed.value("reason", ""), "refused");
        EXPECT_EQ(parsed.value("address", ""), "127.0.0.1");
    }

    // A firewall that DROPs the SYN gives no answer at all. connect() must give
    // up on Tether's schedule, not the kernel's multi-minute retry budget.
    TEST(TcpServerTest, DialToABlackHoleGivesUpQuickly) {
        const std::string home = unique_test_dir("tether_dial_blackhole_test");
        CleanupGuard cleanup_guard(home);
        std::filesystem::remove_all(home);
        std::filesystem::create_directories(home);
        ScopedEnvVar home_env("HOME", home);
        ASSERT_TRUE(tether::Crypto::instance().init());

        constexpr int port = 45141;
        tether::EpollEventLoop loop;
        tether::TcpServer server(loop, port);
        if (!server.start())
            GTEST_SKIP() << "port " << port << " is unavailable";

        int pair[2];
        ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
        tether::register_local_subscriber(pair[1]);

        std::thread loop_thread([&loop] { loop.run(); });
        const auto started = std::chrono::steady_clock::now();
        // Reserved for documentation (RFC 5737); routers drop it rather than reply.
        EXPECT_TRUE(server.connect_peer("192.0.2.1", 5134, "black hole"));

        std::string event;
        char buf[4096];
        timeval read_timeout{60, 0};
        setsockopt(pair[0], SOL_SOCKET, SO_RCVTIMEO, &read_timeout, sizeof(read_timeout));
        const ssize_t n = read(pair[0], buf, sizeof(buf));
        if (n > 0)
            event.assign(buf, static_cast<size_t>(n));
        const auto elapsed = std::chrono::steady_clock::now() - started;

        loop.post([&loop] { loop.stop(); });
        loop_thread.join();
        tether::unregister_local_subscriber(pair[1]);
        close(pair[0]);
        close(pair[1]);

        ASSERT_FALSE(event.empty()) << "no verdict reached local subscribers";
        EXPECT_EQ(nlohmann::json::parse(event).value("command", ""), "pair_rejected");
        // Upper bound only: a network that answers immediately is still a pass.
        EXPECT_LT(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(), 30);
    }

    // The dialling side owns the pair request: it sends one unprompted once TLS is up,
    // which is what makes a desktop-initiated pair reach the peer's approval prompt.
    // The peer here is a bare TLS socket, not a second TcpServer, so nothing forks a
    // real approval dialog onto the machine running the suite.
    TEST(TcpServerTest, DiallingPeerSendsPairRequest) {
        const std::string home = unique_test_dir("tether_dial_pair_test");
        CleanupGuard cleanup_guard(home);
        std::filesystem::remove_all(home);
        std::filesystem::create_directories(home);
        ScopedEnvVar home_env("HOME", home);
        ASSERT_TRUE(tether::Crypto::instance().init());

        const int peer_fd = socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_GE(peer_fd, 0);
        int reuse = 1;
        setsockopt(peer_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in peer_addr{};
        peer_addr.sin_family = AF_INET;
        peer_addr.sin_port = 0; // let the kernel pick
        peer_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ASSERT_EQ(bind(peer_fd, reinterpret_cast<sockaddr*>(&peer_addr), sizeof(peer_addr)), 0);
        ASSERT_EQ(listen(peer_fd, 1), 0);

        socklen_t addrlen = sizeof(peer_addr);
        ASSERT_EQ(getsockname(peer_fd, reinterpret_cast<sockaddr*>(&peer_addr), &addrlen), 0);
        const int peer_port = ntohs(peer_addr.sin_port);

        std::string received;
        std::thread peer_thread([peer_fd, &received] {
            const int conn = accept(peer_fd, nullptr, nullptr);
            if (conn < 0)
                return;

            SSL* ssl = SSL_new(tether::Crypto::instance().get_server_context());
            SSL_set_fd(ssl, conn);
            if (SSL_accept(ssl) == 1) {
                char buf[4096];
                const int n = SSL_read(ssl, buf, sizeof(buf));
                if (n > 0)
                    received.assign(buf, n);
            }
            SSL_free(ssl);
            close(conn);
        });

        constexpr int dialler_port = 45139;
        tether::EpollEventLoop loop;
        tether::TcpServer dialler(loop, dialler_port);
        if (!dialler.start()) {
            close(peer_fd);
            peer_thread.join();
            GTEST_SKIP() << "port " << dialler_port << " is unavailable";
        }

        std::thread loop_thread([&loop] { loop.run(); });
        EXPECT_TRUE(dialler.connect_peer("127.0.0.1", peer_port, "peer"));

        peer_thread.join();
        loop.post([&loop] { loop.stop(); });
        loop_thread.join();
        close(peer_fd);

        ASSERT_FALSE(received.empty()) << "the dialler sent nothing after the handshake";
        const auto parsed = nlohmann::json::parse(received);
        EXPECT_EQ(parsed.value("command", ""), "pair_request");
        EXPECT_FALSE(parsed.value("device_name", "").empty());
    }

} // namespace

TEST(FirewallTest, UfwEnabledReadsTheEnabledKey) {
    EXPECT_TRUE(tether::ufw_enabled("ENABLED=yes\n"));
    EXPECT_FALSE(tether::ufw_enabled("ENABLED=no\n"));
}

TEST(FirewallTest, UfwEnabledIgnoresCommentsAndWhitespace) {
    EXPECT_FALSE(tether::ufw_enabled("# ENABLED=yes\n"));
    EXPECT_TRUE(tether::ufw_enabled("  ENABLED = yes  \n"));
    EXPECT_TRUE(tether::ufw_enabled("ENABLED=YES\r\n"));
}

TEST(FirewallTest, UfwEnabledIgnoresOtherKeysAndTakesTheFirstMatch) {
    EXPECT_FALSE(tether::ufw_enabled("LOGLEVEL=low\nIPV6=yes\n"));
    EXPECT_TRUE(tether::ufw_enabled("LOGLEVEL=low\nENABLED=yes\nIPV6=no\n"));
    EXPECT_FALSE(tether::ufw_enabled("ENABLED=no\nENABLED=yes\n"));
}

TEST(FirewallTest, UfwEnabledIsFalseWhenTheKeyIsAbsent) {
    EXPECT_FALSE(tether::ufw_enabled(""));
    EXPECT_FALSE(tether::ufw_enabled("ENABLED\n"));
    EXPECT_FALSE(tether::ufw_enabled("ENABLEDX=yes\n"));
}

// Both nodes browse and both listen, so an unconditional dial gives each side two promoted
// sessions to the same peer and every broadcast goes out twice.
TEST(ShouldDialPeer, OnlyTheLowerFingerprintDials) {
    EXPECT_TRUE(tether::should_dial_peer("aaa", "bbb", true));
    EXPECT_FALSE(tether::should_dial_peer("bbb", "aaa", true));
}

TEST(ShouldDialPeer, IsReciprocal) {
    const char* fps[] = {"00", "0a", "a0", "aa", "ff", "deadbeef", "0123456789abcdef"};
    for (const char* a : fps) {
        for (const char* b : fps) {
            if (std::string(a) == b)
                continue;
            EXPECT_NE(tether::should_dial_peer(a, b, true), tether::should_dial_peer(b, a, true))
                << a << " vs " << b;
        }
    }
}

TEST(ShouldDialPeer, SkipsUnknownIncompleteAndSelf) {
    EXPECT_FALSE(tether::should_dial_peer("aaa", "bbb", false)); // unpaired peer must still prompt
    EXPECT_FALSE(tether::should_dial_peer("", "bbb", true));
    EXPECT_FALSE(tether::should_dial_peer("aaa", "", true)); // peer published no fp= TXT record
    EXPECT_FALSE(tether::should_dial_peer("aaa", "aaa", true));
}

// A pairing request whose peer never came back used to sit in the device list
// forever, and every run of a test suite added another.
TEST(PendingPairs, DropsRequestsPastTheTtl) {
    const int64_t now = 1'700'000'000;
    nlohmann::json raw;
    raw["fresh"] = {{"name", "iPhone"}, {"ts", now - 60}};
    raw["stale"] = {{"name", "test phone"}, {"ts", now - 7200}};

    const auto live = tether::prune_pending_pairs(raw, now);
    EXPECT_TRUE(live.contains("fresh"));
    EXPECT_FALSE(live.contains("stale"));
    EXPECT_EQ(live["fresh"]["name"], "iPhone");
}

TEST(PendingPairs, AgesTheLegacyBareNameFormatFromNow) {
    const int64_t now = 1'700'000'000;
    nlohmann::json raw;
    raw["old"] = "iPhone";

    const auto live = tether::prune_pending_pairs(raw, now);
    ASSERT_TRUE(live.contains("old"));
    EXPECT_EQ(live["old"]["name"], "iPhone");
    EXPECT_EQ(live["old"]["ts"], now);
    // Migrated, not immortal.
    EXPECT_TRUE(tether::prune_pending_pairs(live, now + 7200).empty());
}

TEST(PendingPairs, IgnoresJunkEntries) {
    nlohmann::json raw;
    raw["no_name"] = {{"ts", 1}};
    raw["wrong_type"] = 42;
    EXPECT_TRUE(tether::prune_pending_pairs(raw, 1'700'000'000).empty());
    EXPECT_TRUE(tether::prune_pending_pairs(nlohmann::json::array(), 1).empty());
}
