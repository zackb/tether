{
  lib,
  stdenv,
  fetchNpmDeps,
  npmHooks,
  npmConfigHook ? npmHooks.npmConfigHook,
  cmake,
  ninja,
  pkg-config,
  gettext,
  nodejs,
  zip,
  git,
  wrapGAppsHook3,
  wayland,
  avahi,
  openssl,
  glib,
  libsecret,
  gtk3,
  gtk-layer-shell,
  libnotify,
  ncurses,
  nlohmann_json,
  gtest,
  bluez,
  runtimeShell,
  gnugrep,
  coreutils,
  procps,
}:

stdenv.mkDerivation (finalAttrs: {
  pname = "tether";
  version = "0.2.18";

  src = lib.cleanSourceWith {
    name = "source";
    src = ../.;
    filter =
      path: _type:
      let
        relative = lib.removePrefix "${toString ../.}/" (toString path);
        components = lib.splitString "/" relative;
        topLevel = lib.head components;
      in
      (builtins.elem topLevel [
        "CMakeLists.txt"
        "LICENSE"
        "README.md"
        "cmake_uninstall.cmake.in"
        "docs"
        "extension"
        "packaging"
        "po"
        "src"
        "test"
      ])
      && !(lib.any (
        name:
        builtins.elem name [
          ".git"
          "build"
          "node_modules"
          "result"
        ]
      ) components)
      && !(lib.any (lib.hasPrefix "result-") components);
  };

  npmRoot = "extension";
  npmDeps = fetchNpmDeps {
    inherit (finalAttrs) src;
    sourceRoot = "source/extension";
    hash = "sha256-/GrpRCr9/xv8fwh7b8zl37aMUh2rKeE6vJuyw8S/S+o=";
  };

  nativeBuildInputs = [
    cmake
    ninja
    pkg-config
    gettext
    nodejs
    npmConfigHook
    zip
    git
    wrapGAppsHook3
  ];

  buildInputs = [
    wayland
    avahi
    openssl
    glib
    libsecret
    gtk3
    gtk-layer-shell
    libnotify
    ncurses
  ];

  cmakeFlags = [
    (lib.cmakeBool "FETCHCONTENT_FULLY_DISCONNECTED" true)
    (lib.cmakeFeature "FETCHCONTENT_SOURCE_DIR_JSON" "${nlohmann_json.src}")
    (lib.cmakeFeature "FETCHCONTENT_SOURCE_DIR_GOOGLETEST" "${gtest.src}")
    (lib.cmakeFeature "TETHER_VERSION" finalAttrs.version)
    (lib.cmakeFeature "BLUETOOTHD_PATH" "${bluez}/libexec/bluetooth/bluetoothd")
    (lib.cmakeFeature "SYSTEMD_UNIT_DIR" "lib/systemd/system")
    (lib.cmakeFeature "CHROME_MESSAGING_DIR" "${placeholder "out"}/etc/chromium/native-messaging-hosts")
    (lib.cmakeFeature "GOOGLE_CHROME_MESSAGING_DIR" "${placeholder "out"}/etc/opt/chrome/native-messaging-hosts")
    (lib.cmakeFeature "MOZILLA_MESSAGING_DIR" "${placeholder "out"}/lib/mozilla/native-messaging-hosts")
    (lib.cmakeFeature "THUNDERBIRD_MESSAGING_DIR" "${placeholder "out"}/lib/thunderbird/native-messaging-hosts")
  ];

  postPatch = ''
    patchShebangs extension/build.sh
    substituteInPlace packaging/systemd/tether-btclass@.service \
      --replace-fail "/bin/sh" "${runtimeShell}" \
      --replace-fail "btmgmt --index" "${bluez}/bin/btmgmt --index" \
      --replace-fail "| grep -q" "| ${gnugrep}/bin/grep -q" \
      --replace-fail "    sleep 1;" "    ${coreutils}/bin/sleep 1;"
    substituteInPlace packaging/systemd/tetherd.service.in \
      --replace-fail "/usr/bin/pkill" "${procps}/bin/pkill"
  '';

  doCheck = true;

  postCheck = ''
    (cd .. && npm test --prefix extension)
  '';

  postInstall = ''
    patchShebangs "$out/bin/tether-native-host"

    for manifest in \
      "$out/etc/chromium/native-messaging-hosts/com.tether.extension.json" \
      "$out/etc/opt/chrome/native-messaging-hosts/com.tether.extension.json" \
      "$out/lib/mozilla/native-messaging-hosts/com.tether.extension.json" \
      "$out/lib/thunderbird/native-messaging-hosts/com.tether.extension.json"; do
      test -f "$manifest"
    done
  '';

  meta = {
    description = "Use an iPhone with Linux";
    homepage = "https://github.com/zackb/tether";
    license = lib.licenses.mit;
    platforms = lib.platforms.linux;
    mainProgram = "tether-gtk";
  };
})
