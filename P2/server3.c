// server.c (corregido)
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>

#define BUFFER_SIZE 1024
#define SERVER_PORT 49200
#define QUANTUM_TIME 15

// memoria compartida para coordinación de turnos
typedef struct {
    int current_server;       // índice del servidor que tiene el turno
    int receiving_server;     // índice del servidor que está recibiendo (-1 si ninguno)
    bool server_busy;         // true mientras un servidor procesa en su turno
    time_t turn_start_time;   // cuándo comenzó el turno actual
    pthread_mutex_t mutex;
    pthread_cond_t turn_cond;
} shared_memory_t;

// nodo para cola de conexiones
typedef struct connection_node {
    int dynamic_client;
    int dynamic_sock;
    char target_server[32];
    struct connection_node* next;
} connection_node_t;

shared_memory_t *shared_mem;
char *server_names[4];

// colas por servidor + mutex por cola
connection_node_t* connection_queues[4] = { NULL, NULL, NULL, NULL };
pthread_mutex_t queue_mutexes[4];

// Guarda archivo en directorio ~/sXX/filename
void saveFile(const char *server_name, const char *filename, const char *content) {
    char file_path[512];
    char dir_path[512];
    char *home_dir = getenv("HOME");
    if (!home_dir) home_dir = "/home";
    snprintf(dir_path, sizeof(dir_path), "%s/%s", home_dir, server_name);
    mkdir(dir_path, 0755);
    snprintf(file_path, sizeof(file_path), "%s/%s/%s", home_dir, server_name, filename);
    FILE *f = fopen(file_path, "w");
    if (f) {
        fputs(content, f);
        fclose(f);
    } else {
        perror("[saveFile] fopen");
    }
}

bool is_quantum_expired(time_t start_time) {
    return (time(NULL) - start_time) >= QUANTUM_TIME;
}

// Encola una conexión en la cola correspondiente al alias (target_server)
void add_to_queue(const char* target_server, int dynamic_client, int dynamic_sock) {
    connection_node_t* new_node = malloc(sizeof(connection_node_t));
    if (!new_node) return;
    new_node->dynamic_client = dynamic_client;
    new_node->dynamic_sock = dynamic_sock;
    strncpy(new_node->target_server, target_server, sizeof(new_node->target_server)-1);
    new_node->target_server[sizeof(new_node->target_server)-1] = '\0';
    new_node->next = NULL;

    int server_index = -1;
    for (int i = 0; i < 4; ++i) {
        if (strcmp(target_server, server_names[i]) == 0) { server_index = i; break; }
    }
    if (server_index == -1) {
        // alias desconocido: responder y cerrar
        char *msg = "REJECTED - Unknown server";
        send(dynamic_client, msg, strlen(msg), 0);
        close(dynamic_client);
        close(dynamic_sock);
        free(new_node);
        return;
    }

    pthread_mutex_lock(&queue_mutexes[server_index]);
    if (connection_queues[server_index] == NULL) {
        connection_queues[server_index] = new_node;
    } else {
        connection_node_t *cur = connection_queues[server_index];
        while (cur->next) cur = cur->next;
        cur->next = new_node;
    }
    pthread_mutex_unlock(&queue_mutexes[server_index]);

    // Informativo
    printf("[ENQUEUE] Job queued for %s (client fd=%d)\n", target_server, dynamic_client);

    // No cerramos el socket aquí; lo cerrará quien procese la conexión.
}

// Saca siguiente conexión de la cola (o NULL)
connection_node_t* get_next_connection(int server_index) {
    pthread_mutex_lock(&queue_mutexes[server_index]);
    connection_node_t* node = connection_queues[server_index];
    if (node) {
        connection_queues[server_index] = node->next;
        node->next = NULL;
    }
    pthread_mutex_unlock(&queue_mutexes[server_index]);
    return node;
}

// Procesa la conexión: lee el mensaje (alias|filename|content), guarda archivo y responde al cliente.
void process_connection(int dynamic_client, int dynamic_sock, const char* expected_alias) {
    char buffer[BUFFER_SIZE * 2];
    char filename[256];
    char file_content[BUFFER_SIZE];

    // Recibir todo el mensaje (asumimos mensaje pequeño; para producción habría que loop recv)
    int bytes = recv(dynamic_client, buffer, sizeof(buffer) - 1, 0);
    if (bytes <= 0) {
        // el cliente cerró o error
        close(dynamic_client);
        close(dynamic_sock);
        return;
    }
    buffer[bytes] = '\0';

    char alias[32];
    if (sscanf(buffer, "%31[^|]|%255[^|]|%[^\n]", alias, filename, file_content) == 3) {
        if (strcmp(alias, expected_alias) == 0) {
            saveFile(alias, filename, file_content);
            char *msg = "File received successfully";
            send(dynamic_client, msg, strlen(msg), 0);
            printf("[PROC %s] Saved %s (client fd=%d)\n", alias, filename, dynamic_client);
        } else {
            char *msg = "REJECTED - Wrong server";
            send(dynamic_client, msg, strlen(msg), 0);
            printf("[PROC %s] Rejected file for %s (client fd=%d)\n", expected_alias, alias, dynamic_client);
        }
    } else {
        char *msg = "REJECTED - Invalid format";
        send(dynamic_client, msg, strlen(msg), 0);
        printf("[PROC %s] Invalid format from client fd=%d\n", expected_alias, dynamic_client);
    }

    close(dynamic_client);
    close(dynamic_sock);
}

// Hilo trabajador por servidor: espera su turno y procesa su cola durante su quantum
void* server_thread(void* arg) {
    int server_index = *(int*)arg; free(arg);
    const char *myname = server_names[server_index];

    while (1) {
        // esperar hasta que sea nuestro turno y nadie esté recibiendo
        pthread_mutex_lock(&shared_mem->mutex);
        while (shared_mem->current_server != server_index || shared_mem->server_busy) {
            pthread_cond_wait(&shared_mem->turn_cond, &shared_mem->mutex);
        }

        // iniciar turno
        shared_mem->server_busy = true;
        shared_mem->receiving_server = server_index;
        shared_mem->turn_start_time = time(NULL);
        pthread_mutex_unlock(&shared_mem->mutex);

        printf("\n[TURN START] %s (quantum %ds)\n", myname, QUANTUM_TIME);

        // procesar trabajos durante el quantum
        time_t start_time = time(NULL);
        bool did_any = false;
        int processed_count = 0;

        while (!is_quantum_expired(start_time)) {
            connection_node_t* node = get_next_connection(server_index);
            if (node) {
                did_any = true;
                processed_count++;
                process_connection(node->dynamic_client, node->dynamic_sock, myname);
                free(node);
                // después de procesar seguimos en el loop para ver si hay más trabajos
                continue;
            } else {
                // cola vacía: si ya procesamos algo, esperar el tiempo restante; si no, esperar brevemente y volver a verificar
                if (did_any) {
                    // esperar tiempo restante para agotar quantum
                    time_t elapsed = time(NULL) - start_time;
                    time_t remaining = (elapsed >= QUANTUM_TIME) ? 0 : (QUANTUM_TIME - elapsed);
                    if (remaining > 0) sleep(remaining);
                    break;
                } else {
                    // no hemos procesado nada aún, reintentar cada segundo hasta que expire quantum
                    sleep(1);
                }
            }
        }

        time_t used = time(NULL) - start_time;
        if (did_any) {
            printf("[TURN END] %s processed %d files in %ld s\n", myname, processed_count, used);
        } else {
            printf("[TURN END] %s: quantum expired with no files (elapsed %ld s)\n", myname, used);
        }

        // ceder turno al siguiente
        pthread_mutex_lock(&shared_mem->mutex);
        shared_mem->server_busy = false;
        shared_mem->receiving_server = -1;
        shared_mem->current_server = (shared_mem->current_server + 1) % 4;
        shared_mem->turn_start_time = time(NULL); // reiniciar tiempo de turno cuando cambiamos
        pthread_cond_broadcast(&shared_mem->turn_cond);
        pthread_mutex_unlock(&shared_mem->mutex);

        // breve pausa antes de la siguiente iteración
        sleep(1);
    }
    return NULL;
}

// Handler que se ejecuta justo después de accept() del puerto dinámico.
// Usamos MSG_PEEK para leer el mensaje sin consumirlo y así poder re-recv() en el worker.
// Extraemos alias y encolamos la conexión en la cola correcta.
void* connection_handler(void* arg) {
    int *socks = (int*)arg;
    int dynamic_client = socks[0];
    int dynamic_sock = socks[1];
    free(arg);

    char peek_buf[BUFFER_SIZE * 2];
    int bytes = recv(dynamic_client, peek_buf, sizeof(peek_buf) - 1, MSG_PEEK);
    if (bytes <= 0) {
        close(dynamic_client);
        close(dynamic_sock);
        return NULL;
    }
    peek_buf[bytes] = '\0';

    char alias[32];
    char filename[256];
    char content[BUFFER_SIZE];

    if (sscanf(peek_buf, "%31[^|]|%255[^|]|%[^\n]", alias, filename, content) == 3) {
        // encolar según alias (el worker procesará y responderá)
        add_to_queue(alias, dynamic_client, dynamic_sock);
    } else {
        // formato inválido -> responder y cerrar
        char *msg = "REJECTED - Invalid format";
        send(dynamic_client, msg, strlen(msg), 0);
        close(dynamic_client);
        close(dynamic_sock);
    }
    return NULL;
}

// administrador que avanza el turno cuando no hay nadie recibiendo y el quantum expiró
void* quantum_manager(void* arg) {
    (void)arg;
    while (1) {
        sleep(1);
        pthread_mutex_lock(&shared_mem->mutex);
        if (!shared_mem->server_busy && is_quantum_expired(shared_mem->turn_start_time)) {
            shared_mem->current_server = (shared_mem->current_server + 1) % 4;
            shared_mem->turn_start_time = time(NULL);
            printf("[QUANTUM MANAGER] No one busy -> switching to %s\n", server_names[shared_mem->current_server]);
            pthread_cond_broadcast(&shared_mem->turn_cond);
        }
        pthread_mutex_unlock(&shared_mem->mutex);
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 5) {
        fprintf(stderr, "Usage: %s s01 s02 s03 s04\n", argv[0]);
        return 1;
    }
    for (int i = 0; i < 4; ++i) {
        server_names[i] = argv[i+1];
        pthread_mutex_init(&queue_mutexes[i], NULL);
    }

    // init shared memory for coordination
    shared_mem = mmap(NULL, sizeof(shared_memory_t), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared_mem == MAP_FAILED) { perror("mmap"); return 1; }
    shared_mem->current_server = 0;
    shared_mem->receiving_server = -1;
    shared_mem->server_busy = false;
    shared_mem->turn_start_time = time(NULL);
    pthread_mutex_init(&shared_mem->mutex, NULL);
    pthread_cond_init(&shared_mem->turn_cond, NULL);

    // lanzar worker por cada servidor (cada uno esperará a su turno)
    for (int i = 0; i < 4; ++i) {
        int *idx = malloc(sizeof(int));
        *idx = i;
        pthread_t th; pthread_create(&th, NULL, server_thread, idx); pthread_detach(th);
    }

    // lanzar quantum manager
    pthread_t qm; pthread_create(&qm, NULL, quantum_manager, NULL); pthread_detach(qm);

    // socket principal (puerto base)
    int port_s = socket(AF_INET, SOCK_STREAM, 0);
    if (port_s < 0) { perror("socket"); return 1; }
    int opt = 1; setsockopt(port_s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET; addr.sin_port = htons(SERVER_PORT); addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(port_s, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); close(port_s); return 1; }
    if (listen(port_s, 10) < 0) { perror("listen"); close(port_s); return 1; }

    printf("[*] Round Robin initialized (quantum %ds)\n", QUANTUM_TIME);
    printf("[*] Turn order: %s -> %s -> %s -> %s\n", server_names[0], server_names[1], server_names[2], server_names[3]);
    printf("[*] LISTENING on port %d...\n\n", SERVER_PORT);

    int port_counter = 1;
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addrlen = sizeof(client_addr);
        int client_fd = accept(port_s, (struct sockaddr*)&client_addr, &addrlen);
        if (client_fd < 0) continue;

        // asignar puerto dinámico
        int dynamic_port = SERVER_PORT + port_counter++;
        char port_msg[64];
        snprintf(port_msg, sizeof(port_msg), "DYNAMIC_PORT|%d", dynamic_port);
        send(client_fd, port_msg, strlen(port_msg), 0);
        close(client_fd);

        // crear socket dinámico y aceptar la conexión real del cliente
        int dynamic_sock = socket(AF_INET, SOCK_STREAM, 0);
        setsockopt(dynamic_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        struct sockaddr_in dyn_addr = {0};
        dyn_addr.sin_family = AF_INET; dyn_addr.sin_port = htons(dynamic_port); dyn_addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(dynamic_sock, (struct sockaddr*)&dyn_addr, sizeof(dyn_addr)) < 0) { perror("bind dyn"); close(dynamic_sock); continue; }
        if (listen(dynamic_sock, 1) < 0) { perror("listen dyn"); close(dynamic_sock); continue; }

        int dynamic_client = accept(dynamic_sock, NULL, NULL);
        if (dynamic_client < 0) { close(dynamic_sock); continue; }

        printf("[*] Client connected on dynamic port %d (fd=%d)\n", dynamic_port, dynamic_client);

        // lanzar handler que solo hace peek, parsea alias y encola
        int *socks = malloc(2 * sizeof(int));
        socks[0] = dynamic_client; socks[1] = dynamic_sock;
        pthread_t h; pthread_create(&h, NULL, connection_handler, socks); pthread_detach(h);
    }

    close(port_s);
    return 0;
}
