#include "tether/discovery.hpp"

#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-client/publish.h>
#include <avahi-common/alternative.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <avahi-common/simple-watch.h>
#include <avahi-common/thread-watch.h>
#include <avahi-common/timeval.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <tether/log.hpp>
#include <tether/net_trust.hpp>
#include <thread>

namespace tether {

    static constexpr const char* SERVICE_TYPE = "_tether._tcp";

    // ─── Impl ───────────────────────────────────────────────────────
    // Defined at file scope so C-style Avahi callbacks can access it.

    // How long to wait before rebuilding a client whose daemon went away.
    static constexpr unsigned RECONNECT_DELAY_MS = 1000;

    struct DiscoveryImpl {
        // Publishing state
        AvahiThreadedPoll* pub_poll = nullptr;
        AvahiClient* pub_client = nullptr;
        AvahiEntryGroup* pub_group = nullptr;
        AvahiTimeout* pub_reconnect = nullptr;
        std::string pub_name;
        uint16_t pub_port = 0;
        std::string pub_fingerprint;
        std::vector<AvahiIfIndex> pub_interfaces;

        // Discovery state (per-call, not persistent)
        std::mutex results_mutex;
        std::vector<DiscoveredHost> results;

        // Continuous browse state
        AvahiThreadedPoll* browse_poll = nullptr;
        AvahiClient* browse_client = nullptr;
        AvahiServiceBrowser* browse_browser = nullptr;
        AvahiTimeout* browse_reconnect = nullptr;
        std::function<void(const std::vector<DiscoveredDevice>&)> browse_callback;
        void* browse_ctx = nullptr;

        // Availability reporting
        std::mutex state_mutex;
        Discovery::StateCallback state_callback;
        bool state_known = false;
        bool state_available = false;
    };

    // Wire the pimpl to use DiscoveryImpl
    struct Discovery::Impl : DiscoveryImpl {};

    // Reports availability, collapsing repeats. The publish and browse
    // clients use the same daemon and the same states
    static void report_state(DiscoveryImpl* impl, bool available) {
        Discovery::StateCallback cb;
        {
            std::lock_guard<std::mutex> lock(impl->state_mutex);
            if (impl->state_known && impl->state_available == available)
                return;
            impl->state_known = true;
            impl->state_available = available;
            cb = impl->state_callback;
        }
        if (cb)
            cb(available);
    }

    // ─── Publishing callbacks ───────────────────────────────────────

    static void create_services(DiscoveryImpl* impl);

    // Take the next free variant of the advertised name ("foo" -> "foo #2" -> "foo #3").
    static void rename_on_collision(DiscoveryImpl* impl) {
        char* alt = avahi_alternative_service_name(impl->pub_name.c_str());
        debug::log(ERR, "mDNS: Service name collision for \"{}\", retrying as \"{}\"", impl->pub_name, alt);
        impl->pub_name = alt;
        avahi_free(alt);
    }

    static void entry_group_callback(AvahiEntryGroup* g, AvahiEntryGroupState state, void* userdata) {
        auto* impl = static_cast<DiscoveryImpl*>(userdata);
        switch (state) {
        case AVAHI_ENTRY_GROUP_ESTABLISHED:
            debug::log(
                INFO, "mDNS: Published \"{}\" as \"{}\" on port {}", SERVICE_TYPE, impl->pub_name, impl->pub_port);
            break;
        case AVAHI_ENTRY_GROUP_COLLISION:
            rename_on_collision(impl);
            avahi_entry_group_reset(g);
            create_services(impl);
            break;
        case AVAHI_ENTRY_GROUP_FAILURE:
            debug::log(ERR,
                       "mDNS: Entry group failure: {}",
                       avahi_strerror(avahi_client_errno(avahi_entry_group_get_client(g))));
            break;
        default:
            break;
        }
    }

    // Every interface when all are trusted, else each trusted one once.
    static std::vector<AvahiIfIndex> publish_interfaces() {
        if (lan_trust_all())
            return {AVAHI_IF_UNSPEC};
        std::vector<AvahiIfIndex> out;
        for (const auto& net : lan_networks())
            if (net.trusted && std::find(out.begin(), out.end(), net.ifindex) == out.end())
                out.push_back(net.ifindex);
        return out;
    }

    static void create_services(DiscoveryImpl* impl) {
        if (!impl->pub_client)
            return;

        if (!impl->pub_group) {
            impl->pub_group = avahi_entry_group_new(impl->pub_client, entry_group_callback, impl);
            if (!impl->pub_group) {
                debug::log(ERR,
                           "mDNS: avahi_entry_group_new() failed: {}",
                           avahi_strerror(avahi_client_errno(impl->pub_client)));
                return;
            }
        }

        if (!avahi_entry_group_is_empty(impl->pub_group))
            return;

        // one record per trusted interface
        const std::vector<AvahiIfIndex> interfaces = publish_interfaces();
        impl->pub_interfaces = interfaces;
        if (interfaces.empty()) {
            debug::log(INFO, "mDNS: No trusted network; not advertising");
            return;
        }

        std::string txt_fp = "fp=" + impl->pub_fingerprint;
        std::string txt_ver = "v=1";

        int ret = 0;
        int added = 0;
        for (int attempt = 0; attempt < 16; ++attempt) {
            added = 0;
            for (AvahiIfIndex ifindex : interfaces) {
                ret = avahi_entry_group_add_service(impl->pub_group,
                                                    ifindex,
                                                    AVAHI_PROTO_UNSPEC,
                                                    (AvahiPublishFlags)0,
                                                    impl->pub_name.c_str(),
                                                    SERVICE_TYPE,
                                                    nullptr, // domain
                                                    nullptr, // host
                                                    impl->pub_port,
                                                    txt_fp.c_str(),
                                                    txt_ver.c_str(),
                                                    nullptr // sentinel
                );
                if (ret == AVAHI_ERR_COLLISION)
                    break;
                // an interface that vanished since refresh does not keep the others dark
                if (ret < 0)
                    debug::log(ERR, "mDNS: Failed to add service on {}: {}", ifindex, avahi_strerror(ret));
                else
                    ++added;
            }

            if (ret != AVAHI_ERR_COLLISION)
                break;

            rename_on_collision(impl);
            avahi_entry_group_reset(impl->pub_group);
        }

        if (added == 0)
            return;

        ret = avahi_entry_group_commit(impl->pub_group);
        if (ret < 0) {
            debug::log(ERR, "mDNS: Failed to commit entry group: {}", avahi_strerror(ret));
        }
    }

    static void pub_client_callback(AvahiClient* c, AvahiClientState state, void* userdata);

    // Rebuilds the publishing client after avahi-daemon went away.
    static void pub_reconnect_callback(AvahiTimeout* t, void* userdata) {
        auto* impl = static_cast<DiscoveryImpl*>(userdata);

        // The group belongs to the dead client and dies with it.
        impl->pub_group = nullptr;
        if (impl->pub_client) {
            avahi_client_free(impl->pub_client);
            impl->pub_client = nullptr;
        }

        int error = 0;
        impl->pub_client = avahi_client_new(
            avahi_threaded_poll_get(impl->pub_poll), AVAHI_CLIENT_NO_FAIL, pub_client_callback, impl, &error);
        if (!impl->pub_client) {
            debug::log(ERR, "mDNS: Failed to recreate client: {}", avahi_strerror(error));
            struct timeval tv;
            avahi_elapse_time(&tv, RECONNECT_DELAY_MS, 0);
            avahi_threaded_poll_get(impl->pub_poll)->timeout_update(t, &tv);
        }
    }

    static void pub_client_callback(AvahiClient* c, AvahiClientState state, void* userdata) {
        auto* impl = static_cast<DiscoveryImpl*>(userdata);
        impl->pub_client = c;

        switch (state) {
        case AVAHI_CLIENT_S_RUNNING:
            create_services(impl);
            report_state(impl, true);
            break;
        case AVAHI_CLIENT_CONNECTING:
            debug::log(ERR, "mDNS: avahi-daemon is not running; waiting for it to start");
            report_state(impl, false);
            break;
        case AVAHI_CLIENT_FAILURE:
            debug::log(ERR, "mDNS: Client failure: {}", avahi_strerror(avahi_client_errno(c)));
            report_state(impl, false);
            if (avahi_client_errno(c) == AVAHI_ERR_DISCONNECTED && impl->pub_reconnect) {
                struct timeval tv;
                avahi_elapse_time(&tv, RECONNECT_DELAY_MS, 0);
                avahi_threaded_poll_get(impl->pub_poll)->timeout_update(impl->pub_reconnect, &tv);
            }
            break;
        case AVAHI_CLIENT_S_COLLISION:
        case AVAHI_CLIENT_S_REGISTERING:
            if (impl->pub_group) {
                avahi_entry_group_reset(impl->pub_group);
            }
            break;
        default:
            break;
        }
    }

    // ─── Discovery callbacks ────────────────────────────────────────

    struct BrowseContext {
        DiscoveryImpl* impl;
        AvahiClient* client;
    };

    static void resolve_callback(AvahiServiceResolver* r,
                                 AvahiIfIndex interface,
                                 AvahiProtocol /*protocol*/,
                                 AvahiResolverEvent event,
                                 const char* name,
                                 const char* /*type*/,
                                 const char* /*domain*/,
                                 const char* /*host_name*/,
                                 const AvahiAddress* address,
                                 uint16_t port,
                                 AvahiStringList* txt,
                                 AvahiLookupResultFlags /*flags*/,
                                 void* userdata) {

        auto* ctx = static_cast<BrowseContext*>(userdata);

        if (event == AVAHI_RESOLVER_FOUND && address) {
            char addr_str[AVAHI_ADDRESS_STR_MAX];
            avahi_address_snprint(addr_str, sizeof(addr_str), address);

            DiscoveredHost host;
            host.name = name ? name : "";
            host.address = addr_str;
            host.port = port;
            host.ifindex = interface;

            // Extract fingerprint from TXT records
            for (AvahiStringList* item = txt; item; item = avahi_string_list_get_next(item)) {
                char* key = nullptr;
                char* value = nullptr;
                if (avahi_string_list_get_pair(item, &key, &value, nullptr) == 0) {
                    if (key && value && std::strcmp(key, "fp") == 0) {
                        host.fingerprint = value;
                    }
                    avahi_free(key);
                    avahi_free(value);
                }
            }

            std::function<void(const std::vector<DiscoveredDevice>&)> cb;
            std::vector<DiscoveredDevice> grouped;
            {
                std::lock_guard<std::mutex> lock(ctx->impl->results_mutex);
                // Deduplicate by address+port
                bool exists = false;
                for (const auto& h : ctx->impl->results) {
                    if (h.address == host.address && h.port == host.port) {
                        exists = true;
                        break;
                    }
                }
                if (!exists) {
                    ctx->impl->results.push_back(std::move(host));
                }

                if (ctx->impl->browse_callback) {
                    cb = ctx->impl->browse_callback;
                    grouped = group_discovered_hosts(ctx->impl->results, ctx->impl->pub_fingerprint);
                }
            }

            if (cb)
                cb(grouped);
        }

        avahi_service_resolver_free(r);
    }

    static void browse_callback(AvahiServiceBrowser* /*b*/,
                                AvahiIfIndex interface,
                                AvahiProtocol protocol,
                                AvahiBrowserEvent event,
                                const char* name,
                                const char* type,
                                const char* domain,
                                AvahiLookupResultFlags /*flags*/,
                                void* userdata) {

        auto* ctx = static_cast<BrowseContext*>(userdata);

        if (event == AVAHI_BROWSER_NEW) {
            avahi_service_resolver_new(ctx->client,
                                       interface,
                                       protocol,
                                       name,
                                       type,
                                       domain,
                                       AVAHI_PROTO_UNSPEC,
                                       (AvahiLookupFlags)0,
                                       resolve_callback,
                                       userdata);
        } else if (event == AVAHI_BROWSER_REMOVE) {
            std::function<void(const std::vector<DiscoveredDevice>&)> cb;
            std::vector<DiscoveredDevice> grouped;
            {
                std::lock_guard<std::mutex> lock(ctx->impl->results_mutex);
                ctx->impl->results.erase(std::remove_if(ctx->impl->results.begin(),
                                                        ctx->impl->results.end(),
                                                        [&](const DiscoveredHost& h) { return h.name == name; }),
                                         ctx->impl->results.end());

                if (ctx->impl->browse_callback) {
                    cb = ctx->impl->browse_callback;
                    grouped = group_discovered_hosts(ctx->impl->results, ctx->impl->pub_fingerprint);
                }
            }
            if (cb)
                cb(grouped);
        }
    }

    // ─── Continuous browse callbacks ────────────────────────────────

    static void browse_client_callback(AvahiClient* c, AvahiClientState state, void* userdata);

    // Counterpart of pub_reconnect_callback for the browsing client.
    static void browse_reconnect_callback(AvahiTimeout* t, void* userdata) {
        auto* impl = static_cast<DiscoveryImpl*>(userdata);

        if (impl->browse_client) {
            avahi_client_free(impl->browse_client);
            impl->browse_client = nullptr;
        }

        int error = 0;
        impl->browse_client = avahi_client_new(
            avahi_threaded_poll_get(impl->browse_poll), AVAHI_CLIENT_NO_FAIL, browse_client_callback, impl, &error);
        if (!impl->browse_client) {
            debug::log(ERR, "mDNS: Failed to recreate browse client: {}", avahi_strerror(error));
            struct timeval tv;
            avahi_elapse_time(&tv, RECONNECT_DELAY_MS, 0);
            avahi_threaded_poll_get(impl->browse_poll)->timeout_update(t, &tv);
        }
    }

    static void browse_client_callback(AvahiClient* c, AvahiClientState state, void* userdata) {
        auto* impl = static_cast<DiscoveryImpl*>(userdata);
        auto* ctx = static_cast<BrowseContext*>(impl->browse_ctx);
        impl->browse_client = c;
        if (ctx)
            ctx->client = c;

        switch (state) {
        case AVAHI_CLIENT_S_RUNNING:
            // The browser can only be created once the daemon is connected, so
            // it is built here rather than alongside the client.
            if (!impl->browse_browser && ctx) {
                impl->browse_browser = avahi_service_browser_new(c,
                                                                 AVAHI_IF_UNSPEC,
                                                                 AVAHI_PROTO_UNSPEC,
                                                                 SERVICE_TYPE,
                                                                 nullptr, // domain (default .local)
                                                                 (AvahiLookupFlags)0,
                                                                 browse_callback,
                                                                 ctx);
                if (!impl->browse_browser) {
                    debug::log(ERR, "mDNS: Failed to create browser: {}", avahi_strerror(avahi_client_errno(c)));
                    break;
                }
            }
            report_state(impl, true);
            break;
        case AVAHI_CLIENT_CONNECTING:
            report_state(impl, false);
            break;
        case AVAHI_CLIENT_FAILURE:
            debug::log(ERR, "mDNS: Browse client failure: {}", avahi_strerror(avahi_client_errno(c)));
            report_state(impl, false);
            // The browser belongs to the dead client; a new one is built when
            // the replacement client reaches S_RUNNING.
            impl->browse_browser = nullptr;
            {
                std::lock_guard<std::mutex> lock(impl->results_mutex);
                impl->results.clear();
            }
            if (avahi_client_errno(c) == AVAHI_ERR_DISCONNECTED && impl->browse_reconnect) {
                struct timeval tv;
                avahi_elapse_time(&tv, RECONNECT_DELAY_MS, 0);
                avahi_threaded_poll_get(impl->browse_poll)->timeout_update(impl->browse_reconnect, &tv);
            }
            break;
        default:
            break;
        }
    }

    // Per-call state for the one-shot discover(). The browser can only be built
    // once the daemon is connected, so the callback owns it.
    struct OneShotBrowse {
        BrowseContext ctx;
        AvahiServiceBrowser* browser = nullptr;
    };

    static void discover_client_callback(AvahiClient* c, AvahiClientState state, void* userdata) {
        auto* one = static_cast<OneShotBrowse*>(userdata);
        one->ctx.client = c;

        if (state == AVAHI_CLIENT_S_RUNNING && !one->browser) {
            one->browser = avahi_service_browser_new(c,
                                                     AVAHI_IF_UNSPEC,
                                                     AVAHI_PROTO_UNSPEC,
                                                     SERVICE_TYPE,
                                                     nullptr, // domain (default .local)
                                                     (AvahiLookupFlags)0,
                                                     browse_callback,
                                                     &one->ctx);
            if (!one->browser)
                debug::log(ERR, "mDNS: Failed to create browser: {}", avahi_strerror(avahi_client_errno(c)));
        } else if (state == AVAHI_CLIENT_FAILURE) {
            debug::log(ERR, "mDNS: Browse client failure: {}", avahi_strerror(avahi_client_errno(c)));
            one->browser = nullptr;
        }
    }

    // ─── Discovery class ────────────────────────────────────────────

    Discovery::Discovery() : impl_(std::make_unique<Impl>()) {}

    Discovery::~Discovery() {
        // Both, not just unpublish(): a running continuous browse keeps its own poll
        // thread alive and its callback fires into freed state during shutdown.
        stop_continuous_browse();
        unpublish();
    }

    Discovery::Discovery(Discovery&&) noexcept = default;
    Discovery& Discovery::operator=(Discovery&&) noexcept = default;

    void Discovery::set_state_callback(StateCallback cb) {
        std::lock_guard<std::mutex> lock(impl_->state_mutex);
        impl_->state_callback = std::move(cb);
    }

    bool Discovery::publish(const std::string& name, uint16_t port, const std::string& fingerprint) {
        unpublish();

        impl_->pub_name = name;
        impl_->pub_port = port;
        impl_->pub_fingerprint = fingerprint;

        impl_->pub_poll = avahi_threaded_poll_new();
        if (!impl_->pub_poll) {
            debug::log(ERR, "mDNS: Failed to create threaded poll");
            return false;
        }

        // Created disabled; armed only when the daemon disconnects. Allocating
        // it up front keeps the reconnect path free of allocation.
        const AvahiPoll* poll_api = avahi_threaded_poll_get(impl_->pub_poll);
        impl_->pub_reconnect = poll_api->timeout_new(poll_api, nullptr, pub_reconnect_callback, impl_.get());

        // NO_FAIL: register whenever avahi-daemon appears rather than only if
        // it happens to be up right now.
        int error = 0;
        impl_->pub_client = avahi_client_new(poll_api, AVAHI_CLIENT_NO_FAIL, pub_client_callback, impl_.get(), &error);

        if (!impl_->pub_client) {
            // NO_FAIL covers an absent avahi-daemon, so reaching here means the
            // bus itself is unreachable: nothing to wait for.
            debug::log(ERR, "mDNS: Failed to create client: {}", avahi_strerror(error));
            unpublish();
            report_state(impl_.get(), false);
            return false;
        }

        if (avahi_threaded_poll_start(impl_->pub_poll) < 0) {
            debug::log(ERR, "mDNS: Failed to start threaded poll");
            unpublish();
            report_state(impl_.get(), false);
            return false;
        }

        return true;
    }

    void Discovery::republish() {
        if (!impl_->pub_poll)
            return;
        avahi_threaded_poll_lock(impl_->pub_poll);
        if (publish_interfaces() == impl_->pub_interfaces) {
            avahi_threaded_poll_unlock(impl_->pub_poll);
            return;
        }
        if (impl_->pub_group)
            avahi_entry_group_reset(impl_->pub_group);
        if (impl_->pub_client && avahi_client_get_state(impl_->pub_client) == AVAHI_CLIENT_S_RUNNING)
            create_services(impl_.get());
        avahi_threaded_poll_unlock(impl_->pub_poll);
    }

    void Discovery::unpublish() {
        if (impl_->pub_poll) {
            avahi_threaded_poll_stop(impl_->pub_poll);
        }
        if (impl_->pub_reconnect) {
            avahi_threaded_poll_get(impl_->pub_poll)->timeout_free(impl_->pub_reconnect);
            impl_->pub_reconnect = nullptr;
        }
        if (impl_->pub_group) {
            avahi_entry_group_free(impl_->pub_group);
            impl_->pub_group = nullptr;
        }
        if (impl_->pub_client) {
            avahi_client_free(impl_->pub_client);
            impl_->pub_client = nullptr;
        }
        if (impl_->pub_poll) {
            avahi_threaded_poll_free(impl_->pub_poll);
            impl_->pub_poll = nullptr;
        }
    }

    std::vector<DiscoveredHost> Discovery::discover(int timeout_ms) {
        AvahiThreadedPoll* poll = avahi_threaded_poll_new();
        if (!poll) {
            debug::log(ERR, "mDNS: Failed to create browse poll");
            return {};
        }

        // Clear previous results
        {
            std::lock_guard<std::mutex> lock(impl_->results_mutex);
            impl_->results.clear();
        }

        OneShotBrowse one{{impl_.get(), nullptr}, nullptr};

        int error = 0;
        AvahiClient* client = avahi_client_new(
            avahi_threaded_poll_get(poll), AVAHI_CLIENT_NO_FAIL, discover_client_callback, &one, &error);

        if (!client) {
            debug::log(ERR, "mDNS: Failed to create browse client: {}", avahi_strerror(error));
            avahi_threaded_poll_free(poll);
            return {};
        }

        // Run the browse loop for the requested duration. With NO_FAIL the scan
        // still covers a daemon that only becomes available partway through.
        avahi_threaded_poll_start(poll);
        std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
        avahi_threaded_poll_stop(poll);

        if (one.browser)
            avahi_service_browser_free(one.browser);
        avahi_client_free(client);
        avahi_threaded_poll_free(poll);

        std::lock_guard<std::mutex> lock(impl_->results_mutex);
        return impl_->results;
    }

    void Discovery::start_continuous_browse(std::function<void(const std::vector<DiscoveredDevice>&)> callback) {
        stop_continuous_browse();

        {
            std::lock_guard<std::mutex> lock(impl_->results_mutex);
            impl_->results.clear();
        }

        impl_->browse_callback = std::move(callback);

        impl_->browse_poll = avahi_threaded_poll_new();
        if (!impl_->browse_poll) {
            debug::log(ERR, "mDNS: Failed to create browse poll");
            return;
        }

        impl_->browse_ctx = new BrowseContext{impl_.get(), nullptr};

        const AvahiPoll* poll_api = avahi_threaded_poll_get(impl_->browse_poll);
        impl_->browse_reconnect = poll_api->timeout_new(poll_api, nullptr, browse_reconnect_callback, impl_.get());

        int error = 0;
        impl_->browse_client =
            avahi_client_new(poll_api, AVAHI_CLIENT_NO_FAIL, browse_client_callback, impl_.get(), &error);

        if (!impl_->browse_client) {
            debug::log(ERR, "mDNS: Failed to create browse client: {}", avahi_strerror(error));
            stop_continuous_browse();
            report_state(impl_.get(), false);
            return;
        }

        avahi_threaded_poll_start(impl_->browse_poll);
    }

    void Discovery::refresh() {
        std::function<void(const std::vector<DiscoveredDevice>&)> cb;
        std::vector<DiscoveredDevice> grouped;
        {
            std::lock_guard<std::mutex> lock(impl_->results_mutex);
            if (!impl_->browse_callback)
                return;
            cb = impl_->browse_callback;
            grouped = group_discovered_hosts(impl_->results, impl_->pub_fingerprint);
        }
        cb(grouped);
    }

    void Discovery::stop_continuous_browse() {
        if (!impl_->browse_poll)
            return;

        avahi_threaded_poll_stop(impl_->browse_poll);

        impl_->browse_callback = nullptr;

        if (impl_->browse_reconnect) {
            avahi_threaded_poll_get(impl_->browse_poll)->timeout_free(impl_->browse_reconnect);
            impl_->browse_reconnect = nullptr;
        }

        if (impl_->browse_browser)
            avahi_service_browser_free(impl_->browse_browser);
        if (impl_->browse_client)
            avahi_client_free(impl_->browse_client);
        avahi_threaded_poll_free(impl_->browse_poll);

        if (impl_->browse_ctx) {
            delete static_cast<BrowseContext*>(impl_->browse_ctx);
            impl_->browse_ctx = nullptr;
        }

        impl_->browse_browser = nullptr;
        impl_->browse_client = nullptr;
        impl_->browse_poll = nullptr;
    }

    // ─── Grouping helper ────────────────────────────────────────────

    std::vector<DiscoveredDevice> group_discovered_hosts(const std::vector<DiscoveredHost>& hosts,
                                                         const std::string& exclude_fingerprint) {
        // Use fingerprint as the grouping key.  Maintain insertion order.
        std::vector<DiscoveredDevice> devices;
        std::map<std::string, size_t> fp_index; // fingerprint -> index into devices

        for (const auto& h : hosts) {
            if (!exclude_fingerprint.empty() && h.fingerprint == exclude_fingerprint)
                continue;

            std::string key = h.fingerprint.empty() ? ("__nokey__" + h.name) : h.fingerprint;

            auto it = fp_index.find(key);
            if (it == fp_index.end()) {
                fp_index[key] = devices.size();
                DiscoveredDevice dev;
                dev.name = h.name;
                dev.fingerprint = h.fingerprint;
                dev.addresses.push_back({h.address, h.port, h.ifindex});
                devices.push_back(std::move(dev));
            } else {
                devices[it->second].addresses.push_back({h.address, h.port, h.ifindex});
            }
        }

        // Sort addresses within each device: IPv4 first (no ':'), then IPv6
        for (auto& dev : devices) {
            std::stable_sort(
                dev.addresses.begin(), dev.addresses.end(), [](const DiscoveredAddress& a, const DiscoveredAddress& b) {
                    bool a_v4 = a.address.find(':') == std::string::npos;
                    bool b_v4 = b.address.find(':') == std::string::npos;
                    return a_v4 > b_v4; // true (v4) sorts before false (v6)
                });
        }

        return devices;
    }

} // namespace tether
