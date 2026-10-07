#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <tether/base64.hpp>
#include <tether/clipboard.hpp>
#include <tether/net.hpp>
#include <tether/wayland.hpp>

#include <fstream>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

TEST(ClipboardMimeTest, TextWinsOverImage) {
    EXPECT_EQ(tether::pick_clipboard_mime({"image/png", "text/plain"}), "text/plain");
    EXPECT_EQ(tether::pick_clipboard_mime({"text/html", "image/png"}), "image/png");
    EXPECT_EQ(tether::pick_clipboard_mime({"UTF8_STRING"}), "UTF8_STRING");
    EXPECT_EQ(tether::pick_clipboard_mime({"text/html", "image/jpeg"}), "");
}

TEST(ClipboardImageTest, ChunksReassembleToTheOriginal) {
    std::string png(1300 * 1024, '\0');
    for (size_t i = 0; i < png.size(); ++i)
        png[i] = static_cast<char>(i * 31);

    auto msgs = tether::clipboard_image_messages(png, "content");
    ASSERT_EQ(msgs.size(), 5u); // start, 3 chunks, end

    auto start = nlohmann::json::parse(msgs.front());
    EXPECT_EQ(start["command"], "file_start");
    EXPECT_EQ(start["clipboard"], "content");
    EXPECT_EQ(start["size"], png.size());
    EXPECT_EQ(nlohmann::json::parse(msgs.back())["command"], "file_end");

    std::string joined;
    for (size_t i = 1; i + 1 < msgs.size(); ++i) {
        auto chunk = nlohmann::json::parse(msgs[i]);
        EXPECT_EQ(chunk["chunk_index"], i - 1);
        auto bytes = tether::base64_decode(chunk["data"].get<std::string>());
        joined.append(bytes.begin(), bytes.end());
    }
    EXPECT_EQ(joined, png);
}

TEST(ClipboardImageTest, EmptyOrOversizedSendsNothing) {
    EXPECT_TRUE(tether::clipboard_image_messages("", "updated").empty());
    EXPECT_TRUE(tether::clipboard_image_messages(std::string(33u * 1024 * 1024, 'x'), "updated").empty());
}

TEST(ClipboardImageTest, ReceiverRoundTripsSentImage) {
    std::string png = std::string("\x89PNG\r\n\x1a\n", 8) + std::string(700 * 1024, 'p');
    tether::ClipboardImageReceiver rx;
    std::string id;
    for (const auto& m : tether::clipboard_image_messages(png, "updated")) {
        auto j = nlohmann::json::parse(m);
        if (j["command"] == "file_start") {
            id = j["transfer_id"];
            ASSERT_TRUE(rx.start(id, j["size"]));
        } else if (j["command"] == "file_chunk") {
            ASSERT_TRUE(rx.chunk(id, j["data"]));
        }
    }
    EXPECT_EQ(rx.finish(id), png);
    EXPECT_EQ(rx.finish(id), ""); // consumed
}

TEST(ClipboardImageTest, ReceiverDropsBadTransfers) {
    const std::string png("\x89PNG\r\n\x1a\nxx", 10);
    const std::string b64 = tether::base64_encode(reinterpret_cast<const unsigned char*>(png.data()), png.size());
    tether::ClipboardImageReceiver rx;

    EXPECT_FALSE(rx.start("big", 33u * 1024 * 1024));
    EXPECT_FALSE(rx.chunk("big", b64)); // not ours, falls through to file transfer

    ASSERT_TRUE(rx.start("short", png.size() + 1));
    ASSERT_TRUE(rx.chunk("short", b64));
    EXPECT_EQ(rx.finish("short"), "");

    ASSERT_TRUE(rx.start("notpng", 3));
    ASSERT_TRUE(rx.chunk("notpng", tether::base64_encode(reinterpret_cast<const unsigned char*>("abc"), 3)));
    EXPECT_EQ(rx.finish("notpng"), "");

    ASSERT_TRUE(rx.start("over", 4));
    EXPECT_TRUE(rx.chunk("over", b64));
    EXPECT_EQ(rx.finish("over"), "");
}

TEST(WaylandSocketTest, OnlyCompositorSocketsAreCandidates) {
    char dir[] = "/tmp/tether-wl-XXXXXX";
    ASSERT_NE(mkdtemp(dir), nullptr);
    const std::filesystem::path root = dir;
    std::ofstream(root / "wayland-0");      // stale regular file
    std::ofstream(root / "wayland-1.lock"); // compositor lock file

    const auto bind_socket = [&](const std::string& name) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, (root / name).c_str(), sizeof(addr.sun_path) - 1);
        EXPECT_EQ(bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
        return fd;
    };
    int fd2 = bind_socket("wayland-2");
    int fd1 = bind_socket("wayland-1");
    int other = bind_socket("pipewire-0");

    EXPECT_EQ(tether::wayland_sockets(root), (std::vector<std::string>{"wayland-1", "wayland-2"}));
    EXPECT_TRUE(tether::wayland_sockets(root / "missing").empty());

    close(fd1);
    close(fd2);
    close(other);
    std::filesystem::remove_all(root);
}
