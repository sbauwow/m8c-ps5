// m8c home-screen launcher.
//
// The real m8c runs as an hbldr payload (/data/homebrew/m8c/eboot.elf): as a
// payload it has the privileges sceUsbd and /data need, which a sandboxed
// title does not. This title only asks the local websrv loader to start it.
// hbldr's daemon=0 launch replaces this app, so on success we never return.

#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace
{
struct NetSockaddrIn
{
    std::uint8_t length;
    std::uint8_t family;
    std::uint16_t port;
    std::uint32_t address;
    std::uint16_t virtual_port;
    std::uint8_t zero[6];
};

struct NotifyRequest
{
    char unused[45];
    char message[3075];
};

constexpr std::uint16_t loader_port = 8080;
constexpr char request[] = "GET /hbldr?pipe=0&daemon=0&path=/data/homebrew/m8c/eboot.elf HTTP/1.0\r\n"
                           "Host: 127.0.0.1\r\n\r\n";
} // namespace

extern "C"
{
    int sceNetSocket(const char *name, int domain, int type, int protocol);
    int sceNetConnect(int socket, const void *address, std::uint32_t address_length);
    int sceNetSend(int socket, const void *data, std::size_t length, int flags);
    int sceNetRecv(int socket, void *data, std::size_t length, int flags);
    int sceNetSetsockopt(int socket, int level, int option, const void *value, std::uint32_t size);
    int sceNetSocketClose(int socket);
    int sceKernelSendNotificationRequest(int device, NotifyRequest *request, std::size_t size, int blocking);
    int sceKernelUsleep(unsigned int microseconds);
    int sceSystemServiceHideSplashScreen();
}

namespace
{
void notify(const char *text)
{
    NotifyRequest req{};
    std::strncpy(req.message, text, sizeof(req.message) - 1);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

bool ask_loader()
{
    const int sock = sceNetSocket("m8c_launcher", 2, 1, 6);
    if (sock < 0)
        return false;
    constexpr std::uint32_t timeout_us = 5'000'000;
    for (const int option : {0x1105, 0x1106, 0x1109}) // send, receive, connect timeouts
        sceNetSetsockopt(sock, 0xffff, option, &timeout_us, sizeof(timeout_us));

    const NetSockaddrIn address{sizeof(NetSockaddrIn),
                                2,
                                static_cast<std::uint16_t>((loader_port << 8) | (loader_port >> 8)),
                                0x0100007f, // 127.0.0.1
                                0,
                                {0}};
    bool ok = sceNetConnect(sock, &address, sizeof(address)) >= 0 &&
              sceNetSend(sock, request, sizeof(request) - 1, 0) == static_cast<int>(sizeof(request) - 1);
    if (ok)
    {
        char reply[256];
        ok = sceNetRecv(sock, reply, sizeof(reply), 0) > 0 && std::strncmp(reply, "HTTP/1.", 7) == 0 &&
             std::strncmp(reply + 9, "200", 3) == 0;
    }
    sceNetSocketClose(sock);
    return ok;
}
} // namespace

int main()
{
    sceSystemServiceHideSplashScreen();
    if (!ask_loader())
    {
        notify("m8c: couldn't reach the homebrew loader on port 8080.\nRun the jailbreak (websrv) first.");
        return 1;
    }
    // hbldr is swapping this app for m8c; wait to be replaced.
    for (int i = 0; i < 30; i++)
        sceKernelUsleep(1'000'000);
    notify("m8c: the loader accepted the launch but m8c didn't start.\nCheck /data/homebrew/m8c/eboot.elf.");
    return 1;
}
