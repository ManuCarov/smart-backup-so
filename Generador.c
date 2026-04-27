#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

// Función para mostrar el manual de uso
void print_usage(const char *prog_name) {
    printf("Uso: %s -o <archivo_salida> -s <num_elementos> -p <patron>\n\n", prog_name);
    printf("Opciones:\n");
    printf("  -o  Ruta del archivo de salida (requerido)\n");
    printf("  -s  Cantidad de elementos (floats) a generar (por defecto: 1000)\n");
    printf("  -p  Tipo de patrón (por defecto: 0)\n");
    printf("  -h  Muestra esta ayuda\n\n");
    printf("Patrones disponibles:\n");
    printf("  0 : Constante (Todos los valores iguales) -> ¡Excelente compresión LZ77!\n");
    printf("  1 : Secuencia repetitiva (Ej. 0,1,2,3...0,1,2,3) -> ¡Muy buena compresión!\n");
    printf("  2 : Incremental continuo (0,1,2,3,4,5...) -> Mala compresión (sin repeticiones exactas)\n");
    printf("  3 : Completamente aleatorio -> Peor escenario de compresión\n");
}

int main(int argc, char *argv[]) {
    int opt;
    char *output_file = NULL;
    size_t num_elements = 1000;
    int pattern = 0;

    // Analizar los argumentos
    while ((opt = getopt(argc, argv, "o:s:p:h")) != -1) {
        switch (opt) {
            case 'o':
                output_file = optarg;
                break;
            case 's':
                num_elements = (size_t)atol(optarg);
                break;
            case 'p':
                pattern = atoi(optarg);
                break;
            case 'h':
                print_usage(argv[0]);
                return EXIT_SUCCESS;
            default:
                print_usage(argv[0]);
                return EXIT_FAILURE;
        }
    }

    if (!output_file) {
        fprintf(stderr, "Error: Debes especificar un archivo de salida con -o.\n");
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    FILE *out = fopen(output_file, "wb");
    if (!out) {
        perror("Error al crear el archivo de salida");
        return EXIT_FAILURE;
    }

    srand((unsigned int)time(NULL));
    printf("Generando archivo '%s' con %zu elementos (Patrón: %d)...\n", output_file, num_elements, pattern);

    for (size_t i = 0; i < num_elements; i++) {
        float val = 0.0f;
        
        switch (pattern) {
            case 0:
                val = 42.0f; // Patrón constante
                break;
            case 1:
                val = (float)(i % 15); // Se repite cada 15 elementos
                break;
            case 2:
                val = (float)i; // Incremental puro
                break;
            case 3:
                val = ((float)rand() / (float)RAND_MAX) * 1000.0f; // Aleatorio 0-1000
                break;
            default:
                val = 0.0f;
                break;
        }

        fwrite(&val, sizeof(float), 1, out);
    }

    fclose(out);
    
    printf("¡Archivo generado con éxito! Tamaño aproximado: %zu bytes.\n", num_elements * sizeof(float));

    return EXIT_SUCCESS;
}