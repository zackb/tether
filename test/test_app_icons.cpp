#include "scoped_env.hpp"

#include <fstream>
#include <gtest/gtest.h>
#include <tether/app_icons.hpp>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace tether::app_icons;
namespace fs = std::filesystem;

TEST(AppIcons, AcceptsBundleIds) {
    EXPECT_TRUE(valid_app_id("com.burbn.instagram"));
    EXPECT_TRUE(valid_app_id("net.whatsapp.WhatsApp"));
    EXPECT_TRUE(valid_app_id("com.apple.mobilemail"));
    EXPECT_TRUE(valid_app_id("com.some-app.ios_client"));
}

TEST(AppIcons, RejectsWhatCouldEscapeAPathOrAQuery) {
    EXPECT_FALSE(valid_app_id(""));
    EXPECT_FALSE(valid_app_id(".."));
    EXPECT_FALSE(valid_app_id("../etc/passwd"));
    EXPECT_FALSE(valid_app_id("com..x"));
    EXPECT_FALSE(valid_app_id(".hidden"));
    EXPECT_FALSE(valid_app_id("com.x/y"));
    EXPECT_FALSE(valid_app_id("com.x&country=us"));
    EXPECT_FALSE(valid_app_id("com.x y"));
    EXPECT_FALSE(valid_app_id(std::string(256, 'a')));
}

TEST(AppIcons, LookupUrl) {
    EXPECT_EQ(lookup_url("com.burbn.instagram", "es"),
              "https://itunes.apple.com/lookup?bundleId=com.burbn.instagram&country=es");
    EXPECT_EQ(lookup_url("com.burbn.instagram", ""), "https://itunes.apple.com/lookup?bundleId=com.burbn.instagram");
}

TEST(AppIcons, StoreCountryComesFromTheLocaleTerritory) {
    EXPECT_EQ(store_country("es_ES.UTF-8"), "es");
    EXPECT_EQ(store_country("en_GB"), "gb");
    EXPECT_EQ(store_country("pt_BR.utf8@euro"), "br");
    EXPECT_EQ(store_country("C"), "");
    EXPECT_EQ(store_country("POSIX"), "");
    EXPECT_EQ(store_country("C.UTF-8"), "");
    EXPECT_EQ(store_country("en"), "");
    EXPECT_EQ(store_country("en_"), "");
    EXPECT_EQ(store_country("en_1"), "");
}

TEST(AppIcons, ArtworkUrlAsksForASmallPng) {
    const std::string body = R"({"resultCount":1,"results":[{"trackName":"Instagram",
        "artworkUrl60":"https://is1-ssl.mzstatic.com/image/thumb/Purple221/v4/e3/33/x/Prod.png/60x60bb.jpg",
        "artworkUrl512":"https://is1-ssl.mzstatic.com/image/thumb/Purple221/v4/e3/33/x/Prod.png/512x512bb.jpg"}]})";
    EXPECT_EQ(artwork_url(body),
              "https://is1-ssl.mzstatic.com/image/thumb/Purple221/v4/e3/33/x/Prod.png/128x128bb.png");
}

TEST(AppIcons, ArtworkUrlFallsBackToSmallerRenditions) {
    const std::string body = R"({"results":[{"artworkUrl100":"https://is2-ssl.mzstatic.com/a/b/100x100bb.jpg"}]})";
    EXPECT_EQ(artwork_url(body), "https://is2-ssl.mzstatic.com/a/b/128x128bb.png");
}

TEST(AppIcons, ArtworkUrlIsEmptyForAnUnknownApp) {
    EXPECT_EQ(artwork_url(R"({"resultCount":0,"results":[]})"), "");
    EXPECT_EQ(artwork_url(R"({"results":[{"trackName":"No art"}]})"), "");
    EXPECT_EQ(artwork_url("not json"), "");
    EXPECT_EQ(artwork_url(""), "");
}

TEST(AppIcons, ArtworkOutsideApplesCdnIsNeverFetched) {
    EXPECT_EQ(artwork_url(R"({"results":[{"artworkUrl512":"https://example.com/a/512x512bb.jpg"}]})"), "");
    EXPECT_EQ(artwork_url(R"({"results":[{"artworkUrl512":"https://mzstatic.com.example.com/a/512x512bb.jpg"}]})"), "");
    EXPECT_EQ(artwork_url(R"({"results":[{"artworkUrl512":"http://is1-ssl.mzstatic.com/a/512x512bb.jpg"}]})"), "");
    EXPECT_EQ(artwork_url(R"({"results":[{"artworkUrl512":"https://user@is1-ssl.mzstatic.com/a/512x512bb.jpg"}]})"),
              "");
}

TEST(AppIcons, ParsesAnHttpResponse) {
    std::string body;
    EXPECT_EQ(parse_http_response("HTTP/1.0 200 OK\r\nContent-Length: 5\r\n\r\nhello", body), 200);
    EXPECT_EQ(body, "hello");
    EXPECT_EQ(parse_http_response("HTTP/1.1 404 Not Found\r\ncontent-length: 0\r\n\r\n", body), 404);
    EXPECT_EQ(body, "");
}

TEST(AppIcons, ResponseWithoutContentLengthReadsToTheEnd) {
    std::string body;
    EXPECT_EQ(parse_http_response("HTTP/1.0 200 OK\r\nContent-Type: image/png\r\n\r\nabc", body), 200);
    EXPECT_EQ(body, "abc");
}

TEST(AppIcons, TruncatedOrMalformedResponsesAreRejected) {
    std::string body;
    EXPECT_EQ(parse_http_response("HTTP/1.0 200 OK\r\nContent-Length: 10\r\n\r\nshort", body), 0);
    EXPECT_EQ(parse_http_response("HTTP/1.0 200 OK\r\nContent-Length: 2\r\n", body), 0);
    EXPECT_EQ(parse_http_response("garbage\r\n\r\n", body), 0);
    EXPECT_EQ(parse_http_response("HTTP/1.0 2x0 OK\r\n\r\n", body), 0);
    EXPECT_EQ(parse_http_response("", body), 0);
}

TEST(AppIcons, RecognizesPng) {
    EXPECT_TRUE(is_png(std::string("\x89PNG\r\n\x1a\n\x00\x00", 10)));
    EXPECT_FALSE(is_png("<html>"));
    EXPECT_FALSE(is_png(""));
}

TEST(AppIcons, CachedIconIsFoundOnlyWhenPresentAndNonEmpty) {
    const fs::path cache = fs::temp_directory_path() / ("tether-app-icons-" + std::to_string(::getpid()));
    tether::testing::ScopedEnv env("XDG_CACHE_HOME", cache.string());
    const fs::path dir = cache / "tether/app-icons";
    fs::create_directories(dir);

    EXPECT_EQ(cached_icon("com.example.app"), "");

    std::ofstream(dir / "com.example.empty.png").flush();
    EXPECT_EQ(cached_icon("com.example.empty"), "");

    std::ofstream(dir / "com.example.app.png") << "\x89PNG\r\n\x1a\nx";
    EXPECT_EQ(cached_icon("com.example.app"), (dir / "com.example.app.png").string());
    EXPECT_EQ(icon_path("com.example.app"), dir / "com.example.app.png");

    EXPECT_EQ(cached_icon("../com.example.app"), "");

    std::error_code ec;
    fs::remove_all(cache, ec);
}

namespace {

    std::vector<uint8_t> opaque_square(int size) {
        return std::vector<uint8_t>(static_cast<size_t>(size * size * 4), 255);
    }

    uint8_t alpha_at(const std::vector<uint8_t>& rgba, int size, int x, int y) {
        return rgba[static_cast<size_t>((y * size + x) * 4 + 3)];
    }

} // namespace

TEST(AppIconShape, CornersBecomeTransparentAndTheMiddleStaysOpaque) {
    constexpr int size = 128;
    auto rgba = opaque_square(size);
    apply_icon_shape(rgba.data(), size, size, size * 4);

    for (const auto& [x, y] : {std::pair{0, 0}, {size - 1, 0}, {0, size - 1}, {size - 1, size - 1}})
        EXPECT_EQ(alpha_at(rgba, size, x, y), 0) << x << "," << y;
    EXPECT_EQ(alpha_at(rgba, size, size / 2, size / 2), 255);
    // The middle of each edge touches the square.
    EXPECT_EQ(alpha_at(rgba, size, size / 2, 1), 255);
    EXPECT_EQ(alpha_at(rgba, size, 1, size / 2), 255);
}

TEST(AppIconShape, TheEdgeIsAntiAliased) {
    constexpr int size = 128;
    auto rgba = opaque_square(size);
    apply_icon_shape(rgba.data(), size, size, size * 4);

    // Alpha rises through partial values along the diagonal.
    bool partial = false;
    for (int i = 0; i < size / 2; ++i) {
        const uint8_t a = alpha_at(rgba, size, i, i);
        partial = partial || (a > 0 && a < 255);
    }
    EXPECT_TRUE(partial);
}

TEST(AppIconShape, ShapeIsSymmetric) {
    constexpr int size = 64;
    auto rgba = opaque_square(size);
    apply_icon_shape(rgba.data(), size, size, size * 4);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            EXPECT_EQ(alpha_at(rgba, size, x, y), alpha_at(rgba, size, size - 1 - x, y));
            EXPECT_EQ(alpha_at(rgba, size, x, y), alpha_at(rgba, size, y, x));
        }
}

TEST(AppIconShape, ColourIsLeftAloneAndExistingTransparencyKept) {
    constexpr int size = 32;
    std::vector<uint8_t> rgba(static_cast<size_t>(size * size * 4));
    for (size_t i = 0; i < rgba.size(); i += 4) {
        rgba[i] = 10;
        rgba[i + 1] = 20;
        rgba[i + 2] = 30;
        rgba[i + 3] = 0;
    }
    apply_icon_shape(rgba.data(), size, size, size * 4);
    for (size_t i = 0; i < rgba.size(); i += 4) {
        EXPECT_EQ(rgba[i], 10);
        EXPECT_EQ(rgba[i + 3], 0);
    }
}

TEST(AppIconShape, HonoursTheRowStride) {
    constexpr int size = 16, stride = size * 4 + 8; // padded rows
    std::vector<uint8_t> rgba(static_cast<size_t>(stride * size), 255);
    apply_icon_shape(rgba.data(), size, size, stride);
    EXPECT_EQ(rgba[3], 0);                                 // top-left pixel
    EXPECT_EQ(rgba[static_cast<size_t>(stride) - 1], 255); // padding untouched
    EXPECT_EQ(rgba[static_cast<size_t>((size / 2) * stride + (size / 2) * 4 + 3)], 255);
}

TEST(AppIconsFetching, NothingIsFetchedForACachedOrMissingIcon) {
    const fs::path cache = fs::temp_directory_path() / ("tether-app-icons-fetching-" + std::to_string(::getpid()));
    tether::testing::ScopedEnv env("XDG_CACHE_HOME", cache.string());
    const fs::path dir = cache / "tether/app-icons";
    fs::create_directories(dir);
    std::ofstream(dir / "com.example.cached.png") << "\x89PNG\r\n\x1a\nx";
    std::ofstream(dir / "com.example.unknown.none").flush();

    for (const char* app_id : {"com.example.cached", "com.example.unknown", "../not-an-id"}) {
        request(app_id);
        EXPECT_FALSE(fetching(app_id)) << app_id;
    }

    std::error_code ec;
    fs::remove_all(cache, ec);
}

TEST(AppIconsLookupOutcome, FindsArtworkInASuccessfulLookup) {
    const std::string body =
        R"({"resultCount":1,"results":[{"artworkUrl512":"https://is1-ssl.mzstatic.com/image/thumb/a/b/512x512bb.jpg"}]})";
    EXPECT_EQ(lookup_outcome(200, body), Lookup::Found);
}

TEST(AppIconsLookupOutcome, ReportsAnAppTheStoreDoesNotHave) {
    EXPECT_EQ(lookup_outcome(200, R"({"resultCount":0,"results":[]})"), Lookup::NotInStore);
}

TEST(AppIconsLookupOutcome, RetriesErrorsLater) {
    for (const int status : {429, 503, 0, 403})
        EXPECT_EQ(lookup_outcome(status, ""), Lookup::TryLater) << status;
}
