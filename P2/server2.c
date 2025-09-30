#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>

#define BUFFER_SIZE 1024
#define server_port 49200
#define QUANTUM_TIME 15

// Estructura para memoria compartida
typedef struct {
    int current_server;
    int receiving_server;
    bool server_busy;
    time_t turn_start_time;
    pthread_mutex_t mutex;
    pthread_cond_t turn_cond;
} shared_memory_t;

// Estructura para cola de conexiones
typedef struct connection_node {
    int dynamic_client;
    int dynamic_sock;
    char target_server[32];
    struct connection_node* next;
} connection_node_t;

shared_memory_t *shared_mem;
char *server_names[4];
connection_node_t* connection_queues[4] = {NULL};
pthread_mutex_t queue_mutexes[4];

void saveFile(const char *server_name, const char *filename, const char *content) {
    char file_path[256];
    char *home_dir = getenv("HOME");
    if (home_dir == NULL) home_dir = "/home";
    
    char dir_path[256];
    snprintf(dir_path, sizeof(dir_path), "%s/%s", home_dir, server_name);
    mkdir(dir_path, 0755);
    
    snprintf(file_path, sizeof(file_path), "%s/%s/%s", home_dir, server_name, filename);
    FILE *file = fopen(file_path, "w");
    if (file) {
        fprintf(file, "%s", content);
        fclose(file);
    }
}

bool is_quantum_expired(time_t start_time) {
    return (time(NULL) - start_time) >= QUANTUM_TIME;
}

// Agregar conexión a la cola del servidor correspondiente
void add_to_queue(const char* target_server, int dynamic_client, int dynamic_sock) {
    connection_node_t* new_node = malloc(sizeof(connection_node_t));
    new_node->dynamic_client = dynamic_client;
    new_node->dynamic_sock = dynamic_sock;
    strncpy(new_node->target_server, target_server, sizeof(new_node->target_server) - 1);
    new_node->target_server[sizeof(new_node->target_server) - 1] = '\0';
    new_node->next = NULL;
    
    int server_index = -1;
    for (int i = 0; i < 4; i++) {
        if (strcmp(target_server, server_names[i]) == 0) {
            server_index = i;
            break;
        }
    }
    
    if (server_index == -1) {
        free(new_node);
        return;
    }
    
    pthread_mutex_lock(&queue_mutexes[server_index]);
    
    if (connection_queues[server_index] == NULL) {
        connection_queues[server_index] = new_node;
    } else {
        connection_node_t* current = connection_queues[server_index];
        while (current->next != NULL) {
            current = current->next;
        }
        current->next = new_node;
    }
    
    pthread_mutex_unlock(&queue_mutexes[server_index]);
}

// Obtener siguiente conexión de la cola
connection_node_t* get_next_connection(int server_index) {
    pthread_mutex_lock(&queue_mutexes[server_index]);
    
    if (connection_queues[server_index] == NULL) {
        pthread_mutex_unlock(&queue_mutexes[server_index]);
        return NULL;
    }
    
    connection_node_t* node = connection_queues[server_index];
    connection_queues[server_index] = node->next;
    
    pthread_mutex_unlock(&queue_mutexes[server_index]);
    return node;
}

// Procesar una conexión individual
void process_connection(int dynamic_client, int dynamic_sock, const char* target_server) {
    char buffer[BUFFER_SIZE] = {0};
    char file_content[BUFFER_SIZE] = {0};
    char filename[256];

    int bytes = recv(dynamic_client, buffer, sizeof(buffer) - 1, 0);
    if (bytes > 0) {
        buffer[bytes] = '\0';
        
        char alias[32];
        if (sscanf(buffer, "%31[^|]|%255[^|]|%[^\n]", alias, filename, file_content) == 3) {
            if (strcmp(alias, target_server) == 0) {
                saveFile(alias, filename, file_content);
                char *msg = "File received successfully";
                send(dynamic_client, msg, strlen(msg), 0);
                printf("[SERVER %s] File %s received\n", alias, filename);
            } else {
                char *msg = "REJECTED - Wrong server";
                send(dynamic_client, msg, strlen(msg), 0);
                printf("[SERVER %s] Rejected file for %s\n", target_server, alias);
            }
        } else {
            char *msg = "REJECTED";
            send(dynamic_client, msg, strlen(msg), 0);
        }
    }
    
    close(dynamic_client);
    close(dynamic_sock);
}

// Hilo principal de cada servidor
void* server_thread(void* arg) {
    int server_index = *(int*)arg;
    free(arg);
    bool first_waiting_printed = false;
    
    while (1) {
        // ESPERAR TURNO
        pthread_mutex_lock(&shared_mem->mutex);
        
        while (shared_mem->current_server != server_index || shared_mem->server_busy) {
            if (!first_waiting_printed) {
                printf("[SERVER %s] Waiting for turn (current: %s)\n", 
                       server_names[server_index], server_names[shared_mem->current_server]);
                first_waiting_printed = true;
            }
            pthread_cond_wait(&shared_mem->turn_cond, &shared_mem->mutex);
        }
        
        // INICIAR TURNO
        shared_mem->server_busy = true;
        shared_mem->receiving_server = server_index;
        shared_mem->turn_start_time = time(NULL);
        
        printf("\n[SERVER %s] Starting turn\n", server_names[server_index]);
        pthread_mutex_unlock(&shared_mem->mutex);
        
        // PROCESAR ARCHIVOS DURANTE EL QUANTUM COMPLETO
        time_t start_time = time(NULL);
        bool processed_any = false;
        int files_processed = 0;
        
        while (!is_quantum_expired(start_time)) {
            connection_node_t* connection = get_next_connection(server_index);
            
            if (connection != NULL) {
                processed_any = true;
                files_processed++;
                printf("[SERVER %s] Processing file...\n", server_names[server_index]);
                process_connection(connection->dynamic_client, connection->dynamic_sock, server_names[server_index]);
                free(connection);
                
                // ✅ NO salir después de procesar, seguir en el loop
                // para procesar más archivos o esperar el tiempo restante
            } else {
                // No hay archivos en la cola
                if (processed_any) {
                    // Ya procesó algunos archivos, esperar tiempo restante
                    time_t remaining = QUANTUM_TIME - (time(NULL) - start_time);
                    if (remaining > 0) {
                        printf("[SERVER %s] %d files processed, waiting %ld seconds...\n", 
                               server_names[server_index], files_processed, remaining);
                        sleep(remaining);
                    }
                    break;
                } else {
                    // No ha procesado nada aún, esperar un poco y verificar de nuevo
                    sleep(1);
                    // El loop continuará verificando hasta que expire el quantum
                }
            }
        }
        
        // Mensaje final del turno
        time_t time_used = time(NULL) - start_time;
        if (processed_any) {
            printf("[SERVER %s] Quantum completed - %d files processed in %ld seconds\n", 
                   server_names[server_index], files_processed, time_used);
        } else {
            printf("[SERVER %s] Quantum expired with no files to process\n", 
                   server_names[server_index]);
        }
        
        // CEDER TURNO
        pthread_mutex_lock(&shared_mem->mutex);
        
        shared_mem->server_busy = false;
        shared_mem->receiving_server = -1;
        shared_mem->current_server = (shared_mem->current_server + 1) % 4;
        
        printf("[SERVER %s] Turn finished\n", server_names[server_index]);
        
        pthread_cond_broadcast(&shared_mem->turn_cond);
        pthread_mutex_unlock(&shared_mem->mutex);
    }
    
    return NULL;
}

// Hilo para leer el archivo y determinar el servidor target
void* connection_handler(void* arg) {
    int* sockets = (int*)arg;
    int dynamic_client = sockets[0];
    int dynamic_sock = sockets[1];
    free(arg);
    
    char buffer[BUFFER_SIZE] = {0};
    
    int bytes = recv(dynamic_client, buffer, sizeof(buffer) - 1, MSG_PEEK);
    if (bytes > 0) {
        buffer[bytes] = '\0';
        
        char alias[32];
        char filename[256];
        char content[BUFFER_SIZE];
        
        if (sscanf(buffer, "%31[^|]|%255[^|]|%[^\n]", alias, filename, content) == 3) {
            add_to_queue(alias, dynamic_client, dynamic_sock);
        } else {
            close(dynamic_client);
            close(dynamic_sock);
        }
    } else {
        close(dynamic_client);
        close(dynamic_sock);
    }
    
    return NULL;
}

// Hilo que maneja la expiración del quantum (MODIFICADO)
void* quantum_manager(void* arg) {
    while (1) {
        sleep(5);  // Verificar cada 5 segundos en lugar de 1
        
        pthread_mutex_lock(&shared_mem->mutex);
        
        // ✅ SOLO cambiar turno si ningún servidor está recibiendo Y el quantum expiró
        // Esto evita cambios automáticos durante el procesamiento
        if (!shared_mem->server_busy && is_quantum_expired(shared_mem->turn_start_time)) {
            shared_mem->current_server = (shared_mem->current_server + 1) % 4;
            shared_mem->turn_start_time = time(NULL);
            
            printf("[QUANTUM] Switching to server: %s\n", server_names[shared_mem->current_server]);
            
            pthread_cond_broadcast(&shared_mem->turn_cond);
        }
        
        pthread_mutex_unlock(&shared_mem->mutex);
    }
    return NULL;
}

/*
    Función principal
*/
int main(int argc, char *argv[]) {
    int port_s;
    int client_port; 
    struct sockaddr_in server_addr;
    int port_counter = 1;

    if (argc < 5) { 
        printf("Use: %s <s01> <s02> <s03> <s04>\n", argv[0]);
        return 1;
    }

    for (int i = 0; i < 4; i++) {
        server_names[i] = argv[i + 1];
        pthread_mutex_init(&queue_mutexes[i], NULL);
    }

    port_s = socket(AF_INET, SOCK_STREAM, 0);
    if (port_s < 0) {
        perror("[-] Error creating socket");
        return 1;
    }

    int opt = 1;
    setsockopt(port_s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(port_s, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("[-] Error binding");
        close(port_s);
        return 1;
    }

    if (listen(port_s, 10) < 0) {
        perror("[-] Error on listen");
        close(port_s);
        return 1;
    }
    
    printf("[*] Round Robin initialized (quantum: %ds)\n", QUANTUM_TIME);
    printf("[*] Turn order: %s -> %s -> %s -> %s\n", server_names[0], server_names[1], server_names[2], server_names[3]);
    printf("[*] LISTENING on port %d...\n\n", server_port);

    shared_mem = mmap(NULL, sizeof(shared_memory_t), PROT_READ | PROT_WRITE, 
                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    shared_mem->current_server = 0;
    shared_mem->receiving_server = -1;
    shared_mem->server_busy = false;
    shared_mem->turn_start_time = time(NULL);
    pthread_mutex_init(&shared_mem->mutex, NULL);
    pthread_cond_init(&shared_mem->turn_cond, NULL);

    pthread_t server_threads[4];
    for (int i = 0; i < 4; i++) {
        int* server_index = malloc(sizeof(int));
        *server_index = i;
        pthread_create(&server_threads[i], NULL, server_thread, server_index);
        pthread_detach(server_threads[i]);
    }

    pthread_t quantum_thread;
    pthread_create(&quantum_thread, NULL, quantum_manager, NULL);
    pthread_detach(quantum_thread);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_size = sizeof(client_addr);
        client_port = accept(port_s, (struct sockaddr*)&client_addr, &addr_size);
        
        if (client_port < 0) continue;

        int dynamic_port = server_port + port_counter++;
        char port_msg[64];
        snprintf(port_msg, sizeof(port_msg), "DYNAMIC_PORT|%d", dynamic_port);
        send(client_port, port_msg, strlen(port_msg), 0);
        close(client_port);

        int dynamic_sock = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in dynamic_addr;
        dynamic_addr.sin_family = AF_INET;
        dynamic_addr.sin_port = htons(dynamic_port);
        dynamic_addr.sin_addr.s_addr = INADDR_ANY;
        
        setsockopt(dynamic_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        bind(dynamic_sock, (struct sockaddr*)&dynamic_addr, sizeof(dynamic_addr));
        listen(dynamic_sock, 1);
        
        int dynamic_client = accept(dynamic_sock, NULL, NULL);
        if (dynamic_client < 0) {
            close(dynamic_sock);
            continue;
        }
        
        printf("[*] Client connected (port %d)\n", dynamic_port);

        int* sockets = malloc(2 * sizeof(int));
        sockets[0] = dynamic_client;
        sockets[1] = dynamic_sock;
        
        pthread_t handler_thread;
        pthread_create(&handler_thread, NULL, connection_handler, sockets);
        pthread_detach(handler_thread);
    }
    
    close(port_s);
    return 0;
}
