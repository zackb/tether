#include "tether/discovery.hpp"

#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-client/publish.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <avahi-common/simple-watch.h>
#include <avahi-common/thread-watch.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>

namespace tether {

    static constexpr const char* SERVICE_TYPE = "_tether._tcp";

    // ─── Impl ───────────────────────────────────────────────────────
    // Defined at file scope so C-style Avahi callbacks can access it.

    struct DiscoveryImpl {
        // Publishing state
        AvahiThreadedPoll* pub_poll = nullptr;
        AvahiClient* pub_client = nullptr;
        AvahiEntryGroup* pub_group = nullptr;
        std::string pub_name;
        uint16_t pub_port = 0;
        std::string pub_fingerprint;

        // Discovery state (per-call, not persistent)
        std::mutex results_mutex;
        std::vector<DiscoveredHost> results;
    };

    // Wire the pimpl to use DiscoveryImpl
    struct Discovery::Impl : DiscoveryImpl {};

    // ─── Publishing callbacks ───────────────────────────────────────

    static void entry_group_callback(AvahiEntryGroup* g, AvahiEntryGroupState state, void* userdata) {
        auto* impl = static_cast<DiscoveryImpl*>(userdata);
        switch (state) {
        case AVAHI_ENTRY_GROUP_ESTABLISHED:
            std::cout << "mDNS: Published \"" << SERVICE_TYPE << "\" as \"" << impl->pub_name
                      << "\" on port " << impl->pub_port << std::endl;
            break;
        case AVAHI_ENTRY_GROUP_COLLISION:
            std::cerr << "mDNS: Service name collision for \"" << impl->pub_name << "\"" << std::endl;
            break;
        case AVAHI_ENTRY_GROUP_FAILURE:
            std::cerr << "mDNS: Entry group failure: "
                      << avahi_strerror(avahi_client_errno(avahi_entry_group_get_client(g))) << std::endl;
            break;
        default:
            break;
        }
    }

    static void create_services(DiscoveryImpl* impl) {
        if (!impl->pub_client) return;

        if (!impl->pub_group) {
            impl->pub_group = avahi_entry_group_new(impl->pub_client, entry_group_callback, impl);
            if (!impl->pub_group) {
                std::cerr << "mDNS: avahi_entry_group_new() failed: "
                          << avahi_strerror(avahi_client_errno(impl->pub_client)) << std::endl;
                return;
            }
        }

        if (avahi_entry_group_is_empty(impl->pub_group)) {
            std::string txt_fp = "fp=" + impl->pub_fingerprint;
            std::string txt_ver = "v=1";

            int ret = avahi_entry_group_add_service(
                impl->pub_group,
                AVAHI_IF_UNSPEC,
                AVAHI_PROTO_UNSPEC,
                (AvahiPublishFlags)0,
                impl->pub_name.c_str(),
                SERVICE_TYPE,
                nullptr, // domain
                nullptr, // host
                impl->pub_port,
                txt_fp.c_str(),
                txt_ver.c_str(),
                nullptr  // sentinel
            );

            if (ret < 0) {
                std::cerr << "mDNS: Failed to add service: " << avahi_strerror(ret) << std::endl;
                return;
            }

            ret = avahi_entry_group_commit(impl->pub_group);
            if (ret < 0) {
                std::cerr << "mDNS: Failed to commit entry group: " << avahi_strerror(ret) << std::endl;
            }
        }
    }

    static void pub_client_callback(AvahiClient* c, AvahiClientState state, void* userdata) {
        auto* impl = static_cast<DiscoveryImpl*>(userdata);
        impl->pub_client = c;

        switch (state) {
        case AVAHI_CLIENT_S_RUNNING:
            create_services(impl);
            break;
        case AVAHI_CLIENT_FAILURE:
            std::cerr << "mDNS: Client failure: " << avahi_strerror(avahi_client_errno(c)) << std::endl;
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

    static void resolve_callback(
        AvahiServiceResolver* r,
        AvahiIfIndex /*interface*/,
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
            }
        }

        avahi_service_resolver_free(r);
    }

    static void browse_callback(
        AvahiServiceBrowser* /*b*/,
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
            avahi_service_resolver_new(
                ctx->client,
                interface,
                protocol,
                name,
                type,
                domain,
                AVAHI_PROTO_UNSPEC,
                (AvahiLookupFlags)0,
                resolve_callback,
                userdata);
        }
    }

    // ─── Discovery class ────────────────────────────────────────────

    Discovery::Discovery() : impl_(std::make_unique<Impl>()) {}

    Discovery::~Discovery() {
        unpublish();
    }

    Discovery::Discovery(Discovery&&) noexcept = default;
    Discovery& Discovery::operator=(Discovery&&) noexcept = default;

    bool Discovery::publish(const std::string& name, uint16_t port, const std::string& fingerprint) {
        unpublish();

        impl_->pub_name = name;
        impl_->pub_port = port;
        impl_->pub_fingerprint = fingerprint;

        impl_->pub_poll = avahi_threaded_poll_new();
        if (!impl_->pub_poll) {
            std::cerr << "mDNS: Failed to create threaded poll" << std::endl;
            return false;
        }

        int error = 0;
        impl_->pub_client = avahi_client_new(
            avahi_threaded_poll_get(impl_->pub_poll),
            (AvahiClientFlags)0,
            pub_client_callback,
            impl_.get(),
            &error);

        if (!impl_->pub_client) {
            std::cerr << "mDNS: Failed to create client: " << avahi_strerror(error) << std::endl;
            avahi_threaded_poll_free(impl_->pub_poll);
            impl_->pub_poll = nullptr;
            return false;
        }

        if (avahi_threaded_poll_start(impl_->pub_poll) < 0) {
            std::cerr << "mDNS: Failed to start threaded poll" << std::endl;
            avahi_client_free(impl_->pub_client);
            impl_->pub_client = nullptr;
            avahi_threaded_poll_free(impl_->pub_poll);
            impl_->pub_poll = nullptr;
            return false;
        }

        return true;
    }

    void Discovery::unpublish() {
        if (impl_->pub_poll) {
            avahi_threaded_poll_stop(impl_->pub_poll);
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
            std::cerr << "mDNS: Failed to create browse poll" << std::endl;
            return {};
        }

        // Clear previous results
        {
            std::lock_guard<std::mutex> lock(impl_->results_mutex);
            impl_->results.clear();
        }

        int error = 0;
        AvahiClient* client = avahi_client_new(
            avahi_threaded_poll_get(poll),
            (AvahiClientFlags)0,
            [](AvahiClient*, AvahiClientState, void*) {},  // no-op callback
            nullptr,
            &error);

        if (!client) {
            std::cerr << "mDNS: Failed to create browse client: " << avahi_strerror(error) << std::endl;
            avahi_threaded_poll_free(poll);
            return {};
        }

        BrowseContext ctx{impl_.get(), client};

        AvahiServiceBrowser* browser = avahi_service_browser_new(
            client,
            AVAHI_IF_UNSPEC,
            AVAHI_PROTO_UNSPEC,
            SERVICE_TYPE,
            nullptr, // domain (default .local)
            (AvahiLookupFlags)0,
            browse_callback,
            &ctx);

        if (!browser) {
            std::cerr << "mDNS: Failed to create browser: "
                      << avahi_strerror(avahi_client_errno(client)) << std::endl;
            avahi_client_free(client);
            avahi_threaded_poll_free(poll);
            return {};
        }

        // Run the browse loop for the requested duration
        avahi_threaded_poll_start(poll);
        std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
        avahi_threaded_poll_stop(poll);

        avahi_service_browser_free(browser);
        avahi_client_free(client);
        avahi_threaded_poll_free(poll);

        std::lock_guard<std::mutex> lock(impl_->results_mutex);
        return impl_->results;
    }

} // namespace tether

