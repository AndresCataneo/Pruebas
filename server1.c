#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <time.h>
#include <sys/stat.h>

#define BUFFER_SIZE 1024
#define BASE_PORT 49200  // Puerto base único

// Función para obtener timestamp
void get_timestamp(char *buffer, size_t size) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", t);
}

// Función para guardar archivo
void save_file(const char *server_name, const char *filename, const char *content) {
    char file_path[256];
    snprintf(file_path, sizeof(file_path), "/home/%s/%s", server_name, filename);
    
    FILE *file = fopen(file_path, "w");
    if (file) {
        fprintf(file, "%s", content);
        fclose(file);
    }
}

int main(int argc, char *argv[]) {
    // Verificar argumentos (nombres de servidores)
    if (argc < 2) {
        printf("Use: %s <s01> <s02> ...\n", argv[0]);
        return 1;
    }

    // Crear directorios para cada servidor
    for (int i = 1; i < argc; i++) {
        char dir_path[256];
        snprintf(dir_path, sizeof(dir_path), "/home/%s", argv[i]);
        mkdir(dir_path, 0755);
        printf("[+] Created directory: %s\n", dir_path);
    }

    // Crear socket principal para el puerto base
    int main_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (main_sock < 0) {
        perror("[-] Error creating socket");
        return 1;
    }

    int opt = 1;
    if (setsockopt(main_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEADDR failed");
        return 1;
    }
    
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(BASE_PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(main_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("[-] Error binding");
        close(main_sock);
        return 1;
    }

    if (listen(main_sock, 5) < 0) {
        perror("[-] Error on listen");
        close(main_sock);
        return 1;
    }

    printf("[*] LISTENING on base port %d...\n", BASE_PORT);
    printf("[+] Server started. Waiting for connections...\n");

    int dynamic_port_counter = 1;  // Contador para puertos dinámicos

    // Bucle infinito - servidor siempre escuchando
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_size = sizeof(client_addr);
        int client_sock = accept(main_sock, (struct sockaddr*)&client_addr, &addr_size);
        
        if (client_sock < 0) {
            perror("Accept error");
            continue;
        }

        // ASIGNAR PUERTO DINÁMICO (49200 + counter)
        int dynamic_port = BASE_PORT + dynamic_port_counter;
        dynamic_port_counter++;
        
        if (dynamic_port_counter > 100) {
            dynamic_port_counter = 1;  // Resetear después de 100 puertos
        }

        // Enviar puerto dinámico al cliente
        char port_msg[64];
        snprintf(port_msg, sizeof(port_msg), "DYNAMIC_PORT|%d", dynamic_port);
        send(client_sock, port_msg, strlen(port_msg), 0);
        close(client_sock);

        // Crear socket para puerto dinámico
        int dynamic_sock = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in dynamic_addr;
        
        dynamic_addr.sin_family = AF_INET;
        dynamic_addr.sin_port = htons(dynamic_port);
        dynamic_addr.sin_addr.s_addr = INADDR_ANY;
        
        // Configurar opciones del socket dinámico
        int dyn_opt = 1;
        setsockopt(dynamic_sock, SOL_SOCKET, SO_REUSEADDR, &dyn_opt, sizeof(dyn_opt));
        
        if (bind(dynamic_sock, (struct sockaddr*)&dynamic_addr, sizeof(dynamic_addr)) < 0) {
            perror("Bind error on dynamic port");
            close(dynamic_sock);
            continue;
        }
        
        if (listen(dynamic_sock, 1) < 0) {
            perror("Listen error on dynamic port");
            close(dynamic_sock);
            continue;
        }
        
        printf("[+] Assigned dynamic port %d to client\n", dynamic_port);
        
        int dynamic_client = accept(dynamic_sock, NULL, NULL);
        if (dynamic_client < 0) {
            perror("Accept error on dynamic port");
            close(dynamic_sock);
            continue;
        }
        
        char buffer[BUFFER_SIZE] = {0};
        char file_content[BUFFER_SIZE] = {0};
        int requested_port;
        char filename[256];

        int bytes = recv(dynamic_client, buffer, sizeof(buffer) - 1, 0);
        if (bytes > 0) {
            buffer[bytes] = '\0';
            
            // Parsear: PUERTO|NOMBRE_ARCHIVO|CONTENIDO
            if (sscanf(buffer, "%d|%255[^|]|%[^\n]", &requested_port, filename, file_content) == 3) {
                // Determinar servidor basado en el puerto solicitado o otro criterio
                char *server_name = (requested_port % 2 == 0) ? "s01" : "s02";
                
                if (requested_port == BASE_PORT) {
                    save_file(server_name, filename, file_content);
                    
                    char *msg = "File received successfully";
                    send(dynamic_client, msg, strlen(msg), 0);
                    printf("[SERVER %s] File %s saved\n", server_name, filename);
                } else {
                    char *msg = "REJECTED";
                    send(dynamic_client, msg, strlen(msg), 0);
                    printf("[SERVER] Request rejected - wrong port\n");
                }
            }
        }
        
        close(dynamic_client);
        close(dynamic_sock);
    }
    
    close(main_sock);
    return 0;
}
