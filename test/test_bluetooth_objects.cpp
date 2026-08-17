#include <gtest/gtest.h>
#include <tether/bluetooth/objects.hpp>

using namespace tether::bluetooth;

namespace {

    // Builds an ObjectManager payload from GVariant text format, the same shape
    // BlueZ returns from GetManagedObjects. Keeps the tests free of a real bus.
    struct Payload {
        GVariant* v = nullptr;

        explicit Payload(const char* text) {
            GError* error = nullptr;
            v = g_variant_parse(G_VARIANT_TYPE("a{oa{sa{sv}}}"), text, nullptr, nullptr, &error);
            if (!v) {
                ADD_FAILURE() << "bad fixture: " << (error ? error->message : "unknown");
                g_clear_error(&error);
            }
        }
        ~Payload() {
            if (v)
                g_variant_unref(v);
        }
        Payload(const Payload&) = delete;
        Payload& operator=(const Payload&) = delete;
    };

    // A capable adapter: powered, both LE roles, advertising, A/V Hands-Free class.
    constexpr const char* ADAPTER_FULL = R"({
      '/org/bluez/hci0': {
        'org.bluez.Adapter1': {
          'Address': <'AC:F2:3C:AF:52:9C'>,
          'Alias': <'btw'>,
          'Class': <uint32 8127496>,
          'Powered': <true>,
          'Roles': <['central', 'peripheral']>
        },
        'org.bluez.LEAdvertisingManager1': {
          'SupportedInstances': <byte 15>
        }
      }
    })";

    // An iPhone bonded on both transports, exposing MAP, PBAP and ANCS.
    constexpr const char* IPHONE_BONDED = R"({
      '/org/bluez/hci0/dev_60_57_C8_30_6A_F7': {
        'org.bluez.Device1': {
          'Address': <'60:57:C8:30:6A:F7'>,
          'Alias': <'Zack 15 Pro'>,
          'Adapter': <objectpath '/org/bluez/hci0'>,
          'Paired': <true>,
          'Bonded': <true>,
          'Trusted': <true>,
          'Connected': <true>,
          'UUIDs': <['00001132-0000-1000-8000-00805f9b34fb',
                     '00001133-0000-1000-8000-00805f9b34fb',
                     '0000112f-0000-1000-8000-00805f9b34fb',
                     '7905f431-b5ce-4e99-a40f-4b1e122d00d0']>
        },
        'org.bluez.Bearer.LE1': {
          'Paired': <true>, 'Bonded': <true>, 'Connected': <true>
        }
      }
    })";

    std::string join(const char* a, const char* b) {
        std::string left(a), right(b);
        // Splice two single-entry dictionaries into one.
        left.erase(left.find_last_of('}'));
        right = right.substr(right.find('{') + 1);
        return left + "," + right;
    }

} // namespace

TEST(BluetoothObjects, ParsesAdapterProperties) {
    Payload p(ADAPTER_FULL);
    auto objects = parse_managed_objects(p.v);

    ASSERT_EQ(objects.adapters.size(), 1u);
    const auto& a = objects.adapters[0];
    EXPECT_EQ(a.path, "/org/bluez/hci0");
    EXPECT_EQ(a.address, "AC:F2:3C:AF:52:9C");
    EXPECT_EQ(a.name, "btw");
    EXPECT_TRUE(a.powered);
    EXPECT_TRUE(a.has_role("central"));
    EXPECT_TRUE(a.has_role("peripheral"));
    EXPECT_FALSE(a.has_role("broadcaster"));
    EXPECT_EQ(a.advertising_instances, 15);
    // 8127496 == 0x7c0408: service bits set above an A/V Hands-Free base class.
    EXPECT_TRUE(a.class_is_handsfree());
}

TEST(BluetoothObjects, ParsesDeviceAndBearer) {
    Payload p(IPHONE_BONDED);
    auto objects = parse_managed_objects(p.v);

    ASSERT_EQ(objects.devices.size(), 1u);
    const auto& d = objects.devices[0];
    EXPECT_EQ(d.address, "60:57:C8:30:6A:F7");
    EXPECT_EQ(d.name, "Zack 15 Pro");
    EXPECT_EQ(d.adapter_path, "/org/bluez/hci0");
    EXPECT_TRUE(d.bonded);
    EXPECT_TRUE(d.connected);
    EXPECT_TRUE(d.has_le_bearer);
    EXPECT_TRUE(d.le_connected);
    EXPECT_TRUE(d.supports_map());
    EXPECT_TRUE(d.supports_pbap());
    EXPECT_TRUE(d.supports_ancs());
    EXPECT_TRUE(d.looks_like_iphone());
}

TEST(BluetoothObjects, UuidMatchIsCaseInsensitive) {
    Payload p(R"({
      '/org/bluez/hci0/dev_AA': {
        'org.bluez.Device1': {
          'UUIDs': <['7905F431-B5CE-4E99-A40F-4B1E122D00D0']>
        }
      }
    })");
    auto objects = parse_managed_objects(p.v);
    ASSERT_EQ(objects.devices.size(), 1u);
    EXPECT_TRUE(objects.devices[0].supports_ancs());
}

TEST(BluetoothObjects, DerivesAdapterPathWhenPropertyMissing) {
    Payload p(R"({
      '/org/bluez/hci1/dev_AA_BB': {
        'org.bluez.Device1': { 'Address': <'AA:BB:CC:DD:EE:FF'> }
      }
    })");
    auto objects = parse_managed_objects(p.v);
    ASSERT_EQ(objects.devices.size(), 1u);
    EXPECT_EQ(objects.devices[0].adapter_path, "/org/bluez/hci1");
}

// Older BlueZ has no Bonded property; a paired device must still read as bonded
// or capability resolution would treat the bearer API as merely unconfirmed.
TEST(BluetoothObjects, BondedFallsBackToPaired) {
    Payload p(R"({
      '/org/bluez/hci0/dev_AA': {
        'org.bluez.Device1': { 'Paired': <true> }
      }
    })");
    auto objects = parse_managed_objects(p.v);
    ASSERT_EQ(objects.devices.size(), 1u);
    EXPECT_TRUE(objects.devices[0].bonded);
}

TEST(BluetoothObjects, IgnoresUnexpectedPropertyTypes) {
    // A string where a boolean belongs must not throw or corrupt the record.
    Payload p(R"({
      '/org/bluez/hci0': {
        'org.bluez.Adapter1': { 'Powered': <'yes'>, 'Class': <'nope'>, 'Address': <uint32 7> }
      }
    })");
    auto objects = parse_managed_objects(p.v);
    ASSERT_EQ(objects.adapters.size(), 1u);
    EXPECT_FALSE(objects.adapters[0].powered);
    EXPECT_EQ(objects.adapters[0].device_class, 0u);
    EXPECT_TRUE(objects.adapters[0].address.empty());
}

TEST(BluetoothObjects, RejectsNullAndWrongShape) {
    EXPECT_TRUE(parse_managed_objects(nullptr).adapters.empty());

    GVariant* wrong = g_variant_new_string("not an object tree");
    g_variant_ref_sink(wrong);
    auto objects = parse_managed_objects(wrong);
    EXPECT_TRUE(objects.adapters.empty());
    EXPECT_TRUE(objects.devices.empty());
    g_variant_unref(wrong);
}

TEST(Capability, FullModeWhenBearerConfirmed) {
    Payload p(join(ADAPTER_FULL, IPHONE_BONDED).c_str());
    auto cap = resolve_capability(parse_managed_objects(p.v));

    EXPECT_EQ(cap.mode, DeliveryMode::Full);
    EXPECT_EQ(cap.bearer_api, BearerApi::Confirmed);
    EXPECT_TRUE(cap.le_central);
    EXPECT_TRUE(cap.le_peripheral);
    EXPECT_TRUE(cap.advertising);
    EXPECT_TRUE(cap.class_ok);
    EXPECT_TRUE(cap.reasons.empty());
}

// Without --experimental the bearer interface can never appear, so its absence
// is conclusive.
TEST(Capability, CompatibilityWhenExperimentalIsOff) {
    Payload p(join(ADAPTER_FULL, R"({
      '/org/bluez/hci0/dev_60_57_C8_30_6A_F7': {
        'org.bluez.Device1': { 'Paired': <true>, 'Bonded': <true> }
      }
    })")
                  .c_str());
    auto objects = parse_managed_objects(p.v);
    objects.experimental_api = false;
    auto cap = resolve_capability(objects);

    EXPECT_EQ(cap.mode, DeliveryMode::Compatibility);
    EXPECT_EQ(cap.bearer_api, BearerApi::Absent);
    EXPECT_FALSE(cap.reasons.empty());
}

// Headsets and game controllers are bonded but never expose a bearer under any
// build, so treating any bond as proof of absence gives a false negative and
// silently disables ANCS on a machine that supports it.
TEST(Capability, ClassicOnlyBondDoesNotDisproveBearerApi) {
    Payload p(join(ADAPTER_FULL, R"({
      '/org/bluez/hci0/dev_F8_D3_F0_3C_84_4A': {
        'org.bluez.Device1': { 'Paired': <true>, 'Bonded': <true> }
      }
    })")
                  .c_str());
    auto objects = parse_managed_objects(p.v);
    objects.experimental_api = true;
    auto cap = resolve_capability(objects);

    EXPECT_EQ(cap.bearer_api, BearerApi::Unknown);
    EXPECT_EQ(cap.mode, DeliveryMode::Full);
}

TEST(Capability, BearerUnknownWithoutAnyBondStaysFull) {
    Payload p(ADAPTER_FULL);
    auto objects = parse_managed_objects(p.v);
    objects.experimental_api = true;
    auto cap = resolve_capability(objects);

    EXPECT_EQ(cap.mode, DeliveryMode::Full);
    EXPECT_EQ(cap.bearer_api, BearerApi::Unknown);
    // Tentative, so it must say why rather than claiming a verified result.
    EXPECT_FALSE(cap.reasons.empty());
}

TEST(Capability, BlockedWhenAdapterMissingOrOff) {
    EXPECT_EQ(resolve_capability({}).mode, DeliveryMode::Blocked);

    Payload off(R"({
      '/org/bluez/hci0': {
        'org.bluez.Adapter1': { 'Powered': <false>, 'Roles': <['central', 'peripheral']> }
      }
    })");
    auto cap = resolve_capability(parse_managed_objects(off.v));
    EXPECT_EQ(cap.mode, DeliveryMode::Blocked);
    EXPECT_TRUE(cap.adapter_present);
}

TEST(Capability, BlockedWithoutCentralRole) {
    Payload p(R"({
      '/org/bluez/hci0': {
        'org.bluez.Adapter1': { 'Powered': <true>, 'Roles': <['peripheral']> }
      }
    })");
    EXPECT_EQ(resolve_capability(parse_managed_objects(p.v)).mode, DeliveryMode::Blocked);
}

TEST(Capability, CompatibilityWithoutAdvertising) {
    Payload p(R"({
      '/org/bluez/hci0': {
        'org.bluez.Adapter1': {
          'Powered': <true>, 'Roles': <['central', 'peripheral']>, 'Class': <uint32 8127496>
        }
      }
    })");
    auto cap = resolve_capability(parse_managed_objects(p.v));
    EXPECT_EQ(cap.mode, DeliveryMode::Compatibility);
    EXPECT_FALSE(cap.advertising);
}

// Wrong Class of Device blocks the iPhone's permission toggles for MAP and PBAP
// too, so it is reported as a setup problem rather than an ANCS downgrade.
TEST(Capability, WrongClassIsReportedButDoesNotDowngradeMode) {
    Payload p(join(R"({
      '/org/bluez/hci0': {
        'org.bluez.Adapter1': {
          'Powered': <true>, 'Roles': <['central', 'peripheral']>, 'Class': <uint32 8126732>
        },
        'org.bluez.LEAdvertisingManager1': { 'SupportedInstances': <byte 15> }
      }
    })",
                   IPHONE_BONDED)
                  .c_str());
    auto cap = resolve_capability(parse_managed_objects(p.v));

    EXPECT_EQ(cap.mode, DeliveryMode::Full);
    EXPECT_FALSE(cap.class_ok);
    EXPECT_FALSE(cap.reasons.empty());
}

TEST(Capability, PrefersAPoweredAdapter) {
    Payload p(R"({
      '/org/bluez/hci0': {
        'org.bluez.Adapter1': { 'Powered': <false>, 'Roles': <['central']> }
      },
      '/org/bluez/hci1': {
        'org.bluez.Adapter1': { 'Powered': <true>, 'Roles': <['central', 'peripheral']> }
      }
    })");
    auto cap = resolve_capability(parse_managed_objects(p.v));
    EXPECT_TRUE(cap.powered);
    EXPECT_TRUE(cap.le_peripheral);
}
