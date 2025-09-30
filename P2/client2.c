#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>

#define BUFFER_SIZE 1024
char *servers[] = {"s01", "s02", "s03", "s04"};

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

/*
    Cliente: conecta al servidor indicado, recibe un puerto dinámico,
    conecta a ese puerto y envía archivo SOLO a ese servidor.
*/
int main(int argc, char *argv[]) {
    if (argc != 4) {
        printf("USE: %s <SERVER_ALIAS> <PORT> <FILE>\n", argv[0]);
        printf("Example: %s s01 49200 file1.txt\n", argv[0]);
        exit(1);
    }

    char *server_alias = argv[1];
    int port = atoi(argv[2]);
    char *filename = argv[3];

    // Verificar alias válido
    int valid = 0;
    for (int i = 0; i < 4; i++) {
        if (strcmp(server_alias, servers[i]) == 0) {
            valid = 1;
            break;
        }
    }
    if (!valid) {
        fprintf(stderr, "ERROR: Invalid server alias '%s'. Use one of: s01, s02, s03, s04\n", server_alias);
        exit(1);
    }

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

    // Resolver dirección (localhost)
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    serv_addr.sin_addr.s_addr = inet_addr("127.0.0.1");  // asume que el server corre localmente

    // Conexión inicial al dispatcher
    int client_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (client_sock < 0) {
        perror("Socket creation failed");
        exit(1);
    }

    if (connect(client_sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Connection to dispatcher failed");
        close(client_sock);
        exit(1);
    }

    // Recibimos el puerto dinámico
    char port_response[64] = {0};
    int bytes_received = recv(client_sock, port_response, sizeof(port_response) - 1, 0);
    if (bytes_received <= 0) {
        perror("Error receiving dynamic port");
        close(client_sock);
        exit(1);
    }
    port_response[bytes_received] = '\0';
    close(client_sock);

    int dynamic_port;
    if (sscanf(port_response, "DYNAMIC_PORT|%d", &dynamic_port) != 1) {
        fprintf(stderr, "Invalid response from dispatcher: %s\n", port_response);
        exit(1);
    }

    // Conectar al puerto dinámico
    int dynamic_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (dynamic_sock < 0) {
        perror("Socket creation failed (dynamic)");
        exit(1);
    }
    serv_addr.sin_port = htons(dynamic_port);

    if (connect(dynamic_sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Connection to dynamic port failed");
        close(dynamic_sock);
        exit(1);
    }

    // Armar buffer: alias|filename|contenido
    char buffer[BUFFER_SIZE * 2];
    snprintf(buffer, sizeof(buffer), "%s|%s|%s", server_alias, filename, file_content);

    if (send(dynamic_sock, buffer, strlen(buffer), 0) < 0) {
        perror("Send failed");
        close(dynamic_sock);
        exit(1);
    }

    // Respuesta del servidor
    char response[BUFFER_SIZE] = {0};
    int bytes = recv(dynamic_sock, response, sizeof(response) - 1, 0);
    if (bytes > 0) {
        response[bytes] = '\0';
        printf("SERVER RESPONSE from %s: %s\n", server_alias, response);
        saveLog("SUCCESS", filename, server_alias);
    } else {
        printf("No response from server\n");
        saveLog("ERROR", filename, server_alias);
    }

    close(dynamic_sock);
    return 0;
}
