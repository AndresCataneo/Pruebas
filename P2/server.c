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
#define server_port 49200  // Puerto base 
#define QUANTUM_TIME 15    // 15 segundos por turno

// Estructura para memoria compartida entre hilos
typedef struct {
    int current_server;      // Índice del servidor actual en turno (0-3)
    int receiving_server;    // Índice del servidor que está recibiendo (-1 si ninguno)
    bool server_busy;        // Indica si algún servidor está recibiendo
    time_t turn_start_time;  // Tiempo cuando comenzó el turno actual
    bool quantum_expired;    // Indica si el quantum expiró
    pthread_mutex_t mutex;
    pthread_cond_t turn_cond;
} shared_memory_t;

// Estructura para datos de conexión
typedef struct {
    int dynamic_client;
    int dynamic_sock;
    char* server_name;
    int server_index;
} connection_data_t;

shared_memory_t *shared_mem;
char *server_names[4];

/*
    Función para guardar archivo en el directorio del servidor
*/
void saveFile(const char *server_name, const char *filename, const char *content) {
    char file_path[256];
    char *home_dir = getenv("HOME");
    if (home_dir == NULL) {
        home_dir = "/home";
        printf("Warning: HOME environment variable not set, using %s\n", home_dir);
    }
    
    snprintf(file_path, sizeof(file_path), "%s/%s/%s", home_dir, server_name, filename);
    FILE *file = fopen(file_path, "w");
    if (file) {
        fprintf(file, "%s", content);
        fclose(file);
    }
}

/*
    Verifica si el tiempo de quantum ha expirado
*/
bool is_quantum_expired(time_t start_time) {
    return (time(NULL) - start_time) >= QUANTUM_TIME;
}

/*
    Función que maneja la recepción de archivos con control de turnos
    Esta función se ejecuta en un hilo separado para cada conexión
*/
void* handle_connection_with_turn(void* arg) {
    connection_data_t* conn_data = (connection_data_t*)arg;
    int dynamic_client = conn_data->dynamic_client;
    int dynamic_sock = conn_data->dynamic_sock;
    char* server_name = conn_data->server_name;
    int server_index = conn_data->server_index;
    
    char buffer[BUFFER_SIZE] = {0};
    char file_content[BUFFER_SIZE] = {0};
    char filename[256];

    // === REQUISITO 1: Esperar turno usando Round Robin ===
    pthread_mutex_lock(&shared_mem->mutex);
    
    // Esperar hasta que sea nuestro turno y ningún servidor esté recibiendo
    while (shared_mem->current_server != server_index || shared_mem->server_busy) {
        printf("[SERVER %s] Waiting for turn (current: %s, busy: %d)\n", 
               server_name, server_names[shared_mem->current_server], 
               shared_mem->server_busy);
        pthread_cond_wait(&shared_mem->turn_cond, &shared_mem->mutex);
        
        // Verificar si el turno cambió mientras esperábamos
        if (shared_mem->current_server != server_index) {
            pthread_mutex_unlock(&shared_mem->mutex);
            printf("[SERVER %s] Turn skipped - no longer our turn\n", server_name);
            close(dynamic_client);
            close(dynamic_sock);
            free(conn_data);
            return NULL;
        }
    }
    
    // === REQUISITO 2: Anunciar que estamos recibiendo ===
    shared_mem->server_busy = true;
    shared_mem->receiving_server = server_index;
    shared_mem->turn_start_time = time(NULL);
    shared_mem->quantum_expired = false;
    
    printf("[SERVER %s] Starting turn for %d seconds\n", server_name, QUANTUM_TIME);
    pthread_mutex_unlock(&shared_mem->mutex);
    
    // === REQUISITO 3: Recibir y procesar el archivo COMPLETAMENTE ===
    int bytes = recv(dynamic_client, buffer, sizeof(buffer) - 1, 0);
    if (bytes > 0) {
        buffer[bytes] = '\0';
        
        char alias[32];
        if (sscanf(buffer, "%31[^|]|%255[^|]|%[^\n]", alias, filename, file_content) == 3) {
            // Verificar si el archivo es para este servidor
            if (strcmp(alias, server_name) == 0) {
                // === REQUISITO 6: Recibir todo el archivo sin importar el tiempo ===
                saveFile(alias, filename, file_content);
                
                char *msg = "File received successfully";
                send(dynamic_client, msg, strlen(msg), 0);
                printf("[SERVER %s] File %s saved COMPLETELY\n", alias, filename);
            } else {
                // El archivo no es para este servidor
                char *msg = "REJECTED - Wrong server";
                send(dynamic_client, msg, strlen(msg), 0);
                printf("[SERVER %s] Rejected file for %s\n", server_name, alias);
            }
        } else {
            char *msg = "REJECTED";
            send(dynamic_client, msg, strlen(msg), 0);
            printf("[SERVER %s] Request rejected - invalid format\n", server_name);
        }
    }
    
    close(dynamic_client);
    close(dynamic_sock);
    
    // === REQUISITO 4: Ceder turno al siguiente servidor ===
    pthread_mutex_lock(&shared_mem->mutex);
    
    shared_mem->server_busy = false;
    shared_mem->receiving_server = -1;
    
    // Verificar si el quantum expiró durante la recepción
    time_t current_time = time(NULL);
    time_t time_used = current_time - shared_mem->turn_start_time;
    
    if (time_used >= QUANTUM_TIME) {
        // === REQUISITO 5: Si se acabó el tiempo, ceder turno ===
        shared_mem->current_server = (shared_mem->current_server + 1) % 4;
        printf("[SERVER %s] Quantum expired after %ld seconds, next server: %s\n", 
               server_name, time_used, server_names[shared_mem->current_server]);
    } else {
        // El servidor mantiene el turno para recibir más archivos
        printf("[SERVER %s] File completed with %ld seconds remaining\n", 
               server_name, QUANTUM_TIME - time_used);
    }
    
    // === REQUISITO 2: Notificar a todos los servidores ===
    pthread_cond_broadcast(&shared_mem->turn_cond);
    pthread_mutex_unlock(&shared_mem->mutex);
    
    free(conn_data);
    return NULL;
}

/*
    Hilo que maneja la expiración del quantum
*/
void* quantum_manager(void* arg) {
    while (1) {
        sleep(1);  // Verificar cada segundo
        
        pthread_mutex_lock(&shared_mem->mutex);
        
        // Si ningún servidor está recibiendo y el quantum expiró, cambiar turno
        if (!shared_mem->server_busy && is_quantum_expired(shared_mem->turn_start_time)) {
            shared_mem->current_server = (shared_mem->current_server + 1) % 4;
            shared_mem->turn_start_time = time(NULL);
            
            printf("[QUANTUM MANAGER] Quantum expired, switching to server: %s\n", 
                   server_names[shared_mem->current_server]);
            
            pthread_cond_broadcast(&shared_mem->turn_cond);
        }
        
        pthread_mutex_unlock(&shared_mem->mutex);
    }
    return NULL;
}

/*
    Función principal - MANTIENE LA ESTRUCTURA ORIGINAL
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

    // Guardar nombres de servidores
    for (int i = 0; i < 4; i++) {
        server_names[i] = argv[i + 1];
    }

    // === INICIALIZAR MEMORIA COMPARTIDA ===
    shared_mem = mmap(NULL, sizeof(shared_memory_t), PROT_READ | PROT_WRITE, 
                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    
    if (shared_mem == MAP_FAILED) {
        perror("Error creating shared memory");
        return 1;
    }
    
    // Inicializar memoria compartida
    shared_mem->current_server = 0;
    shared_mem->receiving_server = -1;
    shared_mem->server_busy = false;
    shared_mem->quantum_expired = false;
    shared_mem->turn_start_time = time(NULL);
    pthread_mutex_init(&shared_mem->mutex, NULL);
    pthread_cond_init(&shared_mem->turn_cond, NULL);
    
    printf("[*] Round Robin coordination initialized (quantum: %d seconds)\n", QUANTUM_TIME);
    printf("[*] Turn order: %s -> %s -> %s -> %s\n", 
           server_names[0], server_names[1], server_names[2], server_names[3]);

    // Iniciar el administrador de quantum
    pthread_t quantum_thread;
    if (pthread_create(&quantum_thread, NULL, quantum_manager, NULL) != 0) {
        perror("Error creating quantum manager thread");
        return 1;
    }
    pthread_detach(quantum_thread);

    // === ESTRUCTURA ORIGINAL DEL MAIN (PUERTO BASE + PUERTOS DINÁMICOS) ===
    
    // Creamos el socket principal para el puerto base
    port_s = socket(AF_INET, SOCK_STREAM, 0);
    if (port_s < 0) {
        perror("[-] Error creating socket");
        return 1;
    }

    int opt = 1;
    // Permitimos que se vuelva a usar el puerto después de terminar la ejecución del programa
    if (setsockopt(port_s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEADDR failed");
        return 1;
    }
    
    // Configuramos la dirección del servidor
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    // Asignamos el socket a la dirección y puerto especificados
    if (bind(port_s, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("[-] Error binding");
        close(port_s);
        return 1;
    }

    // Escuchamos conexiones entrantes
    if (listen(port_s, 10) < 0) {
        perror("[-] Error on listen");
        close(port_s);
        return 1;
    }

    printf("[*] LISTENING on port %d...\n", server_port);
    printf("[*] Server started. Waiting for connections...\n");

    // Contador para asignar servidores a conexiones (Round Robin)
    int connection_counter = 0;

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_size = sizeof(client_addr);
        client_port = accept(port_s, (struct sockaddr*)&client_addr, &addr_size);
        
        if (client_port < 0) {
            perror("Accept error");
            continue;
        }

        // Asignamos un puerto dinámico al cliente mayor al puerto base
        int dynamic_port = server_port + port_counter;
        port_counter++;

        // Enviamos el puerto dinámico al cliente
        char port_msg[64];
        snprintf(port_msg, sizeof(port_msg), "DYNAMIC_PORT|%d", dynamic_port);
        send(client_port, port_msg, strlen(port_msg), 0);
        close(client_port);

        // Creamos el socket para el puerto dinámico
        int dynamic_sock = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in dynamic_addr;
        
        dynamic_addr.sin_family = AF_INET;
        dynamic_addr.sin_port = htons(dynamic_port);
        dynamic_addr.sin_addr.s_addr = INADDR_ANY;
        
        // Permitimos que se vuelva a usar el puerto después de terminar la ejecución del programa
        int dyn_opt = 1;
        setsockopt(dynamic_sock, SOL_SOCKET, SO_REUSEADDR, &dyn_opt, sizeof(dyn_opt));
        
        // Asignamos el socket a la dirección y puerto especificados
        if (bind(dynamic_sock, (struct sockaddr*)&dynamic_addr, sizeof(dynamic_addr)) < 0) {
            perror("Bind error on dynamic port");
            close(dynamic_sock);
            continue;
        }
        
        // Escuchamos conexiones entrantes
        if (listen(dynamic_sock, 1) < 0) {
            perror("Listen error on dynamic port");
            close(dynamic_sock);
            continue;
        }
        
        printf("[*] Assigned dynamic port %d to client\n", dynamic_port);

        // Aceptamos la conexión del cliente en el puerto dinámico
        int dynamic_client = accept(dynamic_sock, NULL, NULL);
        if (dynamic_client < 0) {
            perror("Accept error on dynamic port");
            close(dynamic_sock);
            continue;
        }
        
        // === MODIFICACIÓN: Crear hilo para manejar la conexión con control de turnos ===
        connection_data_t* conn_data = malloc(sizeof(connection_data_t));
        conn_data->dynamic_client = dynamic_client;
        conn_data->dynamic_sock = dynamic_sock;
        conn_data->server_index = connection_counter % 4;  // Asignación Round Robin
        conn_data->server_name = server_names[conn_data->server_index];
        
        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, handle_connection_with_turn, conn_data) != 0) {
            perror("Error creating thread");
            close(dynamic_client);
            close(dynamic_sock);
            free(conn_data);
            continue;
        }
        
        pthread_detach(thread_id);  // El hilo se libera automáticamente al terminar
        connection_counter++;
        
        printf("[*] Created thread for connection assigned to %s\n", conn_data->server_name);
    }
    
    close(port_s);
    
    // Limpiar recursos
    pthread_mutex_destroy(&shared_mem->mutex);
    pthread_cond_destroy(&shared_mem->turn_cond);
    munmap(shared_mem, sizeof(shared_memory_t));
    
    return 0;
}
