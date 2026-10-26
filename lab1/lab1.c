#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* Подсказка по запуску */
static void usage(const char *prog)
{
    fprintf(stderr, "Использование: %s <domain> <type> <ip> <port>"
                    " [--bind]\n", prog);
    fprintf(stderr, "  domain: AF_INET\n");
    fprintf(stderr, "  type:   SOCK_STREAM | SOCK_DGRAM\n");
    fprintf(stderr, "  ip:     IPv4-адрес или INADDR_ANY\n");
    fprintf(stderr, "  port:   0..65535\n");
}

/* Ошибка системного вызова: печать, закрытие сокета, выход */
static int fail(int fd, const char *msg)
{
    perror(msg);
    if (fd >= 0)
        close(fd);
    return EXIT_FAILURE;
}

/* Перевод порта из строки, -1 при ошибке */
static long parse_port(const char *text)
{
    char *end;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0')
        return -1;
    if (value < 0 || value > 65535)
        return -1;
    return value;
}

/* Название типа сокета по числу */
static const char *type_name(int type)
{
    if (type == SOCK_STREAM)
        return "SOCK_STREAM (TCP)";
    if (type == SOCK_DGRAM)
        return "SOCK_DGRAM (UDP)";
    return "неизвестный тип";
}

/* Вывод открытых дескрипторов процесса из /proc/self/fd */
static void print_fds(const char *title)
{
    DIR *dir = opendir("/proc/self/fd");
    if (dir == NULL) {
        perror("opendir");
        return;
    }
    int dir_fd = dirfd(dir);
    printf("Открытые дескрипторы (%s):\n", title);

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.')
            continue;
        int fd = atoi(entry->d_name);
        if (fd == dir_fd)
            continue;

        char path[64];
        char target[128];
        snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
        ssize_t n = readlink(path, target, sizeof(target) - 1);
        if (n < 0)
            strcpy(target, "?");
        else
            target[n] = '\0';
        printf("  %d -> %s\n", fd, target);
    }
    closedir(dir);
}

int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Проверка аргументов командной строки */
    if (argc != 5 && argc != 6) {
        fprintf(stderr, "Ошибка: неверное число аргументов\n");
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    int do_bind = 0;
    if (argc == 6) {
        if (strcmp(argv[5], "--bind") != 0) {
            fprintf(stderr, "Ошибка: неизвестный флаг '%s'\n",
                    argv[5]);
            usage(argv[0]);
            return EXIT_FAILURE;
        }
        do_bind = 1;
    }

    if (strcmp(argv[1], "AF_INET") != 0) {
        fprintf(stderr, "Ошибка: неверный домен '%s'"
                        " (допустим AF_INET)\n", argv[1]);
        return EXIT_FAILURE;
    }

    int type;
    if (strcmp(argv[2], "SOCK_STREAM") == 0) {
        type = SOCK_STREAM;
    } else if (strcmp(argv[2], "SOCK_DGRAM") == 0) {
        type = SOCK_DGRAM;
    } else {
        fprintf(stderr, "Ошибка: неверный тип сокета '%s'\n",
                argv[2]);
        return EXIT_FAILURE;
    }

    long port = parse_port(argv[4]);
    if (port < 0) {
        fprintf(stderr, "Ошибка: неверный порт '%s'"
                        " (допустимо 0..65535)\n", argv[4]);
        return EXIT_FAILURE;
    }
    const char *ip = argv[3];

    /* Шаг 1. Создание сокета */
    print_fds("до socket()");
    int sockfd = socket(AF_INET, type, 0);
    if (sockfd < 0)
        return fail(-1, "socket");
    printf("\n[1] Сокет создан. Файловый дескриптор: %d\n", sockfd);

    /* Шаг 2. Тип сокета через getsockopt(SO_TYPE) */
    int real_type = 0;
    socklen_t len = sizeof(real_type);
    if (getsockopt(sockfd, SOL_SOCKET, SO_TYPE,
                   &real_type, &len) < 0)
        return fail(sockfd, "getsockopt");
    printf("[2] getsockopt(SO_TYPE) = %d -> %s\n",
           real_type, type_name(real_type));

    /* Шаг 3. Заполнение структуры адреса */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);

    if (strcmp(ip, "INADDR_ANY") == 0) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        int res = inet_pton(AF_INET, ip, &addr.sin_addr);
        if (res == 0) {
            fprintf(stderr, "Ошибка: неверный IP-адрес '%s'"
                            " (inet_pton вернула 0)\n", ip);
            close(sockfd);
            return EXIT_FAILURE;
        }
        if (res < 0)
            return fail(sockfd, "inet_pton");
    }
    printf("[3] Адрес заполнен: %s:%ld\n", ip, port);

    /* Шаг 4. Демонстрация сетевого порядка байт */
    unsigned short host_port = (unsigned short)port;
    unsigned char *hb = (unsigned char *)&host_port;
    unsigned char *nb = (unsigned char *)&addr.sin_port;
    printf("\n--- Порядок байт ---\n");
    printf("Порт %ld = 0x%04X\n", port, host_port);
    printf("Байты в памяти хоста:   %02X %02X\n", hb[0], hb[1]);
    printf("Байты после htons():    %02X %02X\n", nb[0], nb[1]);
    printf("Без ntohs() прочитали бы: %u\n", addr.sin_port);
    printf("С ntohs():                %u\n", ntohs(addr.sin_port));
    printf("IP (ntohl): 0x%08X\n", ntohl(addr.sin_addr.s_addr));

    /* Шаг 5. Обратное преобразование адреса в текст */
    char ip_text[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &addr.sin_addr, ip_text,
                  sizeof(ip_text)) == NULL)
        return fail(sockfd, "inet_ntop");
    printf("\n[4] Проверка адреса: %s:%u\n",
           ip_text, ntohs(addr.sin_port));

    /* Шаг 6. Привязка сокета (только с флагом --bind) */
    if (do_bind) {
        if (bind(sockfd, (struct sockaddr *)&addr,
                 sizeof(addr)) < 0) {
            if (errno == EADDRINUSE)
                fprintf(stderr, "Ошибка: порт %ld уже занят"
                                " (EADDRINUSE)\n", port);
            else if (errno == EACCES)
                fprintf(stderr, "Ошибка: нет прав на порт %ld"
                                " (EACCES)\n", port);
            else if (errno == EADDRNOTAVAIL)
                fprintf(stderr, "Ошибка: адрес %s не принадлежит"
                                " машине (EADDRNOTAVAIL)\n", ip_text);
            return fail(sockfd, "bind");
        }
        printf("[5] bind() выполнен: сокет привязан к %s:%ld\n",
               ip_text, port);
    } else {
        printf("[5] bind() не выполнялся (нет флага --bind)\n");
    }

    /* Шаг 7. Информация о сокете */
    printf("\n--- Информация о сокете ---\n");
    printf("Файловый дескриптор: %d\n", sockfd);
    printf("Семейство адресов:   AF_INET (%d)\n", addr.sin_family);
    printf("Тип сокета:          %s\n", type_name(real_type));
    printf("Адрес:               %s:%u\n\n",
           ip_text, ntohs(addr.sin_port));

    /* Шаг 8. Закрытие сокета */
    print_fds("перед close()");
    int res = close(sockfd);
    if (res < 0)
        return fail(-1, "close");
    printf("\n[6] close() вернул %d. Дескриптор %d освобождён.\n",
           res, sockfd);
    print_fds("после close()");
    return EXIT_SUCCESS;
}
