#pragma once

#include <gio/gio.h>
#include <memory>
#include <string>

namespace tether::bluetooth {

    struct AdvertState;

    // Broadcasts an LE advertisement soliciting the ANCS service.
    class AncsAdvertisement {
    public:
        AncsAdvertisement(GDBusConnection* connection, std::string adapter_path);
        ~AncsAdvertisement();

        AncsAdvertisement(const AncsAdvertisement&) = delete;
        AncsAdvertisement& operator=(const AncsAdvertisement&) = delete;

        // Registration is two-phase, and the phases must run on different threads.
        bool export_object();
        bool register_with_bluez();
        void unregister_with_bluez();
        void unexport_object();

        bool active() const;

    private:
        std::unique_ptr<AdvertState> state_;
    };

} // namespace tether::bluetooth
