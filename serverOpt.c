#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

//#define PORT 7006        // Puerto en el que el servidor escucha
#define BUFFER_SIZE 1024 // Tamaño del buffer para recibir datos
int ports[] = {49200, 49201, 49202}; // Puertos en los que el servidor escucha
//server.c

/*
    Función para desencriptar texto usando el cifrado César
*/
void decryptCaesar(char *text, int shift){
    // Ajustamos el desplazamiento y verificamos si las letras son mayúsculas o minúsculas
    shift = shift % 26;
    for (int i = 0; text[i] != '\0'; i++){
        char c = text[i];
        if (isupper(c)){
            text[i] = ((c - 'A' - shift + 26) % 26) + 'A';
        }
        else if (islower(c)){
            text[i] = ((c - 'a' - shift + 26) % 26) + 'a';
        }
        else{
            text[i] = c;
        }
    }
}

/*
    Función para encriptar texto usando el cifrado César
*/
void encryptCaesar(char *text, int shift) {
    // Ajustamos el desplazamiento y verificamos si las letras son mayúsculas o minúsculas
    shift = shift % 26; 
    for (int i = 0; text[i] != '\0'; i++) {
        char c = text[i];
        if (isupper(c)) { 
            text[i] = ((c - 'A' + shift) % 26) + 'A';
        } else if (islower(c)) {
            text[i] = ((c - 'a' + shift) % 26) + 'a';
        }
    }
}

/*
    Función para guardar la información de red del equipo en un archivo
*/
void saveNetworkInfo(const char *outputFile){
    FILE *fpCommand;
    FILE *fpOutput;
    char buffer[512];

    // Ejecutar comando para obtener información de red
    fpCommand = popen("ip addr show", "r");
    if (fpCommand == NULL)
    {
        perror("Error!");
        return;
    }
    // Abrir archivo para guardar la salida
    fpOutput = fopen(outputFile, "w");
    if (fpOutput == NULL)
    {
        perror("[-] Error to open the file");
        pclose(fpCommand);

        return;
    }

    // Leer línea por línea y escribir en el archivo
    while (fgets(buffer, sizeof(buffer), fpCommand) != NULL)
    {
        fputs(buffer, fpOutput);
    }
    // Cerrar ambos archivos
    fclose(fpOutput);
    pclose(fpCommand);
}

/*
    Función para enviar un archivo a través del socket
*/
void sendFile(const char *filename, int sockfd){
    FILE *fp = fopen(filename, "r");
    if (fp == NULL){
        perror("[-] Cannot open the file");
        return;
    }
    char buffer[BUFFER_SIZE];
    size_t bytes;

    // Leemos el archivo y enviamos su contenido a través del socket
    while ((bytes = fread(buffer, 1, sizeof(buffer), fp)) > 0){
        if (send(sockfd, buffer, bytes, 0) == -1){
            perror("[-] Error to send the file");
            break;
        }
    }
    fclose(fp);
}

/*
    Función para convertir a minúsculas
*/
void toLowerCase(char *str){
    for (int i = 0; str[i]; i++)
        str[i] = tolower((unsigned char)str[i]);
}

/*
    Función para eliminar espacios al inicio y final
*/
void trim(char *str){
    char *end;
    while (isspace((unsigned char)*str))
        str++; // inicio
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end))
        end--; // final

    *(end + 1) = '\0';
}

/*
    Función para verificar si una palabra está en el archivo cipherworlds.txt
*/
bool isOnFile(const char *bufferOriginal){
    FILE *fp;
    char line[BUFFER_SIZE];
    char buffer[BUFFER_SIZE];
    bool foundWorld = false;
    // Copiamos el buffer original para poder modificarlo
    strncpy(buffer, bufferOriginal, BUFFER_SIZE);
    buffer[BUFFER_SIZE - 1] = '\0'; // aseguramos terminación
    trim(buffer);
    toLowerCase(buffer);
    fp = fopen("cipherworlds.txt", "r");
    if (fp == NULL){
        printf("[-]Error opening file!\n");
        return false;
    }
    while (fgets(line, sizeof(line), fp) != NULL){
        line[strcspn(line, "\n")] = '\0';
        trim(line);
        toLowerCase(line);
        if (strcmp(line, buffer) == 0)
        {
            foundWorld = true;
            break;
        }
    }
    fclose(fp);
    return foundWorld;
}

/*
    Función para guardar la información del sistema en el archivo sysinfo.txt
*/
void saveSystemInfo(const char *outputFile){
    FILE *fp = fopen(outputFile, "w");
    if (!fp) {
        perror("[-] Error opening file");
        return;
    }

    char buffer[512];
    FILE *fpCommand;

    // Sistema y Kernel
    fprintf(fp, "\n*** Sistema y Kernel ***\n");
    fpCommand = popen("uname -s -r", "r");
    if (fpCommand) {
        while (fgets(buffer, sizeof(buffer), fpCommand) != NULL)
            fputs(buffer, fp);
        pclose(fpCommand);
    }
    fprintf(fp, "\n");

    // Distribución
    fprintf(fp, "*** Distribución ***\n");
    fpCommand = popen("cat /etc/os-release | grep PRETTY_NAME", "r");
    if (fpCommand) {
        while (fgets(buffer, sizeof(buffer), fpCommand) != NULL)
            fputs(buffer, fp);
        pclose(fpCommand);
    }
    fprintf(fp, "\n");

    // Direcciones IP
    fprintf(fp, "*** Direcciones IP ***\n");
    fpCommand = popen("ip -o addr show | awk '{print $2, $3, $4}'", "r");
    if (fpCommand) {
        while (fgets(buffer, sizeof(buffer), fpCommand) != NULL)
            fputs(buffer, fp);
        pclose(fpCommand);
    }
    fprintf(fp, "\n");

    // CPU y Núcleos
    fprintf(fp, "*** CPU y Núcleos ***\n");
    fpCommand = popen("lscpu", "r");
    if (fpCommand) {
        while (fgets(buffer, sizeof(buffer), fpCommand) != NULL)
            fputs(buffer, fp);
        pclose(fpCommand);
    }
    fprintf(fp, "\n");

    // Memoria
    fprintf(fp, "*** Memoria ***\n");
    fpCommand = popen("free -h", "r");
    if (fpCommand) {
        while (fgets(buffer, sizeof(buffer), fpCommand) != NULL)
            fputs(buffer, fp);
        pclose(fpCommand);
    }
    fprintf(fp, "\n");

    // Disco
    fprintf(fp, "*** Disco ***\n");
    fpCommand = popen("df -h", "r");
    if (fpCommand) {
        while (fgets(buffer, sizeof(buffer), fpCommand) != NULL)
            fputs(buffer, fp);
        pclose(fpCommand);
    }
    fprintf(fp, "\n");

    // Usuarios conectados
    fprintf(fp, "*** Usuarios conectados ***\n");
    fpCommand = popen("who", "r");
    if (fpCommand) {
        while (fgets(buffer, sizeof(buffer), fpCommand) != NULL)
            fputs(buffer, fp);
        pclose(fpCommand);
    }
    fprintf(fp, "\n");

    // Todos los usuarios del sistema
    fprintf(fp, "*** Todos los usuarios del sistema ***\n");
    fpCommand = popen("cut -d: -f1 /etc/passwd", "r");
    if (fpCommand) {
        while (fgets(buffer, sizeof(buffer), fpCommand) != NULL)
            fputs(buffer, fp);
        pclose(fpCommand);
    }
    fprintf(fp, "\n");

    // Uptime
    fprintf(fp, "*** Uptime ***\n");
    fpCommand = popen("uptime -p", "r");
    if (fpCommand) {
        while (fgets(buffer, sizeof(buffer), fpCommand) != NULL)
            fputs(buffer, fp);
        pclose(fpCommand);
    }
    fprintf(fp, "\n");

    // Procesos activos
    fprintf(fp, "*** Procesos activos ***\n");
    fpCommand = popen("ps -e", "r");
    if (fpCommand) {
        while (fgets(buffer, sizeof(buffer), fpCommand) != NULL)
            fputs(buffer, fp);
        pclose(fpCommand);
    }
    fprintf(fp, "\n");

    // Directorios montados
    fprintf(fp, "*** Directorios montados ***\n");
    fpCommand = popen("mount | column -t", "r");
    if (fpCommand) {
        while (fgets(buffer, sizeof(buffer), fpCommand) != NULL)
            fputs(buffer, fp);
        pclose(fpCommand);
    }
    fprintf(fp, "\n");

    fclose(fp);
}


/*
    Función principal con la configuración del socket
*/
int main(){

    int server_ports[3];
    struct sockaddr_in server_addr[3];
    fd_set readfds;
    int max_fd = -1;
    char buffer[BUFFER_SIZE] = {0};
    char file_content[BUFFER_SIZE] = {0};
    int shift, requested_port;

    // Creamos los sockets 
    for (int i = 0; i < 3; i++) {
        server_ports[i] = socket(AF_INET, SOCK_STREAM, 0);
        if (server_ports[i] < 0) {
            perror("[-] Error creating socket");
            return 1;
        }

        int opt = 1;
        // Siempre seteamos SO_REUSEADDR
        if (setsockopt(server_ports[i], SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            perror("setsockopt SO_REUSEADDR failed");
            return 1;
        }

        // Solo seteamos SO_REUSEPORT si está definido en el sistema
        #ifdef SO_REUSEPORT
        if (setsockopt(server_ports[i], SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
            perror("setsockopt SO_REUSEPORT failed");
            return 1;
        }
        #endif
        //Configuramos la dirección del servidor (IPv4, puerto, cualquier IP local).
        server_addr[i].sin_family = AF_INET;
        server_addr[i].sin_port = htons(ports[i]);
        server_addr[i].sin_addr.s_addr = INADDR_ANY;
    

        //Asignamos el socket a la dirección y puerto especificados
        if (bind(server_ports[i], (struct sockaddr *)&server_addr[i], sizeof(server_addr[i])) < 0) {
            perror("bind");
            exit(1);
        }

        // Escuchamos conexiones entrantes
        if (listen(server_ports[i], 1) < 0){
            perror("[-] Error on listen");
            close(server_ports[i]);
            return 1;
        }

        printf("[+] Server listening on port %d...\n", ports[i]);

        if (server_ports[i] > max_fd) {
            max_fd = server_ports[i];
        }
    }

    //int processed[3] = {0,0,0}; // bandera si cada puerto ya recibió algo
    //int remaining = 3;

    while (1) {
        FD_ZERO(&readfds);
        for (int i = 0; i < 3; i++) {
            FD_SET(server_sockets[i], &readfds);
        }

        int activity = select(max_fd + 1, &readfds, NULL, NULL, NULL);
        if (activity < 0) {
            perror("select error");
            continue;
        }

        for (int i = 0; i < 3; i++) {
            if (FD_ISSET(server_sockets[i], &readfds)) {
                struct sockaddr_in client_addr;
                socklen_t addr_size = sizeof(client_addr);
                int client_sock = accept(server_sockets[i], (struct sockaddr*)&client_addr, &addr_size);
                if (client_sock < 0) {
                    perror("Accept error");
                    continue;
                }

                char buffer[BUFFER_SIZE] = {0};
                char file_content[BUFFER_SIZE] = {0};
                int requested_port, shift;

                int bytes = recv(client_sock, buffer, sizeof(buffer) - 1, 0);
                if (bytes > 0) {
                    buffer[bytes] = '\0';
                    if (sscanf(buffer, "%d|%d|%[^\n]", &requested_port, &shift, file_content) == 3) {
                        if (requested_port == ports[i] && shift == 34) {
                            encryptCaesar(file_contenta, shift);
                            char *msg = "File received and encrypted";
                            send(client_sock, msg, strlen(msg), 0);
                            printf("[SERVER %d] File encrypted:\n%s\n", ports[i], file_content);
                        } else {
                            char *msg = "REJECTED\n";
                            send(client_sock, msg, strlen(msg), 0);
                            printf("[SERVER %d] Request rejected. Port: %d, Shift: %d\n", 
                                   ports[i], requested_port, shift);
                        }
                    } else {
                        char *msg = "REJECTED\n";
                        send(client_sock, msg, strlen(msg), 0);
                        printf("[SERVER %d] Invalid format. Request rejected.\n", ports[i]);
                    }
                } else {
                    char *msg = "REJECTED\n";
                    send(client_sock, msg, strlen(msg), 0);
                    printf("[SERVER %d] No data received. Request rejected.\n", ports[i]);
                }

                close(client_sock);
            }
        }
    }

    // Cerrar sockets de servidor
    for (int i = 0; i < 3; i++){
        close(server_ports[i]);
    }
    return 0;
}
