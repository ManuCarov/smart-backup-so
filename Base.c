#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> // Necesario para getopt
#include <stdint.h>

#define WINDOW_SIZE 4095
#define LOOKAHEAD_SIZE 15

// Función para mostrar el manual de uso del CLI
void print_usage(const char *prog_name) {
    printf("Uso: %s -i <archivo_entrada> -o <archivo_salida> [-a algoritmo]\n", prog_name);
    printf("Opciones:\n");
    printf("  -i  Ruta del archivo a comprimir (requerido)\n");
    printf("  -o  Ruta del archivo de salida (requerido)\n");
    printf("  -a  Algoritmo a usar (1: TurboQuant+LZ77, 2: Solo LZ77, 3: RLE) [Default: 1]\n");
    printf("  -e  Solo estimar (No guarda ningún archivo, -o se vuelve opcional)\n");
    printf("  -h  Muestra esta ayuda\n");
}

// Simulación del paso de "TurboQuant" (Cuantización de Matriz)
// Nota: El TurboQuant real usa Rotación Aleatoria (PolarQuant) y 
// Transformadas de Johnson-Lindenstrauss Cuantizadas (QJL) para bajar a 3 bits.
// Aquí aplicamos una cuantización lineal a 8 bits como puente conceptual.
void apply_turboquant_mock(const float *matrix, uint8_t *quantized, size_t elements) {
    if (elements == 0) return;
    float min_v = matrix[0], max_v = matrix[0];
    for (size_t i = 1; i < elements; i++) {
        if (matrix[i] < min_v) min_v = matrix[i];
        if (matrix[i] > max_v) max_v = matrix[i];
    }
    float range = (max_v - min_v == 0) ? 1.0f : (max_v - min_v);
    
    for (size_t i = 0; i < elements; i++) {
        // Normalizamos y escalamos la matriz de floats a valores discretos de 8 bits
        quantized[i] = (uint8_t)(((matrix[i] - min_v) / range) * 255.0f);
    }
}

// Algoritmo LZ... (Implementación básica de la familia LZ77 - Ventana Deslizante)
size_t compress_lz77(const uint8_t *data, size_t size, FILE *out) {
    size_t i = 0;
    size_t out_size = 0;
    while (i < size) {
        int match_length = 0;
        int match_offset = 0;
        int window_start = (i > WINDOW_SIZE) ? i - WINDOW_SIZE : 0;

        for (size_t j = window_start; j < i; j++) {
            int len = 0;
            while (len < LOOKAHEAD_SIZE && i + len < size && data[j + len] == data[i + len]) {
                len++;
            }
            if (len > match_length) {
                match_length = len;
                match_offset = (int)(i - j);
            }
        }

        if (match_length >= 3) {
            uint8_t flag = 1; // Indicador de que hubo un match en la ventana
            uint16_t token = (match_offset << 4) | (match_length & 0x0F);
            if (out) {
                fwrite(&flag, 1, 1, out);
                fwrite(&token, 2, 1, out);
            }
            out_size += 3;
            i += match_length;
        } else {
            uint8_t flag = 0; // Literal sin match
            if (out) {
                fwrite(&flag, 1, 1, out);
                fwrite(&data[i], 1, 1, out);
            }
            out_size += 2;
            i++;
        }
    }
    return out_size;
}

// Algoritmo RLE (Run-Length Encoding) - Excelente para valores idénticos continuos
size_t compress_rle(const uint8_t *data, size_t size, FILE *out) {
    size_t i = 0;
    size_t out_size = 0;
    while (i < size) {
        uint8_t count = 1;
        // Contar cuántos bytes iguales hay consecutivamente (máx 255 por bloque)
        while (i + count < size && data[i] == data[i + count] && count < 255) {
            count++;
        }
        if (out) {
            fwrite(&count, 1, 1, out);    // Escribir cantidad
            fwrite(&data[i], 1, 1, out);  // Escribir el byte
        }
        out_size += 2;
        i += count;
    }
    return out_size;
}

// Función principal de compresión con la canalización TurboQuant -> LZ
int compress_file(FILE *in, FILE *out, int algorithm) {
    fseek(in, 0, SEEK_END);
    long file_size = ftell(in);
    fseek(in, 0, SEEK_SET);

    if (file_size <= 0) return 0;

    // Asumimos un archivo binario que representa una matriz de floats
    size_t elements = file_size / sizeof(float);
    float *matrix = (float *)malloc(file_size);
    uint8_t *raw_bytes = (uint8_t *)matrix; // Puntero para tratar la matriz como bytes crudos

    if (!matrix) {
        fprintf(stderr, "Error: No hay suficiente memoria para cargar la matriz.\n");
        return -1;
    }

    if (fread(matrix, sizeof(float), elements, in) != elements) {
        fprintf(stderr, "Error al leer la matriz de entrada.\n");
        free(matrix);
        return -1;
    }

    size_t compressed_size = 0;

    if (algorithm == 1) {
        uint8_t *quantized = (uint8_t *)malloc(elements * sizeof(uint8_t));
        printf("1. %s cuantización (TurboQuant mock)...\n", out ? "Aplicando" : "Estimando");
        apply_turboquant_mock(matrix, quantized, elements);
        printf("2. %s la matriz cuantizada con LZ77...\n", out ? "Comprimiendo" : "Estimando");
        compressed_size = compress_lz77(quantized, elements, out);
        free(quantized);
    } else if (algorithm == 2) {
        printf("%s bytes crudos con LZ77 (Sin Pérdida)...\n", out ? "Comprimiendo" : "Estimando");
        compressed_size = compress_lz77(raw_bytes, file_size, out);
    } else if (algorithm == 3) {
        printf("%s bytes crudos con RLE (Sin Pérdida)...\n", out ? "Comprimiendo" : "Estimando");
        compressed_size = compress_rle(raw_bytes, file_size, out);
    } else {
        fprintf(stderr, "Error: Algoritmo desconocido.\n");
        free(matrix);
        return -1;
    }

    
    printf("\n--- Estimador de Resultados ---\n");
    printf("Tamaño original   : %ld bytes\n", file_size);
    printf("Tamaño comprimido : %zu bytes\n", compressed_size);
    printf("Reducción de peso : %.2f%%\n", (1.0 - ((double)compressed_size / file_size)) * 100.0);
    printf("-------------------------------\n");

    free(matrix);
    return 0; // 0 significa éxito
}

int main(int argc, char *argv[]) {
    int opt;
    char *input_file = NULL;
    char *output_file = NULL;
    int algorithm = 1; // Por defecto
    int estimate_only = 0;

    // Analizar los argumentos de la línea de comandos usando getopt
    while ((opt = getopt(argc, argv, "i:o:a:eh")) != -1) {
        switch (opt) {
            case 'i':
                input_file = optarg;
                break;
            case 'o':
                output_file = optarg;
                break;
            case 'a':
                algorithm = atoi(optarg);
                break;
            case 'e':
                estimate_only = 1;
                break;
            case 'h':
                print_usage(argv[0]);
                return EXIT_SUCCESS;
            default:
                print_usage(argv[0]);
                return EXIT_FAILURE;
        }
    }

    // Validar que los argumentos requeridos fueron proporcionados
    if (!input_file || (!output_file && !estimate_only)) {
        fprintf(stderr, "Error: Faltan argumentos requeridos.\n");
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    // Abrir archivos en modo binario ("rb" y "wb")
    FILE *in = fopen(input_file, "rb");
    if (!in) {
        perror("Error al abrir el archivo de entrada");
        return EXIT_FAILURE;
    }

    FILE *out = NULL;
    if (!estimate_only) {
        out = fopen(output_file, "wb");
        if (!out) {
            perror("Error al crear el archivo de salida");
            fclose(in); // Asegurar de cerrar el archivo de entrada antes de salir
            return EXIT_FAILURE;
        }
    }

    if (estimate_only) {
        printf("Estimando compresión de '%s' (Algoritmo %d)...\n", input_file, algorithm);
    } else {
        printf("Comprimiendo '%s' hacia '%s' (Algoritmo %d)...\n", input_file, output_file, algorithm);
    }
    
    // Ejecutar el proceso
    if (compress_file(in, out, algorithm) == 0) {
        printf("\n¡%s completad%s con éxito!\n", estimate_only ? "Estimación" : "Proceso", estimate_only ? "a" : "o");
    } else {
        fprintf(stderr, "Ocurrió un error al procesar el archivo.\n");
    }

    // Limpieza de recursos
    fclose(in);
    if (out) fclose(out);

    return EXIT_SUCCESS;
}
