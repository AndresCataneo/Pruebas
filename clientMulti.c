#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define BUFFER_SIZE 1024

int main(int argc, char *argv[]) {
    if (argc < 5) {
        printf("USE: %s <SERVER_IP> <PORT1> [PORT2] [PORT3] <FILE1> [FILE2] [FILE3] <SHIFT>\n", argv[0]);
        exit(1);
    }

    char *server_ip = argv[1];
    int num_ports = 0;
    int ports[3];

    // Leer puertos (hasta 3)
    for (int i = 2; i < argc && num_ports < 3; i++) {
        if (strspn(argv[i], "0123456789") == strlen(argv[i])) {
            ports[num_ports++] = atoi(argv[i]);
        } else break;
    }

    // Archivos (después de puertos, antes del shift)
    int num_files = argc - num_ports - 2;
    char **files = &argv[2 + num_ports];
    int shift = atoi(argv[argc - 1]);

    for (int i = 0; i < num_ports; i++) {
        if (i >= num_files) break; // no más archivos

        FILE *fp = fopen(files[i], "r");
        if (!fp) {
            perror("Error opening file");
            continue;
        }

        char file_content[BUFFER_SIZE] = {0};
        fread(file_content, 1, sizeof(file_content) - 1, fp);
        fclose(fp);

        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            perror("Socket creation failed");
            continue;
        }

        struct sockaddr_in serv_addr;
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(ports[i]);
        inet_pton(AF_INET, server_ip, &serv_addr.sin_addr);

        if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
            printf("Connection to server %d failed.\n", ports[i]);
            close(sock);
            continue;
        }

        char buffer[BUFFER_SIZE];
        snprintf(buffer, sizeof(buffer), "%d|%d|%s", ports[i], shift, file_content);
        send(sock, buffer, strlen(buffer), 0);

        char response[BUFFER_SIZE] = {0};
        int bytes = read(sock, response, sizeof(response) - 1);
        if (bytes > 0) {
            response[bytes] = '\0';
            printf("[*] SERVER RESPONSE %d: %s\n", ports[i], response);
        }
        close(sock);
    }
    return 0;
}
