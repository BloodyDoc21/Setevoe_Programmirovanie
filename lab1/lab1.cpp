#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string_view>

static void usage(const char* program)
{
    std::cerr << "Использование: " << program
              << " <domain> <type> <ip> <port> [--bind]\n"
              << "  domain: AF_INET\n"
              << "  type:   SOCK_STREAM | SOCK_DGRAM\n"
              << "  ip:     IPv4-адрес или INADDR_ANY\n"
              << "  port:   0..65535\n";
}

// Сохраняем причину ошибки до закрытия дескриптора.
static void print_error(const char* operation, int error)
{
    std::cerr << operation << ": " << std::strerror(error) << '\n';
}

static bool close_socket(int fd)
{
    if (close(fd) == 0)
        return true;

    print_error("close", errno);
    return false;
}

static int fail(int fd, const char* operation, int error)
{
    print_error(operation, error);
    if (fd >= 0)
        close_socket(fd);
    return EXIT_FAILURE;
}

static const char* type_name(int type)
{
    if (type == SOCK_STREAM)
        return "SOCK_STREAM (TCP)";
    if (type == SOCK_DGRAM)
        return "SOCK_DGRAM (UDP)";
    return "неизвестный тип";
}

// Дескриптор самой папки в список не включаем.
static bool print_fds(const char* label)
{
    DIR* dir = opendir("/proc/self/fd");
    if (dir == nullptr) {
        print_error("opendir", errno);
        return false;
    }

    const int directory_fd = dirfd(dir);
    bool ok = directory_fd >= 0;
    if (!ok)
        print_error("dirfd", errno);
    else {
        std::cout << "Открытые дескрипторы (" << label << "):";
        while (true) {
            errno = 0;
            const dirent* entry = readdir(dir);
            if (entry == nullptr) {
                const int error = errno;
                std::cout << '\n';
                if (error != 0) {
                    print_error("readdir", error);
                    ok = false;
                }
                break;
            }
            if (entry->d_name[0] != '.'
                && std::atoi(entry->d_name) != directory_fd)
                std::cout << ' ' << entry->d_name;
        }
    }

    if (closedir(dir) == -1) {
        print_error("closedir", errno);
        return false;
    }
    return ok;
}

static void print_bytes(const char* label, std::uint16_t value)
{
    const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
    std::cout << label << std::hex << std::uppercase << std::setfill('0')
              << std::setw(2) << static_cast<unsigned>(bytes[0]) << ' '
              << std::setw(2) << static_cast<unsigned>(bytes[1])
              << std::dec << std::nouppercase << std::setfill(' ') << '\n';
}

int main(int argc, char* argv[])
{
    const bool do_bind =
        argc == 6 && std::string_view(argv[5]) == "--bind";
    if (argc != 5 && !do_bind) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (std::string_view(argv[1]) != "AF_INET") {
        std::cerr << "Ошибка: поддерживается только AF_INET\n";
        return EXIT_FAILURE;
    }

    const std::string_view type_arg(argv[2]);
    int type;
    if (type_arg == "SOCK_STREAM")
        type = SOCK_STREAM;
    else if (type_arg == "SOCK_DGRAM")
        type = SOCK_DGRAM;
    else {
        std::cerr << "Ошибка: неверный тип сокета " << type_arg << '\n';
        return EXIT_FAILURE;
    }

    const std::string_view port_arg(argv[4]);
    const char* end = port_arg.data() + port_arg.size();
    unsigned port = 0;
    const auto [next, error] = std::from_chars(port_arg.data(), end, port);
    if (error != std::errc{} || next != end || port > 65535) {
        std::cerr << "Ошибка: порт должен быть числом от 0 до 65535\n";
        return EXIT_FAILURE;
    }

    if (!print_fds("до socket()"))
        return EXIT_FAILURE;

    const int fd = socket(AF_INET, type, 0);
    if (fd == -1)
        return fail(-1, "socket", errno);
    std::cout << "Сокет создан. Файловый дескриптор: " << fd << '\n';

    // Вариант 13: читаем тип из уже созданного сокета.
    int actual_type = 0;
    socklen_t type_size = sizeof(actual_type);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE,
                   &actual_type, &type_size) == -1)
        return fail(fd, "getsockopt", errno);
    std::cout << "getsockopt(SO_TYPE) = " << actual_type << " -> "
              << type_name(actual_type) << '\n';

    sockaddr_in address{};
    address.sin_family = AF_INET;
    const auto host_port = static_cast<std::uint16_t>(port);
    address.sin_port = htons(host_port);
    if (std::string_view(argv[3]) == "INADDR_ANY")
        address.sin_addr.s_addr = htonl(INADDR_ANY);
    else {
        const int result = inet_pton(AF_INET, argv[3], &address.sin_addr);
        if (result == -1)
            return fail(fd, "inet_pton", errno);
        if (result == 0) {
            std::cerr << "Ошибка: неверный IPv4-адрес " << argv[3]
                      << " (inet_pton вернула 0)\n";
            close_socket(fd);
            return EXIT_FAILURE;
        }
    }

    print_bytes("Байты порта в памяти хоста: ", host_port);
    print_bytes("Байты после htons():        ", address.sin_port);
    std::cout << "Порт без ntohs(): " << address.sin_port
              << ", после ntohs(): " << ntohs(address.sin_port) << '\n';

    if (do_bind) {
        const auto* local = reinterpret_cast<const sockaddr*>(&address);
        if (bind(fd, local, sizeof(address)) == -1) {
            const int bind_error = errno;
            if (bind_error == EADDRINUSE)
                std::cerr << "Ошибка: адрес и порт заняты (EADDRINUSE)\n";
            else if (bind_error == EACCES)
                std::cerr << "Ошибка: привязка запрещена (EACCES)\n";
            else if (bind_error == EADDRNOTAVAIL)
                std::cerr << "Ошибка: адрес недоступен (EADDRNOTAVAIL)\n";
            return fail(fd, "bind", bind_error);
        }
        // При порте 0 ядро назначает свободный порт.
        auto* bound = reinterpret_cast<sockaddr*>(&address);
        socklen_t address_size = sizeof(address);
        if (getsockname(fd, bound, &address_size) == -1)
            return fail(fd, "getsockname", errno);
    }

    char ip[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &address.sin_addr, ip, sizeof(ip)) == nullptr)
        return fail(fd, "inet_ntop", errno);
    if (do_bind)
        std::cout << "bind() выполнен. Локальный адрес: ";
    else
        std::cout << "Без bind(). Адрес из аргументов: ";
    std::cout << ip << ':' << ntohs(address.sin_port) << '\n';

    const bool listed = print_fds("перед close()");
    if (!close_socket(fd))
        return EXIT_FAILURE;
    std::cout << "Дескриптор " << fd << " освобождён.\n";
    const bool listed_after = print_fds("после close()");
    return listed && listed_after ? EXIT_SUCCESS : EXIT_FAILURE;
}
