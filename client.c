#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define BUFFER_SIZE 4096
int servers[] = {49200, 49201, 49202};

int main(int argc, char *argv[]) {
    if (argc != 5) {
        printf("USE: %s <SERVER_IP> <PORT> <SHIFT> <FILENAME>\n", argv[0]);
        exit(1);
    }

    char *server_ip = argv[1];
    char *target_port = argv[2]; 
    char *shift = argv[3];       
    char *filename = argv[4];

    // Leer archivo
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        perror("Error opening file");
        exit(1);
    }
    char file_content[BUFFER_SIZE] = {0};
    fread(file_content, 1, sizeof(file_content) - 1, fp);
    fclose(fp);

    // Conectarse a los 3 servidores
    for (int i = 0; i < 3; i++) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            perror("Socket creation failed");
            continue;
        }

        struct sockaddr_in serv_addr;
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(servers[i]);
        inet_pton(AF_INET, server_ip, &serv_addr.sin_addr);

        if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        printf("Connection to server %d failed.\n", servers[i]);
        close(sock);
        continue;
    }

        // Enviar datos
        char buffer[BUFFER_SIZE];
        snprintf(buffer, sizeof(buffer), "%s|%s|%s", target_port, shift, file_content);
        send(sock, buffer, strlen(buffer), 0);
    
        // Recibir respuesta
        char response[BUFFER_SIZE] = {0};
        int bytes = read(sock, response, sizeof(response) - 1);
        if (bytes > 0) {
            response[bytes] = '\0';
            printf("[*] SERVER RESPONSE %d: %s\n", servers[i], response);
        } else {
            printf("[*] SERVER RESPONSE %d: (no response)\n", servers[i]);
        }
    
        close(sock);
        }

    return 0;
}
