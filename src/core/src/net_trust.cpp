#include "tether/net_trust.hpp"

#include "tether/bluetooth/config.hpp"
#include "tether/event_loop.hpp"
#include "tether/log.hpp"

#include <arpa/inet.h>
#include <cstring>
#include <gio/gio.h>
#include <ifaddrs.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <mutex>
#include <net/if.h>
#include <netinet/in.h>
#include <set>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

namespace tether {

    void apply_lan_trust(std::vector<LanNetwork>& networks,
                         bool trust_all,
                         const std::map<std::string, std::string>& trusted) {
        for (auto& net : networks)
            net.trusted = trust_all || trusted.contains(net.key);
    }

    std::string ifname_for_local_addr(const sockaddr* addr, const ifaddrs* head) {
        if (!addr)
            return {};

        int family = addr->sa_family;
        const void* want = nullptr;
        in_addr mapped{};
        if (family == AF_INET) {
            want = &reinterpret_cast<const sockaddr_in*>(addr)->sin_addr;
        } else if (family == AF_INET6) {
            const auto& a6 = reinterpret_cast<const sockaddr_in6*>(addr)->sin6_addr;
            if (IN6_IS_ADDR_V4MAPPED(&a6)) {
                std::memcpy(&mapped, &a6.s6_addr[12], sizeof(mapped));
                family = AF_INET;
                want = &mapped;
            } else {
                want = &a6;
            }
        } else {
            return {};
        }

        for (const ifaddrs* ifa = head; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != family || !ifa->ifa_name)
                continue;
            const bool same =
                family == AF_INET
                    ? std::memcmp(&reinterpret_cast<const sockaddr_in*>(ifa->ifa_addr)->sin_addr, want, 4) == 0
                    : std::memcmp(&reinterpret_cast<const sockaddr_in6*>(ifa->ifa_addr)->sin6_addr, want, 16) == 0;
            if (same)
                return ifa->ifa_name;
        }
        return {};
    }

    void apply_wifi_networks(std::vector<LanNetwork>& networks, const std::map<std::string, WifiNetwork>& wifi) {
        static constexpr char HEX[] = "0123456789abcdef";
        for (auto& net : networks) {
            const auto it = wifi.find(net.ifname);
            if (it == wifi.end())
                continue;
            std::string hex;
            for (unsigned char c : it->second.ssid) {
                hex += HEX[c >> 4];
                hex += HEX[c & 0xf];
            }
            net.key = "iwd:" + hex + ":" + it->second.security;
            net.name = it->second.ssid;
        }
    }

    namespace {

        constexpr const char* NM_NAME = "org.freedesktop.NetworkManager";
        // Calls block the daemon loop, so a hung NetworkManager costs at most this per call.
        constexpr int NM_TIMEOUT_MS = 250;
        // NMActivationStateFlags: the connection was set up outside NetworkManager.
        constexpr guint32 NM_ACTIVATION_STATE_FLAG_EXTERNAL = 0x80;
        // Netlink sends bursts while a link comes up; one refresh after it settles.
        constexpr int SETTLE_SECONDS = 1;

        struct TrustState {
            std::mutex mutex;
            std::vector<LanNetwork> networks;
            bool trust_all = false;
            std::function<void()> changed;
        };

        TrustState& state() {
            static TrustState s;
            return s;
        }

        GVariant* nm_property(GDBusConnection* bus, const char* path, const char* iface, const char* prop) {
            GError* error = nullptr;
            GVariant* reply = g_dbus_connection_call_sync(bus,
                                                          NM_NAME,
                                                          path,
                                                          "org.freedesktop.DBus.Properties",
                                                          "Get",
                                                          g_variant_new("(ss)", iface, prop),
                                                          G_VARIANT_TYPE("(v)"),
                                                          G_DBUS_CALL_FLAGS_NO_AUTO_START,
                                                          NM_TIMEOUT_MS,
                                                          nullptr,
                                                          &error);
            if (!reply) {
                g_clear_error(&error);
                return nullptr;
            }
            GVariant* value = nullptr;
            g_variant_get(reply, "(v)", &value);
            g_variant_unref(reply);
            return value;
        }

        std::string nm_string(GDBusConnection* bus, const char* path, const char* iface, const char* prop) {
            GVariant* v = nm_property(bus, path, iface, prop);
            if (!v)
                return {};
            std::string out = g_variant_is_of_type(v, G_VARIANT_TYPE_STRING) ? g_variant_get_string(v, nullptr) : "";
            g_variant_unref(v);
            return out;
        }

        std::vector<std::string> nm_paths(GDBusConnection* bus, const char* path, const char* iface, const char* prop) {
            std::vector<std::string> out;
            GVariant* v = nm_property(bus, path, iface, prop);
            if (!v)
                return out;
            if (g_variant_is_of_type(v, G_VARIANT_TYPE_OBJECT_PATH_ARRAY)) {
                GVariantIter iter;
                g_variant_iter_init(&iter, v);
                const char* p = nullptr;
                while (g_variant_iter_next(&iter, "&o", &p))
                    out.emplace_back(p);
            }
            g_variant_unref(v);
            return out;
        }

        // False when NetworkManager is not running.
        // Cached once found; a bus missing at startup is looked for again next refresh.
        GDBusConnection* system_bus() {
            static GDBusConnection* bus = nullptr;
            if (!bus)
                bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, nullptr);
            return bus;
        }

        bool nm_networks(std::vector<LanNetwork>& out) {
            GDBusConnection* bus = system_bus();
            if (!bus)
                return false;

            GVariant* probe = nm_property(bus, "/org/freedesktop/NetworkManager", NM_NAME, "ActiveConnections");
            if (!probe)
                return false;
            g_variant_unref(probe);

            const char* active_iface = "org.freedesktop.NetworkManager.Connection.Active";
            const char* device_iface = "org.freedesktop.NetworkManager.Device";
            for (const auto& ac : nm_paths(bus, "/org/freedesktop/NetworkManager", NM_NAME, "ActiveConnections")) {
                const std::string uuid = nm_string(bus, ac.c_str(), active_iface, "Uuid");
                const std::string id = nm_string(bus, ac.c_str(), active_iface, "Id");
                if (uuid.empty())
                    continue;
                // NetworkManager makes up a fresh UUID each boot for an interface it only
                // watches (tailscale0, docker0), so those are keyed by name instead.
                bool external = false;
                if (GVariant* flags = nm_property(bus, ac.c_str(), active_iface, "StateFlags")) {
                    external = g_variant_is_of_type(flags, G_VARIANT_TYPE_UINT32) &&
                               (g_variant_get_uint32(flags) & NM_ACTIVATION_STATE_FLAG_EXTERNAL);
                    g_variant_unref(flags);
                }
                for (const auto& dev : nm_paths(bus, ac.c_str(), active_iface, "Devices")) {
                    std::string ifname = nm_string(bus, dev.c_str(), device_iface, "IpInterface");
                    if (ifname.empty())
                        ifname = nm_string(bus, dev.c_str(), device_iface, "Interface");
                    const int ifindex = ifname.empty() ? 0 : static_cast<int>(if_nametoindex(ifname.c_str()));
                    if (ifindex == 0 || ifname == "lo")
                        continue;
                    out.push_back({external ? "if:" + ifname : uuid, id.empty() ? ifname : id, ifname, ifindex, false});
                }
            }
            return true;
        }

        // Without NetworkManager a network is its interface; iwd then names the Wi-Fi ones.
        // ponytail: wpa_supplicant/connman Wi-Fi stays one network per card; add connman if asked.
        std::vector<LanNetwork> interface_networks() {
            std::vector<LanNetwork> out;
            ifaddrs* head = nullptr;
            if (getifaddrs(&head) != 0)
                return out;
            std::set<std::string> seen;
            for (const ifaddrs* ifa = head; ifa; ifa = ifa->ifa_next) {
                if (!ifa->ifa_addr || !ifa->ifa_name || !(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK))
                    continue;
                const int family = ifa->ifa_addr->sa_family;
                if ((family != AF_INET && family != AF_INET6) || !seen.insert(ifa->ifa_name).second)
                    continue;
                out.push_back({std::string("if:") + ifa->ifa_name,
                               ifa->ifa_name,
                               ifa->ifa_name,
                               static_cast<int>(if_nametoindex(ifa->ifa_name)),
                               false});
            }
            freeifaddrs(head);
            return out;
        }

        constexpr const char* IWD_NAME = "net.connman.iwd";

        // Interface name -> the Wi-Fi network iwd has it connected to. Empty without iwd.
        std::map<std::string, WifiNetwork> iwd_wifi_networks() {
            std::map<std::string, WifiNetwork> out;
            GDBusConnection* bus = system_bus();
            if (!bus)
                return out;
            GError* error = nullptr;
            GVariant* reply = g_dbus_connection_call_sync(bus,
                                                          IWD_NAME,
                                                          "/",
                                                          "org.freedesktop.DBus.ObjectManager",
                                                          "GetManagedObjects",
                                                          nullptr,
                                                          G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
                                                          G_DBUS_CALL_FLAGS_NO_AUTO_START,
                                                          NM_TIMEOUT_MS,
                                                          nullptr,
                                                          &error);
            if (!reply) {
                g_clear_error(&error);
                return out;
            }

            // Devices name their interface; connected Networks point at their Device.
            std::map<std::string, std::string> device_ifname;
            std::vector<std::pair<std::string, WifiNetwork>> connected; // device path, network
            GVariant* objects = g_variant_get_child_value(reply, 0);
            GVariantIter object_iter;
            g_variant_iter_init(&object_iter, objects);
            const char* path = nullptr;
            GVariant* interfaces = nullptr;
            while (g_variant_iter_next(&object_iter, "{&o@a{sa{sv}}}", &path, &interfaces)) {
                if (GVariant* dev = g_variant_lookup_value(interfaces, "net.connman.iwd.Device", nullptr)) {
                    const char* name = nullptr;
                    if (g_variant_lookup(dev, "Name", "&s", &name))
                        device_ifname[path] = name;
                    g_variant_unref(dev);
                }
                if (GVariant* net = g_variant_lookup_value(interfaces, "net.connman.iwd.Network", nullptr)) {
                    gboolean is_connected = FALSE;
                    const char* ssid = nullptr;
                    const char* type = nullptr;
                    const char* device = nullptr;
                    if (g_variant_lookup(net, "Connected", "b", &is_connected) && is_connected &&
                        g_variant_lookup(net, "Name", "&s", &ssid) && g_variant_lookup(net, "Device", "&o", &device)) {
                        if (!g_variant_lookup(net, "Type", "&s", &type))
                            type = "";
                        connected.push_back({device, {ssid, type}});
                    }
                    g_variant_unref(net);
                }
                g_variant_unref(interfaces);
            }
            g_variant_unref(objects);
            g_variant_unref(reply);

            for (const auto& [device, net] : connected)
                if (const auto it = device_ifname.find(device); it != device_ifname.end())
                    out[it->second] = net;
            return out;
        }

        bool is_loopback(const sockaddr* addr) {
            if (addr->sa_family == AF_INET)
                return (ntohl(reinterpret_cast<const sockaddr_in*>(addr)->sin_addr.s_addr) >> 24) == 127;
            if (addr->sa_family == AF_INET6) {
                const auto& a6 = reinterpret_cast<const sockaddr_in6*>(addr)->sin6_addr;
                return IN6_IS_ADDR_LOOPBACK(&a6) || (IN6_IS_ADDR_V4MAPPED(&a6) && a6.s6_addr[12] == 127);
            }
            return false;
        }

        void drain(int fd) {
            char buf[4096];
            while (read(fd, buf, sizeof(buf)) > 0) {
            }
        }

    } // namespace

    void lan_trust_refresh() {
        std::vector<LanNetwork> networks;
        if (!nm_networks(networks)) {
            networks = interface_networks();
            apply_wifi_networks(networks, iwd_wifi_networks());
        }

        const auto config = bluetooth::load_config();
        const bool trust_all = config.lan_scope == bluetooth::LanScope::All;
        apply_lan_trust(networks, trust_all, config.trusted_networks);

        std::function<void()> changed;
        {
            auto& s = state();
            std::lock_guard<std::mutex> lock(s.mutex);
            if (s.networks == networks && s.trust_all == trust_all)
                return;
            s.networks = std::move(networks);
            s.trust_all = trust_all;
            for (const auto& net : s.networks)
                debug::log(INFO,
                           "Network {} ({}) on {}: {}",
                           net.name,
                           net.key,
                           net.ifname,
                           net.trusted ? "trusted" : "hidden");
            changed = s.changed;
        }
        if (changed)
            changed();
    }

    void lan_trust_start(EpollEventLoop& loop) {
        lan_trust_refresh();

        const int settle = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        const int nl = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_ROUTE);
        sockaddr_nl sa{};
        sa.nl_family = AF_NETLINK;
        sa.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR;
        if (settle < 0 || nl < 0 || bind(nl, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) < 0) {
            debug::log(ERR, "Network watch unavailable: {}", std::strerror(errno));
            if (settle >= 0)
                close(settle);
            if (nl >= 0)
                close(nl);
            return;
        }

        loop.addFd(nl, [settle](int fd) {
            drain(fd);
            itimerspec spec{};
            spec.it_value.tv_sec = SETTLE_SECONDS;
            timerfd_settime(settle, 0, &spec, nullptr);
        });
        loop.addFd(settle, [](int fd) {
            drain(fd);
            lan_trust_refresh();
        });
    }

    void set_lan_trust_changed_handler(std::function<void()> handler) {
        auto& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.changed = std::move(handler);
    }

    std::vector<LanNetwork> lan_networks() {
        auto& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        return s.networks;
    }

    bool lan_trust_all() {
        auto& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        return s.trust_all;
    }

    bool lan_ifindex_trusted(int ifindex) {
        auto& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.trust_all)
            return true;
        for (const auto& net : s.networks)
            if (net.ifindex == ifindex && net.trusted)
                return true;
        return false;
    }

    bool lan_ifname_trusted(const std::string& ifname) {
        auto& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.trust_all || ifname == "lo")
            return true;
        for (const auto& net : s.networks)
            if (net.ifname == ifname && net.trusted)
                return true;
        return false;
    }

    bool lan_local_addr_trusted(const sockaddr* addr) {
        if (!addr)
            return false;
        if (is_loopback(addr) || lan_trust_all())
            return true;
        ifaddrs* head = nullptr;
        if (getifaddrs(&head) != 0)
            return false;
        const std::string ifname = ifname_for_local_addr(addr, head);
        freeifaddrs(head);
        return !ifname.empty() && lan_ifname_trusted(ifname);
    }

} // namespace tether
