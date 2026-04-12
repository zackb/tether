#include <iostream>
#include <tether/core.hpp>

int main(int argc, char** argv) {
    std::cout << "tether-gtk version " << tether::get_version() << "\n";
    std::cout << "Starting GTK UI stub...\n";
    return 0;
}
