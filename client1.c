#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>

#define BUFFER_SIZE 1024
#define FILENAME_SIZE 256
#define PORT_MSG_SIZE 64
#define MAX_TOTAL_SIZE 2048

/*
    Función para guardar fecha, hora, estado, nombre de archivo y servidor
*/
void saveLog(const char *status, const char *filename, const char *server) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", t);
    
    FILE *log_file = fopen("clientLog.txt", "a");
    if (log_file) {
        fprintf(log_file, "%s | %s | %s | %s\n", timestamp, status, filename, server);
        fclose(log_file);
    }
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        printf("USE: %s <SERVER> <PORT> <FILE>\n", argv[0]);
        printf("Example: %s s01 49200 file1.txt\n", argv[0]);
        exit(1);
    }

    char *server_ip = argv[1];
    int port = atoi(argv[2]);
    char *filename = argv[3];

    // Leer archivo
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        perror("Error opening file");
        saveLog("ERROR", filename, "File not found");
        exit(1);
    }

    char file_content[BUFFER_SIZE] = {0};
    size_t bytes_read = fread(file_content, 1, sizeof(file_content) - 1, fp);
    file_content[bytes_read] = '\0';
    fclose(fp);

    // RESOLVER NOMBRE DE SERVIDOR
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(server_ip, NULL, &hints, &res) != 0) {
        perror("Error resolving hostname");
        saveLog("ERROR", filename, "Host resolution failed");
        exit(1);
    }

    // CONEXIÓN INICIAL al puerto estático
    int initial_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (initial_sock < 0) {
        perror("Socket creation failed");
        saveLog("ERROR", filename, "Socket creation failed");
        freeaddrinfo(res);
        exit(1);
    }
    
    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    
    // Usar la dirección IP resuelta
    struct sockaddr_in *ipv4 = (struct sockaddr_in *)res->ai_addr;
    serv_addr.sin_addr = ipv4->sin_addr;
    
    freeaddrinfo(res);  // Liberar memoria

    if (connect(initial_sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Connection failed");
        saveLog("ERROR", filename, "Connection failed");
        close(initial_sock);
        exit(1);
    }

    // Recibir puerto dinámico
    char port_response[PORT_MSG_SIZE] = {0};
    int bytes_received = recv(initial_sock, port_response, sizeof(port_response) - 1, 0);
    if (bytes_received <= 0) {
        perror("Error receiving port");
        close(initial_sock);
        exit(1);
    }
    port_response[bytes_received] = '\0';
    close(initial_sock);

    int dynamic_port;
    if (sscanf(port_response, "DYNAMIC_PORT|%d", &dynamic_port) == 1) {
        // CONEXIÓN al puerto dinámico (usar la misma IP resuelta)
        int dynamic_sock = socket(AF_INET, SOCK_STREAM, 0);
        serv_addr.sin_port = htons(dynamic_port);
        
        if (connect(dynamic_sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
            perror("Connection to dynamic port failed");
            saveLog("ERROR", filename, "Dynamic connection failed");
            exit(1);
        }

        // [El resto del código igual...]
        char buffer[MAX_TOTAL_SIZE];
        snprintf(buffer, sizeof(buffer), "%d|%s|%s", port, filename, file_content);

        if (send(dynamic_sock, buffer, strlen(buffer), 0) < 0) {
            perror("Send failed");
            saveLog("ERROR", filename, "Send failed");
            close(dynamic_sock);
            exit(1);
        }

        char response[BUFFER_SIZE] = {0};
        int bytes = recv(dynamic_sock, response, sizeof(response) - 1, 0);
        if (bytes > 0) {
            response[bytes] = '\0';
            printf("SERVER RESPONSE: %s\n", response);
            saveLog("SUCCESS", filename, server_ip);
        } else {
            printf("No response from server\n");
            saveLog("ERROR", filename, "No response");
        }
        
        close(dynamic_sock);
    }
    
    return 0;
}
