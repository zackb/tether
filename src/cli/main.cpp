#include <iostream>
#include <tether/core.hpp>

int main(int argc, char** argv) {
    std::cout << "tether cli version " << tether::get_version() << "\n";
    return 0;
}
