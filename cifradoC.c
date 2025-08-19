#include <stdio.h>
#include <ctype.h>

void encryptCaesar(char *text, int shift) {
    shift = shift % 26;  // Aseguramos que el desplazamiento esté entre 0 y 25
    for (int i = 0; text[i] != '\0'; i++) {
        char c = text[i];
        if (isupper(c)) { // Si es letra mayúscula
            text[i] = ((c - 'A' + shift) % 26) + 'A';
        } else if (islower(c)) { // Si es letra minúscula
            text[i] = ((c - 'a' + shift) % 26) + 'a';
        }
    }
}

int main() {
    char mensaje[100];
    int desplazamiento;

    printf("Introduce un mensaje: ");
    fgets(mensaje, sizeof(mensaje), stdin); // Leemos el mensaje desde stdin
    printf("Introduce el desplazamiento: ");
    scanf("%d", &desplazamiento);

    encryptCaesar(mensaje, desplazamiento);
    printf("Mensaje cifrado: %s\n", mensaje);

    return 0;
}
