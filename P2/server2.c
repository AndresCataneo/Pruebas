// server.c
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>

#define BUFFER_SIZE 1024
#define SERVER_PORT 49200
#define QUANTUM_TIME 15

typedef struct job {
    char alias[32];
    char filename[256];
    char content[BUFFER_SIZE];
    struct job *next;
} job_t;

typedef struct {
    job_t *head, *tail;
    pthread_mutex_t mutex;
} queue_t;

typedef struct {
    int current_server;
    time_t turn_start;
    pthread_mutex_t mutex;
    pthread_cond_t turn_cond;
} shared_t;

shared_t *shared_mem;
char *server_names[4];
queue_t queues[4]; // una cola por servidor

void queue_init(queue_t *q){
    q->head = q->tail = NULL;
    pthread_mutex_init(&q->mutex, NULL);
}

void queue_push(queue_t *q, job_t *job){
    pthread_mutex_lock(&q->mutex);
    job->next = NULL;
    if(!q->tail) q->head = q->tail = job;
    else {
        q->tail->next = job;
        q->tail = job;
    }
    pthread_mutex_unlock(&q->mutex);
}

job_t *queue_pop(queue_t *q){
    pthread_mutex_lock(&q->mutex);
    job_t *j = q->head;
    if(j){
        q->head = j->next;
        if(!q->head) q->tail = NULL;
    }
    pthread_mutex_unlock(&q->mutex);
    return j;
}

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

void *server_worker(void *arg){
    int idx = *(int*)arg;
    free(arg);
    while(1){
        // esperar a que sea nuestro turno
        pthread_mutex_lock(&shared_mem->mutex);
        while(shared_mem->current_server != idx){
            pthread_cond_wait(&shared_mem->turn_cond,&shared_mem->mutex);
        }
        pthread_mutex_unlock(&shared_mem->mutex);

        // consumir cola mientras haya trabajos y no expire quantum
        time_t start = time(NULL);
        job_t *job;
        while((job=queue_pop(&queues[idx]))){
            saveFile(job->alias,job->filename,job->content);
            free(job);
            if(time(NULL)-start>=QUANTUM_TIME) break;
        }

        // si se acabó el quantum pasar turno
        if(time(NULL)-start>=QUANTUM_TIME || !job){
            pthread_mutex_lock(&shared_mem->mutex);
            shared_mem->current_server=(shared_mem->current_server+1)%4;
            shared_mem->turn_start=time(NULL);
            pthread_cond_broadcast(&shared_mem->turn_cond);
            pthread_mutex_unlock(&shared_mem->mutex);
        }
        sleep(1);
    }
    return NULL;
}

void *handle_connection(void *arg){
    int client_fd = *(int*)arg;
    free(arg);
    char buf[BUFFER_SIZE*2]={0};
    int n=recv(client_fd,buf,sizeof(buf)-1,0);
    if(n<=0){close(client_fd);return NULL;}
    buf[n]='\0';
    char alias[32],filename[256],content[BUFFER_SIZE];
    if(sscanf(buf,"%31[^|]|%255[^|]|%[^\n]",alias,filename,content)==3){
        // buscar índice del alias
        int idx=-1;
        for(int i=0;i<4;i++) if(strcmp(alias,server_names[i])==0){idx=i;break;}
        if(idx>=0){
            job_t *j=malloc(sizeof(job_t));
            strcpy(j->alias,alias);
            strcpy(j->filename,filename);
            strcpy(j->content,content);
            queue_push(&queues[idx],j);
            char *msg="QUEUED";
            send(client_fd,msg,strlen(msg),0);
            printf("[SERVER] queued file %s for %s\n",filename,alias);
        }else{
            char *msg="REJECTED - Unknown server";
            send(client_fd,msg,strlen(msg),0);
        }
    }else{
        char *msg="REJECTED - Invalid format";
        send(client_fd,msg,strlen(msg),0);
    }
    close(client_fd);
    return NULL;
}

int main(int argc,char *argv[]){
    if(argc<5){printf("use: %s s01 s02 s03 s04\n",argv[0]);return 1;}
    for(int i=0;i<4;i++){server_names[i]=argv[i+1];queue_init(&queues[i]);}

    shared_mem=mmap(NULL,sizeof(shared_t),PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    shared_mem->current_server=0;
    shared_mem->turn_start=time(NULL);
    pthread_mutex_init(&shared_mem->mutex,NULL);
    pthread_cond_init(&shared_mem->turn_cond,NULL);

    // lanzar worker por servidor
    for(int i=0;i<4;i++){
        int *idx=malloc(sizeof(int));*idx=i;
        pthread_t th;pthread_create(&th,NULL,server_worker,idx);pthread_detach(th);
    }

    int sock=socket(AF_INET,SOCK_STREAM,0);
    int opt=1;setsockopt(sock,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
    struct sockaddr_in addr={0};
    addr.sin_family=AF_INET;addr.sin_port=htons(SERVER_PORT);addr.sin_addr.s_addr=INADDR_ANY;
    bind(sock,(struct sockaddr*)&addr,sizeof(addr));
    listen(sock,10);
    printf("Listening on %d...\n",SERVER_PORT);

    while(1){
        int client_fd=accept(sock,NULL,NULL);
        if(client_fd<0) continue;
        int *fd=malloc(sizeof(int));*fd=client_fd;
        pthread_t th;pthread_create(&th,NULL,handle_connection,fd);pthread_detach(th);
    }
}
