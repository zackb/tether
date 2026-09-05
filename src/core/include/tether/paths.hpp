#pragma once

#include <filesystem>

namespace tether::paths {

    // XDG base directories for tether's own files. Returns an empty path
    // when there is no home directory to fall back on. Nothing is created.
    //
    // A directory named by an XDG variable is used only when the value is
    // absolute, per the spec. When the XDG location does not exist yet but the
    // legacy $HOME one does, the legacy path wins so an existing install
    // doesn't break.

    // $XDG_CONFIG_HOME/tether, else ~/.config/tether.
    std::filesystem::path config_dir();

    // $XDG_DATA_HOME/tether, else ~/.local/share/tether.
    std::filesystem::path data_dir();

    // $XDG_STATE_HOME/tether, else ~/.local/state/tether.
    std::filesystem::path state_dir();

} // namespace tether::paths
