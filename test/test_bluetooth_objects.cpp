#include <gtest/gtest.h>
#include <tether/bluetooth/objects.hpp>
#include <tether/packaging.hpp>
#include <tether/service.hpp>

#include <sstream>
#include <string_view>

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
          'Address': <'02:00:00:00:00:02'>,
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

    // The same adapter on a controller BlueZ gave no LEAdvertisingManager1 at all,
    // which is what a controller reporting no LE advertising support looks like.
    constexpr const char* ADAPTER_NO_ADVERTISING = R"({
      '/org/bluez/hci0': {
        'org.bluez.Adapter1': {
          'Address': <'02:00:00:00:00:02'>,
          'Alias': <'btw'>,
          'Class': <uint32 8127496>,
          'Powered': <true>,
          'Roles': <['central', 'peripheral']>
        }
      }
    })";

    // The same adapter as reported by a bluetoothd running with the experimental API on:
    // AdvertisementMonitorManager1 is created only under that flag.
    constexpr const char* ADAPTER_EXPERIMENTAL = R"({
      '/org/bluez/hci0': {
        'org.bluez.Adapter1': {
          'Address': <'02:00:00:00:00:02'>,
          'Alias': <'btw'>,
          'Class': <uint32 8127496>,
          'Powered': <true>,
          'Roles': <['central', 'peripheral']>
        },
        'org.bluez.LEAdvertisingManager1': {
          'SupportedInstances': <byte 15>
        },
        'org.bluez.AdvertisementMonitorManager1': {
          'SupportedFeatures': <['controller-patterns']>
        }
      }
    })";

    // An iPhone bonded on both transports, exposing MAP, PBAP and ANCS.
    constexpr const char* IPHONE_BONDED = R"({
      '/org/bluez/hci0/dev_02_00_00_00_00_01': {
        'org.bluez.Device1': {
          'Address': <'02:00:00:00:00:01'>,
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
    EXPECT_EQ(a.address, "02:00:00:00:00:02");
    EXPECT_EQ(a.name, "btw");
    EXPECT_TRUE(a.powered);
    EXPECT_TRUE(a.has_role("central"));
    EXPECT_TRUE(a.has_role("peripheral"));
    EXPECT_FALSE(a.has_role("broadcaster"));
    EXPECT_TRUE(a.has_advertising_manager);
    EXPECT_EQ(a.advertising_instances, 15);
    EXPECT_EQ(to_json(a)["advertising_manager"], true);
    // 8127496 == 0x7c0408: service bits set above an A/V Hands-Free base class.
    EXPECT_TRUE(a.class_is_handsfree());
}

// DeviceID in main.conf is what makes AirPods treat the machine as a Mac.
TEST(BluetoothObjects, ReadsTheApplePresentationFromTheAdapterModalias) {
    Adapter apple;
    apple.modalias = "bluetooth:v004Cp0000d0000";
    EXPECT_TRUE(apple.presents_as_apple());

    Adapter stock;
    stock.modalias = "usb:v1D6Bp0246d0548";
    EXPECT_FALSE(stock.presents_as_apple());

    EXPECT_FALSE(Adapter{}.presents_as_apple());
}

TEST(BluetoothObjects, ParsesDeviceAndBearer) {
    Payload p(IPHONE_BONDED);
    auto objects = parse_managed_objects(p.v);

    ASSERT_EQ(objects.devices.size(), 1u);
    const auto& d = objects.devices[0];
    EXPECT_EQ(d.address, "02:00:00:00:00:01");
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

TEST(BluetoothObjects, RecognizesAnUnpairedAppleNearbyAdvertisement) {
    Payload p(R"({
      '/org/bluez/hci0/dev_40_F6_64_3D_7A_F1': {
        'org.bluez.Device1': {
          'Address': <'40:F6:64:3D:7A:F1'>,
          'AddressType': <'random'>,
          'Alias': <'40-F6-64-3D-7A-F1'>,
          'ManufacturerData': <{uint16 76: <[byte 0x10, 0x06, 0x71, 0x1e]>}>,
          'UUIDs': <@as []>
        }
      }
    })");
    auto objects = parse_managed_objects(p.v);

    ASSERT_EQ(objects.devices.size(), 1u);
    const auto encoded = to_json(objects.devices[0]);
    EXPECT_TRUE(encoded.value("apple_nearby", false));
    EXPECT_FALSE(objects.devices[0].looks_like_iphone());
}

TEST(BluetoothObjects, DoesNotTreatEveryManufacturerAdvertisementAsAPhoneCandidate) {
    Payload p(R"({
      '/org/bluez/hci0/dev_AA': {
        'org.bluez.Device1': {
          'ManufacturerData': <{uint16 76: <[byte 0x07, 0x19]>}>
        }
      },
      '/org/bluez/hci0/dev_BB': {
        'org.bluez.Device1': {
          'ManufacturerData': <{uint16 64: <[byte 0x10, 0x06]>}>
        }
      }
    })");
    auto objects = parse_managed_objects(p.v);

    ASSERT_EQ(objects.devices.size(), 2u);
    EXPECT_FALSE(to_json(objects.devices[0]).value("apple_nearby", false));
    EXPECT_FALSE(to_json(objects.devices[1]).value("apple_nearby", false));
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
    EXPECT_TRUE(cap.setup.empty());
}

TEST(Capability, SecureConnectionsStateIsInjectedRatherThanReadDuringResolution) {
    Payload p(join(ADAPTER_FULL, IPHONE_BONDED).c_str());
    const auto objects = parse_managed_objects(p.v);

    const auto unknown = resolve_capability(objects);
    EXPECT_FALSE(unknown.secure_connections_known);

    const auto enabled = resolve_capability(objects, true);
    EXPECT_TRUE(enabled.secure_connections_known);
    EXPECT_TRUE(enabled.secure_connections);

    const auto disabled = resolve_capability(objects, false);
    EXPECT_TRUE(disabled.secure_connections_known);
    EXPECT_FALSE(disabled.secure_connections);
}

// Reading the flag off bluetoothd's own objects is what makes it answerable from a
// Flatpak sandbox, where /proc holds no bluetoothd, and from a main.conf
// 'Experimental = true' that leaves no argv trace.
TEST(BluetoothObjects, ExperimentalComesFromTheAdvertisementMonitorManager) {
    Payload on(ADAPTER_EXPERIMENTAL);
    auto with = parse_managed_objects(on.v);
    ASSERT_EQ(with.adapters.size(), 1u);
    EXPECT_TRUE(with.adapters[0].has_adv_monitor_manager);
    EXPECT_TRUE(with.experimental_api);

    Payload off(ADAPTER_FULL);
    auto without = parse_managed_objects(off.v);
    ASSERT_EQ(without.adapters.size(), 1u);
    EXPECT_FALSE(without.adapters[0].has_adv_monitor_manager);
    EXPECT_FALSE(without.experimental_api);
}

// The experimental step must clear on its own once bluetoothd restarts with the flag,
// with nothing bonded yet to populate Bearer.LE1.
TEST(Capability, ExperimentalAdapterAloneClearsTheSetupStep) {
    Payload p(ADAPTER_EXPERIMENTAL);
    auto cap = resolve_capability(parse_managed_objects(p.v));

    EXPECT_EQ(cap.bearer_api, BearerApi::Unknown);
    for (const auto& step : cap.setup)
        EXPECT_EQ(step.command.find("--experimental"), std::string::npos) << step.command;
}

// Without --experimental the bearer interface can never appear, so its absence
// is conclusive.
TEST(Capability, CompatibilityWhenExperimentalIsOff) {
    Payload p(join(ADAPTER_FULL, R"({
      '/org/bluez/hci0/dev_02_00_00_00_00_01': {
        'org.bluez.Device1': { 'Paired': <true>, 'Bonded': <true> }
      }
    })")
                  .c_str());
    auto objects = parse_managed_objects(p.v);
    objects.experimental_api = false;
    auto cap = resolve_capability(objects);

    EXPECT_EQ(cap.mode, DeliveryMode::Compatibility);
    EXPECT_EQ(cap.bearer_api, BearerApi::Absent);
    ASSERT_EQ(cap.setup.size(), 1u);
    EXPECT_EQ(cap.setup[0].command, enable_experimental_command(tether::systemd_booted()));

    // ExecStart must resolve bluetoothd on the machine running the command
    const std::string systemd = enable_experimental_command(true);
    EXPECT_NE(systemd.find("bluetooth.service.d"), std::string::npos);
    EXPECT_EQ(systemd.find("/usr/share/tether"), std::string::npos) << systemd;
    EXPECT_NE(systemd.find("bluetoothd"), std::string::npos) << systemd;
}

// Without --experimental bluetoothd registers Bearer.LE1 as an empty marker: no
// properties, no Connect(). Reading its mere presence as proof reported a usable
// LE bearer, suppressed the drop-in advice, and left le_bonded permanently false
// -- which in turn latched notification mirroring off after every pairing.
TEST(Capability, AnEmptyBearerInterfaceIsNotProofOfTheApi) {
    Payload p(join(ADAPTER_FULL, R"({
      '/org/bluez/hci0/dev_02_00_00_00_00_01': {
        'org.bluez.Device1': { 'Paired': <true>, 'Bonded': <true> },
        'org.bluez.Bearer.LE1': {},
        'org.bluez.Bearer.BREDR1': {}
      }
    })")
                  .c_str());
    auto objects = parse_managed_objects(p.v);
    objects.experimental_api = false;
    auto cap = resolve_capability(objects);

    ASSERT_FALSE(objects.devices.empty());
    EXPECT_FALSE(objects.devices[0].has_le_bearer);
    EXPECT_EQ(cap.bearer_api, BearerApi::Absent);
    ASSERT_EQ(cap.setup.size(), 1u);
    EXPECT_EQ(cap.setup[0].command, enable_experimental_command(tether::systemd_booted()));
}

TEST(Capability, ClassicOnlyBondDoesNotDisproveBearerApi) {
    Payload p(join(ADAPTER_FULL, R"({
      '/org/bluez/hci0/dev_02_00_00_00_00_03': {
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

// A bonded iPhone whose Device1 carries no LE bearer is a bond that was made
// without cross-transport key derivation: it can never carry ANCS, and no amount
// of re-soliciting or restarting bluetoothd changes that. Reporting it as
// "unconfirmed until a device is bonded" -- to someone looking at a bonded device
// -- was a dead end in every bug report it produced.
TEST(Capability, ClassicOnlyIphoneBondIsNamedAsSuch) {
    Payload p(join(ADAPTER_FULL, R"({
      '/org/bluez/hci0/dev_02_00_00_00_00_01': {
        'org.bluez.Device1': {
          'Paired': <true>,
          'Bonded': <true>,
          'UUIDs': <['00001132-0000-1000-8000-00805f9b34fb', '0000112f-0000-1000-8000-00805f9b34fb']>
        }
      }
    })")
                  .c_str());
    auto objects = parse_managed_objects(p.v);
    objects.experimental_api = true;
    auto cap = resolve_capability(objects);

    EXPECT_TRUE(cap.bonded_device_present);
    EXPECT_FALSE(cap.bond_has_le);

    bool named = false;
    for (const auto& reason : cap.reasons) {
        if (reason.find("BR/EDR only") != std::string::npos) {
            named = true;
            EXPECT_NE(reason.find("Forget This Device"), std::string::npos) << "names no remedy";
        }
        EXPECT_EQ(reason.find("unconfirmed until a device is bonded"), std::string::npos)
            << "still claims nothing is bonded";
    }
    EXPECT_TRUE(named) << "a bond with no LE half went unreported";

    // The controller can still do ANCS; it is this bond that cannot. Downgrading
    // the mode would be a claim about the hardware.
    EXPECT_EQ(cap.mode, DeliveryMode::Full);
}

// Bearer.LE1 carrying properties says the experimental API is live. It says
// nothing about the bond, and reading it as "the bond has an LE half" reported
// "Bond: BR/EDR + LE" for every bonded device on a machine running bluetoothd
// --experimental -- which is what made issue #128's dump unable to say whether
// the controller had derived the LE keys at all.
TEST(Capability, APopulatedLeBearerIsNotProofOfAnLeBond) {
    Payload p(join(ADAPTER_FULL, R"({
      '/org/bluez/hci0/dev_02_00_00_00_00_01': {
        'org.bluez.Device1': {
          'Paired': <true>,
          'Bonded': <true>,
          'UUIDs': <['00001132-0000-1000-8000-00805f9b34fb', '0000112f-0000-1000-8000-00805f9b34fb']>
        },
        'org.bluez.Bearer.LE1': { 'Paired': <false>, 'Bonded': <false> }
      }
    })")
                  .c_str());
    auto objects = parse_managed_objects(p.v);
    objects.experimental_api = true;
    auto cap = resolve_capability(objects);

    ASSERT_FALSE(objects.devices.empty());
    EXPECT_TRUE(objects.devices[0].has_le_bearer);
    EXPECT_EQ(cap.bearer_api, BearerApi::Confirmed);
    EXPECT_TRUE(cap.bonded_device_present);
    EXPECT_FALSE(cap.bond_has_le);

    bool named = false;
    for (const auto& reason : cap.reasons)
        named = named || reason.find("BR/EDR only") != std::string::npos;
    EXPECT_TRUE(named) << "a bond with no LE half went unreported";
}

// A controller that cannot advertise never solicits ANCS, so the iPhone is never
// asked for the service and no bond made on it can have an LE half. Telling that
// user to Forget This Device and pair again sends them round a loop that cannot
// terminate -- which is exactly what issue #118 recorded, three times over.
TEST(Capability, NoRepairAdviceWhenTheAdapterCannotAdvertise) {
    Payload p(join(ADAPTER_NO_ADVERTISING, R"({
      '/org/bluez/hci0/dev_02_00_00_00_00_01': {
        'org.bluez.Device1': {
          'Paired': <true>,
          'Bonded': <true>,
          'UUIDs': <['00001132-0000-1000-8000-00805f9b34fb', '0000112f-0000-1000-8000-00805f9b34fb']>
        }
      }
    })")
                  .c_str());
    auto objects = parse_managed_objects(p.v);
    objects.experimental_api = true;
    auto cap = resolve_capability(objects);

    ASSERT_FALSE(cap.advertising);
    ASSERT_TRUE(cap.bonded_device_present);
    ASSERT_FALSE(cap.bond_has_le);

    bool named = false;
    for (const auto& reason : cap.reasons) {
        if (reason.find("BR/EDR only") == std::string::npos)
            continue;
        named = true;
        EXPECT_EQ(reason.find("Forget This Device"), std::string::npos) << "advises a re-pair that cannot help";
        EXPECT_EQ(reason.find("pair again"), std::string::npos) << "advises a re-pair that cannot help";
        EXPECT_NE(reason.find("cannot solicit ANCS"), std::string::npos) << "does not name the real cause";
    }
    EXPECT_TRUE(named) << "a bond with no LE half went unreported";
}

// Headsets and controllers bond over BR/EDR alone by design. Reading one of
// those as a broken bond fires on nearly every machine.
TEST(Capability, ClassicOnlyBondOnANonPhoneIsNotReported) {
    Payload p(join(ADAPTER_FULL, R"({
      '/org/bluez/hci0/dev_02_00_00_00_00_03': {
        'org.bluez.Device1': { 'Paired': <true>, 'Bonded': <true>, 'Alias': <'ZBZ AirPros'> }
      }
    })")
                  .c_str());
    auto objects = parse_managed_objects(p.v);
    objects.experimental_api = true;
    auto cap = resolve_capability(objects);

    EXPECT_FALSE(cap.bonded_device_present);
    for (const auto& reason : cap.reasons)
        EXPECT_EQ(reason.find("BR/EDR only"), std::string::npos) << "blamed a headset for having no LE bond";
}

// Issue #226: BlueZ 5.72 has no bearer API, so no bond there can show an LE
// half, yet ANCS was live on it. A working subscription outranks the inference.
TEST(Capability, LiveAncsOnABondWithNoLeHalfIsNotReported) {
    Payload p(join(ADAPTER_FULL, R"({
      '/org/bluez/hci0/dev_02_00_00_00_00_01': {
        'org.bluez.Device1': {
          'Paired': <true>,
          'Bonded': <true>,
          'Connected': <true>,
          'ServicesResolved': <true>,
          'UUIDs': <['00001132-0000-1000-8000-00805f9b34fb', '0000112f-0000-1000-8000-00805f9b34fb']>
        }
      },
      '/org/bluez/hci0/dev_02_00_00_00_00_01/service004f/char0050': {
        'org.bluez.GattCharacteristic1': { 'UUID': <'9FBF120D-6301-42D9-8C58-25E699A21DBD'>, 'Notifying': <true> }
      }
    })")
                  .c_str());
    auto objects = parse_managed_objects(p.v);
    objects.experimental_api = true;
    auto cap = resolve_capability(objects);

    ASSERT_TRUE(cap.bonded_device_present);
    EXPECT_FALSE(cap.bond_has_le);
    EXPECT_TRUE(cap.bond_carries_ancs);
    for (const auto& reason : cap.reasons)
        EXPECT_EQ(reason.find("BR/EDR only"), std::string::npos) << "called a working ANCS bond broken";
}

// Observed on a MediaTek MT7925 with the phone's ANCS session live and
// delivering: Bearer.LE1.Connected read false the whole time, because the phone
// opened the link inbound in answer to the solicitation advert. GATT exists only
// over LE and BlueZ withdraws these objects when the link drops, so their
// presence is the more trustworthy signal of the two.
TEST(DeviceParsing, AnAncsNotifySessionProvesTheLeLinkIsUp) {
    Payload p(join(ADAPTER_FULL, R"({
      '/org/bluez/hci0/dev_02_00_00_00_00_01': {
        'org.bluez.Device1': { 'Paired': <true>, 'Bonded': <true>, 'Connected': <true>,
                               'ServicesResolved': <true> },
        'org.bluez.Bearer.LE1': { 'Paired': <true>, 'Bonded': <true>, 'Connected': <false> }
      },
      '/org/bluez/hci0/dev_02_00_00_00_00_01/service004f/char0050': {
        'org.bluez.GattCharacteristic1': { 'UUID': <'9FBF120D-6301-42D9-8C58-25E699A21DBD'>, 'Notifying': <true> }
      }
    })")
                  .c_str());
    auto objects = parse_managed_objects(p.v);

    ASSERT_EQ(objects.devices.size(), 1u);
    EXPECT_FALSE(objects.devices[0].le_connected) << "the fixture is not reproducing the disagreement";
    EXPECT_TRUE(objects.devices[0].ancs_notifying);
    EXPECT_TRUE(objects.devices[0].le_link_up()) << "a live ANCS session was reported as no LE link";
}

// Measured on this hardware: Bearer.LE1.Disconnect under a device still Classic
// connected leaves the whole GATT tree in place -- 23 characteristics, both ANCS
// notify sources still Notifying, the ANCS UUID still on Device1 -- for as long as
// it was watched. Only ServicesResolved goes with the ATT link. Reading Notifying
// alone latched le_link_up() true on a dead link, which took the supervisor's LE
// recovery, the solicitation and the outbound dial all off the table.
TEST(DeviceParsing, ANotifyFlagLeftOverADeadLeBearerIsNotALink) {
    Payload p(join(ADAPTER_FULL, R"({
      '/org/bluez/hci0/dev_02_00_00_00_00_01': {
        'org.bluez.Device1': { 'Paired': <true>, 'Bonded': <true>, 'Connected': <true>,
                               'ServicesResolved': <false> },
        'org.bluez.Bearer.LE1': { 'Paired': <true>, 'Bonded': <true>, 'Connected': <false> },
        'org.bluez.Bearer.BREDR1': { 'Connected': <true> }
      },
      '/org/bluez/hci0/dev_02_00_00_00_00_01/service004f/char0050': {
        'org.bluez.GattCharacteristic1': { 'UUID': <'9FBF120D-6301-42D9-8C58-25E699A21DBD'>, 'Notifying': <true> }
      }
    })")
                  .c_str());
    auto objects = parse_managed_objects(p.v);

    ASSERT_EQ(objects.devices.size(), 1u);
    EXPECT_TRUE(objects.devices[0].ancs_notifying) << "the fixture is not reproducing the stale flag";
    EXPECT_FALSE(objects.devices[0].services_resolved);
    EXPECT_FALSE(objects.devices[0].le_link_up()) << "a stale notify flag was read as a live LE link";
    EXPECT_TRUE(objects.devices[0].classic_link_up()) << "Classic is up throughout; only LE dropped";
}

// Captured after BR/EDR dropped out of range and came back without LE: BR/EDR
// discovery sets ServicesResolved again, so the parsed properties read as a live
// LE link. Only a read through gap_name_path can tell, so the path must be found.
TEST(DeviceParsing, ClassicReconnectLeavesAStaleLeLinkThatNeedsAReadToDisprove) {
    Payload p(join(ADAPTER_FULL, R"({
      '/org/bluez/hci0/dev_02_00_00_00_00_01': {
        'org.bluez.Device1': { 'Paired': <true>, 'Bonded': <true>, 'Connected': <true>,
                               'ServicesResolved': <true> },
        'org.bluez.Bearer.LE1': { 'Paired': <true>, 'Bonded': <true>, 'Connected': <false> },
        'org.bluez.Bearer.BREDR1': { 'Connected': <true> }
      },
      '/org/bluez/hci0/dev_02_00_00_00_00_01/service0001/char0002': {
        'org.bluez.GattCharacteristic1': { 'UUID': <'00002a00-0000-1000-8000-00805f9b34fb'> }
      },
      '/org/bluez/hci0/dev_02_00_00_00_00_01/service0059/char005d': {
        'org.bluez.GattCharacteristic1': { 'UUID': <'9fbf120d-6301-42d9-8c58-25e699a21dbd'>, 'Notifying': <true> }
      }
    })")
                  .c_str());
    auto objects = parse_managed_objects(p.v);

    ASSERT_EQ(objects.devices.size(), 1u);
    EXPECT_TRUE(objects.devices[0].le_link_up()) << "the fixture is not reproducing the latch";
    EXPECT_EQ(objects.devices[0].gap_name_path, "/org/bluez/hci0/dev_02_00_00_00_00_01/service0001/char0002");
}

// The characteristic has to belong to this device. A sibling device's GATT tree
// saying nothing about ours is the whole reason the paths are matched.
TEST(DeviceParsing, AnAncsNotifySessionBelongsToOneDeviceOnly) {
    Payload p(join(ADAPTER_FULL, R"({
      '/org/bluez/hci0/dev_02_00_00_00_00_01': {
        'org.bluez.Device1': { 'Paired': <true>, 'Bonded': <true> }
      },
      '/org/bluez/hci0/dev_02_00_00_00_00_03': {
        'org.bluez.Device1': { 'Paired': <true>, 'Bonded': <true> }
      },
      '/org/bluez/hci0/dev_02_00_00_00_00_03/service004f/char0050': {
        'org.bluez.GattCharacteristic1': { 'UUID': <'9fbf120d-6301-42d9-8c58-25e699a21dbd'>, 'Notifying': <true> }
      }
    })")
                  .c_str());
    auto objects = parse_managed_objects(p.v);

    ASSERT_EQ(objects.devices.size(), 2u);
    EXPECT_FALSE(objects.devices[0].ancs_notifying) << "borrowed a sibling device's notify session";
    EXPECT_TRUE(objects.devices[1].ancs_notifying);
}

// An LE link that is down has no GATT tree, which is what makes its presence
// mean anything.
// BlueZ caches a bonded device's attributes (main.conf [GATT] Cache, default
// always), so the ANCS characteristics stay in the object tree long after the LE
// link is gone. Reading their presence as a live link reported LE up on a dead
// one, which stopped the bearer supervisor dialling it at all.
TEST(DeviceParsing, CachedCharacteristicsWithoutNotifyAreNotALink) {
    Payload p(join(ADAPTER_FULL, R"({
      '/org/bluez/hci0/dev_02_00_00_00_00_01': {
        'org.bluez.Device1': { 'Paired': <true>, 'Bonded': <true>, 'Connected': <true>, 'ServicesResolved': <true> },
        'org.bluez.Bearer.LE1': { 'Paired': <true>, 'Bonded': <true>, 'Connected': <false> }
      },
      '/org/bluez/hci0/dev_02_00_00_00_00_01/service006f/char0073': {
        'org.bluez.GattCharacteristic1': { 'UUID': <'9fbf120d-6301-42d9-8c58-25e699a21dbd'>, 'Notifying': <false> }
      }
    })")
                  .c_str());
    auto objects = parse_managed_objects(p.v);

    ASSERT_EQ(objects.devices.size(), 1u);
    EXPECT_FALSE(objects.devices[0].ancs_notifying);
    EXPECT_FALSE(objects.devices[0].le_link_up()) << "a cached GATT tree was read as a live LE link";
}

TEST(DeviceParsing, NoGattTreeAtAllMeansNoLeLink) {
    Payload p(join(ADAPTER_FULL, R"({
      '/org/bluez/hci0/dev_02_00_00_00_00_01': {
        'org.bluez.Device1': { 'Paired': <true>, 'Bonded': <true>, 'Connected': <true> },
        'org.bluez.Bearer.LE1': { 'Paired': <true>, 'Bonded': <true>, 'Connected': <false> }
      }
    })")
                  .c_str());
    auto objects = parse_managed_objects(p.v);

    ASSERT_EQ(objects.devices.size(), 1u);
    EXPECT_FALSE(objects.devices[0].ancs_notifying);
    EXPECT_FALSE(objects.devices[0].le_link_up());
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
    // The unit is templated on the adapter, so the wrong id makes the command a no-op.
    EXPECT_EQ(cap.adapter_id, "hci0");
    ASSERT_EQ(cap.setup.size(), 1u);
    EXPECT_EQ(cap.setup[0].command, set_class_command("hci0", tether::systemd_booted()));
}

TEST(Capability, ClassCommandWithSystemdEnablesTheUnit) {
    const std::string command = set_class_command("hci0", true);
    EXPECT_TRUE(command.ends_with("sudo systemctl enable --now tether-btclass@hci0")) << command;

    // The portable-build branch pastes the unit through a here-document, and a here-document
    // delimiter only ends it at column 0. That is why the unit text has to end in a newline:
    // without it EOF lands on the last line of the unit and closes nothing.
    EXPECT_TRUE(std::string_view(tether::packaging::BTCLASS_UNIT).ends_with("\n"));
    if (command.find("<<'EOF'") != std::string::npos) {
        bool terminated = false;
        std::istringstream lines(command);
        for (std::string line; std::getline(lines, line);)
            terminated = terminated || line == "EOF";
        EXPECT_TRUE(terminated) << command;
    }
}

// No unit runs without systemd, even where a package left the file on disk.
TEST(Capability, ClassCommandWithoutSystemdEditsMainConf) {
    const std::string command = set_class_command("hci0", false);
    EXPECT_NE(command.find("Class = 0x000408"), std::string::npos) << command;
    EXPECT_NE(command.find("echo | sudo btmgmt --index hci0 class 4 8"), std::string::npos) << command;
    EXPECT_EQ(command.find("systemctl"), std::string::npos) << command;
}

TEST(Capability, ExperimentalCommandWithoutSystemdEditsMainConf) {
    const std::string command = enable_experimental_command(false);
    EXPECT_NE(command.find("Experimental = true"), std::string::npos) << command;
    EXPECT_EQ(command.find("systemd"), std::string::npos) << command;
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

// Two controllers is the common case that broke #69: the first one is a clone
// dongle whose LE half never works, and everything ran on it because it sorts
// first. The configured id has to win.
constexpr const char* TWO_ADAPTERS = R"({
  '/org/bluez/hci0': {
    'org.bluez.Adapter1': { 'Address': <'AA:AA:AA:AA:AA:AA'>, 'Powered': <true>, 'Roles': <['central']> }
  },
  '/org/bluez/hci1': {
    'org.bluez.Adapter1': { 'Address': <'BB:BB:BB:BB:BB:BB'>, 'Powered': <true>, 'Roles': <['central', 'peripheral']> }
  }
})";

TEST(PreferredAdapter, ConfiguredIdWinsOverTheFirstPowered) {
    Payload p(TWO_ADAPTERS);
    auto objects = parse_managed_objects(p.v);

    ASSERT_TRUE(preferred_adapter(objects, "hci1"));
    EXPECT_EQ(preferred_adapter(objects, "hci1")->path, "/org/bluez/hci1");
    // The address is the other spelling a user has in front of them.
    EXPECT_EQ(preferred_adapter(objects, "bb:bb:bb:bb:bb:bb")->path, "/org/bluez/hci1");
    EXPECT_EQ(resolve_capability(objects, std::nullopt, "hci1").adapter_id, "hci1");
    EXPECT_TRUE(resolve_capability(objects, std::nullopt, "hci1").le_peripheral);
}

// An unplugged controller must not leave the machine with no Bluetooth at all.
TEST(PreferredAdapter, FallsBackWhenTheConfiguredOneIsAbsent) {
    Payload p(TWO_ADAPTERS);
    auto objects = parse_managed_objects(p.v);

    ASSERT_TRUE(preferred_adapter(objects, "hci7"));
    EXPECT_EQ(preferred_adapter(objects, "hci7")->path, "/org/bluez/hci0");
    EXPECT_EQ(preferred_adapter(objects, "")->path, "/org/bluez/hci0");
}

TEST(PreferredAdapter, PrefersPoweredThenFirst) {
    Payload p(R"({
      '/org/bluez/hci0': {
        'org.bluez.Adapter1': { 'Powered': <false>, 'Roles': <['central']> }
      },
      '/org/bluez/hci1': {
        'org.bluez.Adapter1': { 'Powered': <true>, 'Roles': <['central']> }
      }
    })");
    auto objects = parse_managed_objects(p.v);
    EXPECT_EQ(preferred_adapter(objects, "")->path, "/org/bluez/hci1");
    // An unpowered adapter named outright is still the one to describe, so the
    // capability reasons can say it is off rather than reporting nothing.
    EXPECT_EQ(preferred_adapter(objects, "hci0")->path, "/org/bluez/hci0");

    BluezObjects empty;
    EXPECT_EQ(preferred_adapter(empty, "hci0"), nullptr);
}

// Device1.Connected is an aggregate across bearers. A phone holding an LE link
// with no BR/EDR link reports Connected=true while MAP and PBAP — which both
// ride BR/EDR — are unreachable, so obexd answers every session attempt with
// "Unable to find service record". Reading the aggregate as the Classic link
// makes the daemon report a healthy BR/EDR connection and never reconnect it.
constexpr const char* IPHONE_LE_ONLY_LINK = R"({
  '/org/bluez/hci0/dev_02_00_00_00_00_01': {
    'org.bluez.Device1': {
      'Address': <'02:00:00:00:00:01'>,
      'Adapter': <objectpath '/org/bluez/hci0'>,
      'Paired': <true>,
      'Bonded': <true>,
      'Connected': <true>,
      'UUIDs': <['00001132-0000-1000-8000-00805f9b34fb']>
    },
    'org.bluez.Bearer.BREDR1': {
      'Paired': <true>,
      'Bonded': <true>,
      'Connected': <false>
    },
    'org.bluez.Bearer.LE1': {
      'Paired': <true>,
      'Bonded': <true>,
      'Connected': <true>
    }
  }
})";

TEST(BluetoothObjects, ClassicLinkComesFromTheBredrBearerNotTheAggregate) {
    Payload payload(IPHONE_LE_ONLY_LINK);
    ASSERT_TRUE(payload.v);

    auto objects = parse_managed_objects(payload.v);
    ASSERT_EQ(objects.devices.size(), 1u);
    const Device& device = objects.devices.front();

    EXPECT_TRUE(device.connected) << "the aggregate is still reported as BlueZ gave it";
    EXPECT_TRUE(device.has_classic_bearer);
    EXPECT_TRUE(device.classic_state_known);
    EXPECT_FALSE(device.classic_connected);
    EXPECT_TRUE(device.le_connected);

    EXPECT_FALSE(device.classic_link_up())
        << "an LE-only link must not be reported as a Classic connection: every OBEX profile rides BR/EDR";
}

// Not every BlueZ exposes the bearer interfaces. Where they are absent the
// aggregate is the only signal there is, and it is the right one.
TEST(BluetoothObjects, ClassicLinkFallsBackToTheAggregateWithoutBearers) {
    constexpr const char* NO_BEARERS = R"({
      '/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF': {
        'org.bluez.Device1': {
          'Address': <'AA:BB:CC:DD:EE:FF'>,
          'Adapter': <objectpath '/org/bluez/hci0'>,
          'Paired': <true>,
          'Connected': <true>,
          'UUIDs': <@as []>
        }
      }
    })";
    Payload payload(NO_BEARERS);
    ASSERT_TRUE(payload.v);

    auto objects = parse_managed_objects(payload.v);
    ASSERT_EQ(objects.devices.size(), 1u);
    EXPECT_FALSE(objects.devices.front().has_classic_bearer);
    EXPECT_TRUE(objects.devices.front().classic_link_up());
}

// BlueZ strips a bearer interface down to a bare signal while the device is
// disconnected: the name is still in the object tree, but there are no
// properties to read and no methods to call. Treating that as an authoritative
// "BR/EDR is down" would let a stub contradict a link that is genuinely up.
TEST(BluetoothObjects, StubBearerIsNotMistakenForAReading) {
    constexpr const char* STUB_BEARERS = R"({
      '/org/bluez/hci0/dev_02_00_00_00_00_01': {
        'org.bluez.Device1': {
          'Address': <'02:00:00:00:00:01'>,
          'Adapter': <objectpath '/org/bluez/hci0'>,
          'Paired': <true>,
          'Connected': <true>,
          'UUIDs': <@as []>
        },
        'org.bluez.Bearer.BREDR1': {},
        'org.bluez.Bearer.LE1': {}
      }
    })";
    Payload payload(STUB_BEARERS);
    ASSERT_TRUE(payload.v);

    auto objects = parse_managed_objects(payload.v);
    ASSERT_EQ(objects.devices.size(), 1u);
    const Device& device = objects.devices.front();

    EXPECT_TRUE(device.has_classic_bearer) << "the interface is present, which is what confirms the bearer API";
    EXPECT_FALSE(device.classic_state_known) << "a stub carries no Connected property to believe";
    EXPECT_TRUE(device.classic_link_up()) << "with nothing to read, the aggregate is the only answer available";
}
