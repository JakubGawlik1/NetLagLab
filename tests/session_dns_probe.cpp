#include <arpa/inet.h>
#include <netdb.h>

#include <cstdio>
#include <cstring>

int main(const int argc, char* argv[])
{
    if (argc != 2) {
        return 2;
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses{};
    const int lookup_result{getaddrinfo(argv[1], nullptr, &hints, &addresses)};
    if (lookup_result != 0) {
        std::fprintf(stderr, "getaddrinfo failed: %s\n", gai_strerror(lookup_result));
        return 1;
    }
    bool found_expected_address{};
    for (const addrinfo* current{addresses}; current != nullptr;
         current = current->ai_next) {
        char address[INET_ADDRSTRLEN]{};
        const auto* ipv4{reinterpret_cast<const sockaddr_in*>(current->ai_addr)};
        if (inet_ntop(AF_INET, &ipv4->sin_addr, address, sizeof(address)) != nullptr) {
            std::puts(address);
            found_expected_address = found_expected_address
                || std::strcmp(address, "203.0.113.77") == 0;
        }
    }
    freeaddrinfo(addresses);
    return found_expected_address ? 0 : 1;
}
