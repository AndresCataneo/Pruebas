// server.c — encolado inmediato + procesado en turno
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

// Memoria compartida para coordinación de turnos
typedef struct {
    int current_server;       // índice del servidor con turno
    int receiving_server;     // índice del servidor que está recibiendo (-1 si ninguno)
    bool server_busy;         // true mientras un servidor procesa su turno
    time_t turn_start_time;   // cuándo empezó el turno actual
    pthread_mutex_t mutex;
    pthread_cond_t turn_cond;
} shared_memory_t;

typedef struct connection_node {
    int dynamic_client;
    int dynamic_sock;
    char target_server[32];
    struct connection_node *next;
} connection_node_t;

shared_memory_t *shared_mem;
char *server_names[4];

// Cola por servidor y mutex por cola
connection_node_t* connection_queues[4] = { NULL, NULL, NULL, NULL };
pthread_mutex_t queue_mutexes[4];

void saveFile(const char *server_name, const char *filename, const char *content) {
    char dir_path[512], file_path[512];
    char *home_dir = getenv("HOME");
    if (!home_dir) home_dir = "/home";
    snprintf(dir_path, sizeof(dir_path), "%s/%s", home_dir, server_name);
    mkdir(dir_path, 0755);
    snprintf(file_path, sizeof(file_path), "%s/%s/%s", home_dir, server_name, filename);
    FILE *f = fopen(file_path, "w");
    if (f) {
        fputs(content, f);
        fclose(f);
        printf("[SAVE] %s/%s\n", server_name, filename);
    } else {
        perror("[saveFile] fopen");
    }
}

bool is_quantum_expired(time_t start_time) {
    return (time(NULL) - start_time) >= QUANTUM_TIME;
}

// Encola inmediatamente la conexión en la cola del alias destino
void add_to_queue(const char* target_server, int dynamic_client, int dynamic_sock) {
    connection_node_t *node = malloc(sizeof(connection_node_t));
    if (!node) {
        close(dynamic_client); close(dynamic_sock);
        return;
    }
    node->dynamic_client = dynamic_client;
    node->dynamic_sock = dynamic_sock;
    strncpy(node->target_server, target_server, sizeof(node->target_server)-1);
    node->target_server[sizeof(node->target_server)-1] = '\0';
    node->next = NULL;

    int idx = -1;
    for (int i = 0; i < 4; ++i) if (strcmp(target_server, server_names[i]) == 0) { idx = i; break; }
    if (idx == -1) {
        // alias desconocido — responder y cerrar
        char *msg = "REJECTED - Unknown server";
        send(dynamic_client, msg, strlen(msg), 0);
        close(dynamic_client); close(dynamic_sock); free(node);
        return;
    }

    pthread_mutex_lock(&queue_mutexes[idx]);
    if (connection_queues[idx] == NULL) connection_queues[idx] = node;
    else {
        connection_node_t *cur = connection_queues[idx];
        while (cur->next) cur = cur->next;
        cur->next = node;
    }
    pthread_mutex_unlock(&queue_mutexes[idx]);

    printf("[ENQUEUE] queued for %s (fd=%d)\n", target_server, dynamic_client);
    // NOTA: no cerramos sockets aquí; el worker los cerrará al procesar.
}

// Saca el primer nodo de la cola (o NULL)
connection_node_t* pop_queue(int server_index) {
    pthread_mutex_lock(&queue_mutexes[server_index]);
    connection_node_t *n = connection_queues[server_index];
    if (n) connection_queues[server_index] = n->next;
    pthread_mutex_unlock(&queue_mutexes[server_index]);
    if (n) n->next = NULL;
    return n;
}

// Procesa la conexión: hace recv() real, guarda archivo y responde al cliente
void process_connection(int dynamic_client, int dynamic_sock, const char* expected_alias) {
    char buf[BUFFER_SIZE*2];
    char filename[256];
    char content[BUFFER_SIZE];

    // recibir el mensaje (mensajes pequeños; para producción hay que loop)
    int bytes = recv(dynamic_client, buf, sizeof(buf) - 1, 0);
    if (bytes <= 0) { close(dynamic_client); close(dynamic_sock); return; }
    buf[bytes] = '\0';

    char alias[32];
    if (sscanf(buf, "%31[^|]|%255[^|]|%[^\n]", alias, filename, content) == 3) {
        if (strcmp(alias, expected_alias) == 0) {
            saveFile(alias, filename, content);
            char *msg = "File received successfully";
            send(dynamic_client, msg, strlen(msg), 0);
            printf("[PROC %s] Saved %s (fd=%d)\n", expected_alias, filename, dynamic_client);
        } else {
            char *msg = "REJECTED - Wrong server";
            send(dynamic_client, msg, strlen(msg), 0);
            printf("[PROC %s] Rejected for %s (fd=%d)\n", expected_alias, alias, dynamic_client);
        }
    } else {
        char *msg = "REJECTED - Invalid format";
        send(dynamic_client, msg, strlen(msg), 0);
        printf("[PROC %s] Invalid format (fd=%d)\n", expected_alias, dynamic_client);
    }

    close(dynamic_client);
    close(dynamic_sock);
}

// Worker por servidor: espera su turno y procesa su cola durante el quantum
void* server_thread(void* arg) {
    int server_index = *(int*)arg; free(arg);
    const char *myname = server_names[server_index];

    while (1) {
        // esperar que sea nuestro turno y que no haya otro recibiendo
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

        time_t start = time(NULL);
        bool processed_any = false;
        int cnt = 0;

        // procesar mientras haya cola y no expire quantum
        while (!is_quantum_expired(start)) {
            connection_node_t *node = pop_queue(server_index);
            if (node) {
                processed_any = true;
                cnt++;
                process_connection(node->dynamic_client, node->dynamic_sock, myname);
                free(node);
                continue; // seguir procesando mientras haya y tiempo
            } else {
                // cola vacía: si ya procesamos algo agotamos tiempo restante; si no, comprobamos periódicamente
                if (processed_any) {
                    time_t elapsed = time(NULL) - start;
                    time_t rem = (elapsed >= QUANTUM_TIME) ? 0 : (QUANTUM_TIME - elapsed);
                    if (rem > 0) sleep(rem);
                    break;
                } else {
                    sleep(1); // esperar a que lleguen nuevas conexiones (enqueue inmediato desde handlers)
                }
            }
        }

        time_t used = time(NULL) - start;
        if (processed_any) printf("[TURN END] %s processed %d files in %ld s\n", myname, cnt, used);
        else printf("[TURN END] %s: quantum expired with no files (elapsed %ld s)\n", myname, used);

        // ceder turno
        pthread_mutex_lock(&shared_mem->mutex);
        shared_mem->server_busy = false;
        shared_mem->receiving_server = -1;
        shared_mem->current_server = (shared_mem->current_server + 1) % 4;
        shared_mem->turn_start_time = time(NULL);
        pthread_cond_broadcast(&shared_mem->turn_cond);
        pthread_mutex_unlock(&shared_mem->mutex);

        sleep(1);
    }
    return NULL;
}

// Handler que hace MSG_PEEK, parsea alias y encola inmediatamente
void* connection_handler(void* arg) {
    int *s = (int*)arg;
    int dynamic_client = s[0];
    int dynamic_sock = s[1];
    free(arg);

    // leemos por adelantado (peek) para conocer el alias sin consumir el mensaje,
    // así el worker podrá recv() normalmente cuando procese la conexión.
    char peek_buf[BUFFER_SIZE*2];
    int bytes = recv(dynamic_client, peek_buf, sizeof(peek_buf)-1, MSG_PEEK);
    if (bytes <= 0) { close(dynamic_client); close(dynamic_sock); return NULL; }
    peek_buf[bytes] = '\0';

    char alias[32], filename[256], content[BUFFER_SIZE];
    if (sscanf(peek_buf, "%31[^|]|%255[^|]|%[^\n]", alias, filename, content) == 3) {
        add_to_queue(alias, dynamic_client, dynamic_sock);
    } else {
        char *msg = "REJECTED - Invalid format";
        send(dynamic_client, msg, strlen(msg), 0);
        close(dynamic_client);
        close(dynamic_sock);
    }
    return NULL;
}

// Quantum manager: si nadie está recibiendo y expiró quantum, avanza turno
void* quantum_manager(void* arg) {
    (void)arg;
    while (1) {
        sleep(1);
        pthread_mutex_lock(&shared_mem->mutex);
        if (!shared_mem->server_busy && is_quantum_expired(shared_mem->turn_start_time)) {
            shared_mem->current_server = (shared_mem->current_server + 1) % 4;
            shared_mem->turn_start_time = time(NULL);
            printf("[QUANTUM] switching to %s\n", server_names[shared_mem->current_server]);
            pthread_cond_broadcast(&shared_mem->turn_cond);
        }
        pthread_mutex_unlock(&shared_mem->mutex);
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 5) { fprintf(stderr, "Usage: %s s01 s02 s03 s04\n", argv[0]); return 1; }
    for (int i=0;i<4;i++) { server_names[i] = argv[i+1]; pthread_mutex_init(&queue_mutexes[i], NULL); }

    // init shared mem
    shared_mem = mmap(NULL, sizeof(shared_memory_t), PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0);
    if (shared_mem == MAP_FAILED) { perror("mmap"); return 1; }
    shared_mem->current_server = 0;
    shared_mem->receiving_server = -1;
    shared_mem->server_busy = false;
    shared_mem->turn_start_time = time(NULL);
    pthread_mutex_init(&shared_mem->mutex, NULL);
    pthread_cond_init(&shared_mem->turn_cond, NULL);

    // lanzar workers por servidor
    for (int i=0;i<4;i++){
        int *idx = malloc(sizeof(int)); *idx = i;
        pthread_t th; pthread_create(&th, NULL, server_thread, idx); pthread_detach(th);
    }

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
        struct sockaddr_in client_addr; socklen_t alen = sizeof(client_addr);
        int client_fd = accept(port_s, (struct sockaddr*)&client_addr, &alen);
        if (client_fd < 0) continue;

        int dynamic_port = SERVER_PORT + port_counter++;
        char port_msg[64]; snprintf(port_msg, sizeof(port_msg), "DYNAMIC_PORT|%d", dynamic_port);
        send(client_fd, port_msg, strlen(port_msg), 0);
        close(client_fd);

        int dyn_sock = socket(AF_INET, SOCK_STREAM, 0);
        setsockopt(dyn_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        struct sockaddr_in dyn = {0}; dyn.sin_family = AF_INET; dyn.sin_port = htons(dynamic_port); dyn.sin_addr.s_addr = INADDR_ANY;
        if (bind(dyn_sock, (struct sockaddr*)&dyn, sizeof(dyn)) < 0) { perror("bind dyn"); close(dyn_sock); continue; }
        if (listen(dyn_sock, 1) < 0) { perror("listen dyn"); close(dyn_sock); continue; }

        int dyn_client = accept(dyn_sock, NULL, NULL);
        if (dyn_client < 0) { close(dyn_sock); continue; }

        printf("[*] Client connected on dynamic port %d (fd=%d)\n", dynamic_port, dyn_client);

        int *s = malloc(2*sizeof(int)); s[0]=dyn_client; s[1]=dyn_sock;
        pthread_t h; pthread_create(&h, NULL, connection_handler, s); pthread_detach(h);
    }

    close(port_s);
    return 0;
}
