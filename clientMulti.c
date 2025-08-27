#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <ctype.h>

#define BUFFER_SIZE 1024

int is_number(const char *str) {
    for (int i = 0; str[i] != '\0'; i++) {
        if (!isdigit(str[i])) return 0;
    }
    return 1;
}

int main(int argc, char *argv[]) {
    if (argc < 5) {
        printf("USE: %s <SERVER_IP> <PORT1> [PORT2] [PORT3] <FILE1> [FILE2] [FILE3] <SHIFT>\n", argv[0]);
        printf("Example: %s 192.168.1.71 49200 49201 49202 file1.txt file2.txt file3.txt 34\n", argv[0]);
        exit(1);
    }

    char *server_ip = argv[1];
    int ports[3] = {0};
    int num_ports = 0;
    int num_files = 0;
    int shift = 0;

    // Parsear argumentos
    for (int i = 2; i < argc; i++) {
        if (is_number(argv[i]) && num_ports < 3) {
            ports[num_ports++] = atoi(argv[i]);
        } else {
            // El último argumento debe ser el shift
            if (i == argc - 1 && is_number(argv[i])) {
                shift = atoi(argv[i]);
                break;
            }
            // Los argumentos restantes son archivos
            num_files = argc - i - 1;
            break;
        }
    }

    if (num_ports == 0 || num_files == 0 || shift == 0) {
        printf("Error: Invalid arguments\n");
        printf("USE: %s <SERVER_IP> <PORT1> [PORT2] [PORT3] <FILE1> [FILE2] [FILE3] <SHIFT>\n", argv[0]);
        exit(1);
    }

    char **files = &argv[2 + num_ports];
    
    printf("Connecting to server %s\n", server_ip);
    printf("Ports: ");
    for (int i = 0; i < num_ports; i++) printf("%d ", ports[i]);
    printf("\nFiles: ");
    for (int i = 0; i < num_files; i++) printf("%s ", files[i]);
    printf("\nShift: %d\n", shift);

    // Procesar cada puerto/archivo
    for (int i = 0; i < num_ports && i < num_files; i++) {
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

        printf("Connecting to port %d...\n", ports[i]);
        if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
            perror("Connection failed");
            close(sock);
            continue;
        }

        char buffer[BUFFER_SIZE];
        snprintf(buffer, sizeof(buffer), "%d|%d|%s", ports[i], shift, file_content);
        
        if (send(sock, buffer, strlen(buffer), 0) < 0) {
            perror("Send failed");
            close(sock);
            continue;
        }

        char response[BUFFER_SIZE] = {0};
        int bytes = recv(sock, response, sizeof(response) - 1, 0);
        if (bytes > 0) {
            response[bytes] = '\0';
            printf("[PORT %d] SERVER RESPONSE: %s\n", ports[i], response);
        } else {
            printf("[PORT %d] No response from server\n", ports[i]);
        }
        
        close(sock);
    }
    
    return 0;
}
