#include <iostream>
#include <string_view>

int main(int argc, char* argv[])
{
    if (argc == 1) {
        std::cout << "NetLagLab\n";
        return 0;
    }

    if (argc > 2) {
        std::cerr << "Error: expected at most one argument\n";
        return 1;
    }

    const std::string_view argument{argv[1]};

    if (argument == "--version") {
        std::cout << "NetLagLab " << NETLAGLAB_VERSION << '\n';
        return 0;
    }

    std::cerr << "Error: unknown argument '" << argument << "'\n";
    return 1;
}
