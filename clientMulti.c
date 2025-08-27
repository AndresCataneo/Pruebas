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

    // Parsear argumentos - encontrar dónde terminan los puertos
    int i;
    for (i = 2; i < argc; i++) {
        if (is_number(argv[i]) && num_ports < 3) {
            ports[num_ports++] = atoi(argv[i]);
        } else {
            break;
        }
    }

    // El último argumento debe ser el shift
    if (i < argc && is_number(argv[argc - 1])) {
        shift = atoi(argv[argc - 1]);
        // Los archivos están entre los puertos y el shift
        num_files = argc - 1 - i;
    } else {
        printf("Error: Shift value missing or invalid\n");
        exit(1);
    }

    if (num_ports == 0 || num_files == 0 || shift == 0) {
        printf("Error: Invalid arguments\n");
        printf("Ports found: %d, Files found: %d, Shift: %d\n", num_ports, num_files, shift);
        printf("USE: %s <SERVER_IP> <PORT1> [PORT2] [PORT3] <FILE1> [FILE2] [FILE3] <SHIFT>\n", argv[0]);
        exit(1);
    }

    char **files = &argv[i];
    
    printf("Connecting to server %s\n", server_ip);
    printf("Ports: ");
    for (int j = 0; j < num_ports; j++) printf("%d ", ports[j]);
    printf("\nFiles: ");
    for (int j = 0; j < num_files; j++) printf("%s ", files[j]);
    printf("\nShift: %d\n", shift);

    // Procesar cada puerto/archivo
    for (int j = 0; j < num_ports && j < num_files; j++) {
        FILE *fp = fopen(files[j], "r");
        if (!fp) {
            perror("Error opening file");
            continue;
        }

        char file_content[BUFFER_SIZE] = {0};
        size_t bytes_read = fread(file_content, 1, sizeof(file_content) - 1, fp);
        file_content[bytes_read] = '\0';
        fclose(fp);

        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            perror("Socket creation failed");
            continue;
        }

        struct sockaddr_in serv_addr;
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(ports[j]);
        inet_pton(AF_INET, server_ip, &serv_addr.sin_addr);

        printf("Connecting to port %d...\n", ports[j]);
        if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
            perror("Connection failed");
            close(sock);
            continue;
        }

        char buffer[BUFFER_SIZE];
        snprintf(buffer, sizeof(buffer), "%d|%d|%s", ports[j], shift, file_content);
        
        if (send(sock, buffer, strlen(buffer), 0) < 0) {
            perror("Send failed");
            close(sock);
            continue;
        }

        char response[BUFFER_SIZE] = {0};
        int bytes = recv(sock, response, sizeof(response) - 1, 0);
        if (bytes > 0) {
            response[bytes] = '\0';
            printf("[PORT %d] SERVER RESPONSE: %s\n", ports[j], response);
        } else {
            printf("[PORT %d] No response from server\n", ports[j]);
        }
        
        close(sock);
    }
    
    return 0;
}
