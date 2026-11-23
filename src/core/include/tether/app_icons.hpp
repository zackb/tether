#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

// Opt-in real app icons for notifications. Only the bundle id and store country are
// sent to Apple's App Store lookup; the resulting icon is cached as a PNG.
namespace tether::app_icons {

    // Size in pixels of the cached icon.
    inline constexpr int ICON_SIZE = 128;

    // Whether `app_id` is a plain bundle id, safe as a file name and in a query string.
    bool valid_app_id(std::string_view app_id);

    // The App Store lookup URL for a bundle id; an empty `country` means the default store.
    std::string lookup_url(std::string_view app_id, std::string_view country);

    // The store country for a POSIX locale ("es_ES.UTF-8" gives "es"); empty if none.
    std::string store_country(std::string_view locale);

    // The ICON_SIZE PNG URL from a lookup response; empty if absent or not on Apple's image CDN.
    std::string artwork_url(std::string_view lookup_json);

    // What a lookup response means: an icon to fetch, an app the store doesn't have, or an error worth retrying.
    enum class Lookup { Found, NotInStore, TryLater };
    Lookup lookup_outcome(int status, std::string_view body);

    // Splits a complete HTTP/1.0 response into body and status; returns 0 if malformed or truncated.
    int parse_http_response(std::string_view raw, std::string& body);

    // Whether `data` starts with the PNG signature.
    bool is_png(std::string_view data);

    // Masks an icon to a rounded superellipse with anti-aliased edges; `rgba` is non-premultiplied 8-bit.
    void apply_icon_shape(uint8_t* rgba, int width, int height, int stride);

    // Optionally rewrites a downloaded PNG before caching (empty keeps it); call before the first request().
    using ImageFilter = std::function<std::string(const std::string& png)>;
    void set_image_filter(ImageFilter filter);

    // Where the icon for `app_id` is cached (it may not exist).
    std::filesystem::path icon_path(std::string_view app_id);

    // The cached icon's absolute path, or empty if none.
    std::string cached_icon(std::string_view app_id);

    // Fetches the icon in the background unless cached, in flight or recently missed; thread-safe, non-blocking.
    void request(const std::string& app_id);

    // Whether a fetch for `app_id` is queued or running.
    bool fetching(const std::string& app_id);

} // namespace tether::app_icons
