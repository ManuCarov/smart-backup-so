#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <getopt.h>

#include "smart_copy.h"

/* =========================================================================
 * SECCIÓN DE BENCHMARK (Comparativa de Rendimiento)
 * ========================================================================= */

/* Obtener tiempo en segundos con alta precisión (Wall-clock time) */
double get_current_time() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
}

/* Copia usando la librería estándar de C (stdio.h) */
int std_copy(const char *src, const char *dest) {
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    
    FILE *out = fopen(dest, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }

    char buffer[BUFFER_SIZE];
    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, in)) > 0) {
        fwrite(buffer, 1, bytes_read, out);
    }

    fclose(in);
    fclose(out);
    return 0;
}

/* Generar archivo dummy de tamaño específico en disco */
void create_dummy_file(const char* filename, size_t size, int pattern) {
    FILE *f = fopen(filename, "wb");
    if (!f) {
        perror("Error creando archivo dummy");
        return;
    }
    
    if (pattern == -1) {
        /* Comportamiento original súper rápido para el Benchmark (lleno de letras 'A') */
        char buffer[BUFFER_SIZE];
        memset(buffer, 'A', BUFFER_SIZE);
        size_t written = 0;
        while (written < size) {
            size_t to_write = (size - written > BUFFER_SIZE) ? BUFFER_SIZE : (size - written);
            fwrite(buffer, 1, to_write, f);
            written += to_write;
        }
    } else {
        /* Comportamiento unificado del Generador (Matriz de Floats) */
        srand((unsigned int)time(NULL));
        size_t written = 0;
        size_t i = 0;
        float buffer[BUFFER_SIZE / sizeof(float)];
        while (written < size) {
            size_t to_write = (size - written > BUFFER_SIZE) ? BUFFER_SIZE : (size - written);
            size_t elems = to_write / sizeof(float);
            for (size_t j = 0; j < elems; j++, i++) {
                switch (pattern) {
                    case 0: buffer[j] = 42.0f; break;
                    case 1: buffer[j] = (float)(i % 15); break;
                    case 2: buffer[j] = (float)i; break;
                    case 3: buffer[j] = ((float)rand() / (float)RAND_MAX) * 1000.0f; break;
                    default: buffer[j] = 42.0f; break;
                }
            }
            fwrite(buffer, sizeof(float), elems, f);
            written += elems * sizeof(float);
        }
    }
    fclose(f);
}

/* Ejecutar la suite de pruebas automatizada */
void run_benchmark() {
    printf("\n==============================================================\n");
    printf("       BENCHMARK DE RENDIMIENTO: SysCall vs Librería C        \n");
    printf("==============================================================\n");
    printf("Generando archivos de prueba (1KB, 1MB, 1GB)... \n");
    printf("(El archivo de 1GB puede tardar unos segundos)\n\n");

    size_t size_1KB = 1024;
    size_t size_1MB = 1024 * 1024;
    size_t size_1GB = 1024 * 1024 * 1024;

    create_dummy_file("Archivos/bench_1KB.bin", size_1KB, -1);
    create_dummy_file("Archivos/bench_1MB.bin", size_1MB, -1);
    create_dummy_file("Archivos/bench_1GB.bin", size_1GB, -1);

    struct {
        const char* name;
        const char* filename;
    } tests[] = {
        {"1 KB", "Archivos/bench_1KB.bin"},
        {"1 MB", "Archivos/bench_1MB.bin"},
        {"1 GB", "Archivos/bench_1GB.bin"}
    };

    printf("%-10s | %-20s | %-20s\n", "Tamaño", "sys_smart_copy (s)", "stdio (fread/write)");
    printf("--------------------------------------------------------------\n");

    for (int i = 0; i < 3; i++) {
        double start, end, time_syscall, time_stdio;
        
        /* 1. Test Syscall (Nuestro motor crudo) */
        start = get_current_time();
        sys_smart_copy(tests[i].filename, "Archivos/bench_out_sys.bin", SCOPY_OVERWRITE, 0, NULL);
        end = get_current_time();
        time_syscall = end - start;

        /* 2. Test Stdio (Librería estándar C) */
        start = get_current_time();
        std_copy(tests[i].filename, "Archivos/bench_out_std.bin");
        end = get_current_time();
        time_stdio = end - start;

        printf("%-10s | %-20.6f | %-20.6f\n", tests[i].name, time_syscall, time_stdio);
    }

    printf("==============================================================\n");
    printf("Limpiando disco...\n");
    
    remove("Archivos/bench_1KB.bin");
    remove("Archivos/bench_1MB.bin");
    remove("Archivos/bench_1GB.bin");
    remove("Archivos/bench_out_sys.bin");
    remove("Archivos/bench_out_std.bin");
    printf("Benchmark finalizado.\n\n");
}

void print_help(const char *prog_name) {
    printf("==========================================\n");
    printf("      SISTEMA DE BACKUP C - SysCalls      \n");
    printf("==========================================\n");
    printf("Uso: %s [OPCIÓN] [ORIGEN] [DESTINO]\n", prog_name);
    printf("\nPrueba de concepto de un sistema de copias de seguridad usando las\n");
    printf("llamadas al sistema de POSIX/Linux (open, read, write, close, mkdir, etc.).\n\n");
    printf("Opciones:\n");
    printf("  -h, --help    Muestra esta ayuda.\n");
    printf("  -b, --backup  Realiza el respaldo de un archivo o directorio recursivamente.\n");
    printf("  -c, --comp    Comprime el respaldo (requiere -b).\n");
    printf("  -r, --restore Restaura/descomprime un respaldo (requiere -b).\n");
    printf("  -a, --algo    Algoritmo de compresión (1: TQLZ, 2: LZ77, 3: RLE) [Default: 1]\n");
    printf("  -p, --perf    Ejecutar benchmark de rendimiento (1KB, 1MB, 1GB).\n");
    printf("  -g, --generate [MB] Genera un archivo dummy del tamaño indicado en MB.\n");
    printf("  -t, --type    Patrón para -g (0: Constante, 1: Repetitivo, 2: Incremental, 3: Aleatorio) [Default: 0]\n");
    printf("  -C, --cc      Compara el tamaño original vs final al terminar.\n");
    printf("\nEjemplos:\n");
    printf("  %s -b origen destino\n", prog_name);
    printf("  %s -b -c -a 3 origen destino\n\n", prog_name);
    printf("  %s -g 10 Archivos/prueba.bin\n", prog_name);
    printf("  %s -g 5 -t 3 aleatorio.bin\n", prog_name);
    printf("  %s -b -r -a 3 destino_comprimido destino_restaurado\n\n", prog_name);
}

int main(int argc, char *argv[]) {
    int opt;
    int opt_backup = 0;
    int opt_perf = 0;
    int opt_compress = 0;
    int opt_restore = 0;
    int opt_generate = 0;
    size_t gen_size = 0;
    int opt_pattern = 0;
    int opt_compare = 0;
    int algo = 1; // Por defecto

    static struct option long_options[] = {
        {"help",   no_argument,       0,  'h' },
        {"backup", no_argument,       0,  'b' },
        {"perf",   no_argument,       0,  'p' },
        {"comp",   no_argument,       0,  'c' },
        {"restore",no_argument,       0,  'r' },
        {"algo",   required_argument, 0,  'a' },
        {"generate",required_argument,0,  'g' },
        {"type",   required_argument, 0,  't' },
        {"cc",     no_argument,       0,  'C' },
        {0, 0, 0, 0}
    };

    int option_index = 0;
    while ((opt = getopt_long(argc, argv, "hbpcra:g:t:C", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'h': print_help(argv[0]); return EXIT_SUCCESS;
            case 'b': opt_backup = 1; break;
            case 'p': opt_perf = 1; break;
            case 'c': opt_compress = 1; break;
            case 'r': opt_restore = 1; break;
            case 'a': algo = atoi(optarg); break;
            case 'g': opt_generate = 1; gen_size = (size_t)atoi(optarg) * 1024 * 1024; break;
            case 't': opt_pattern = atoi(optarg); break;
            case 'C': opt_compare = 1; break;
            default: print_help(argv[0]); return EXIT_FAILURE;
        }
    }

    /* Asegurar que la carpeta Archivos exista para alojar logs y benchs */
#if defined(_WIN32)
    mkdir("Archivos");
#else
    mkdir("Archivos", 0777);
#endif

    /* Modo Generador de Archivos Dummy */
    if (opt_generate) {
        if (optind >= argc) {
            fprintf(stderr, "Error: Falta la ruta del archivo a generar.\n");
            fprintf(stderr, "Uso: %s -g <MB> <ruta_archivo>\n", argv[0]);
            return EXIT_FAILURE;
        }
        
        const char *user_filename = argv[optind];
        char filename[MAX_PATH_LEN];

        /* Forzar guardado en Archivos/ si no se especifica */
        if (strncmp(user_filename, "Archivos/", 9) == 0 || strncmp(user_filename, "Archivos\\", 9) == 0 ||
            user_filename[0] == '/' || (strlen(user_filename) > 1 && user_filename[1] == ':')) {
            snprintf(filename, sizeof(filename), "%s", user_filename);
        } else {
            snprintf(filename, sizeof(filename), "Archivos/%s", user_filename);
        }

        printf("--- Generando archivo dummy de %zu bytes en '%s' (Patrón: %d) ---\n", gen_size, filename, opt_pattern);
        create_dummy_file(filename, gen_size, opt_pattern);
        printf("--- Archivo generado exitosamente ---\n");
    }

    if (opt_perf) {
        run_benchmark();
        return EXIT_SUCCESS;
    }

    if (opt_backup) {
        if (optind + 1 >= argc) {
            fprintf(stderr, "Error: Faltan rutas de origen y destino.\n");
            return EXIT_FAILURE;
        }

        const char *user_src = argv[optind];
        char src[MAX_PATH_LEN];
        snprintf(src, sizeof(src), "%s", user_src);
        const char *user_dest = argv[optind + 1];
        char dest[MAX_PATH_LEN];

        /* Si el usuario no especificó la carpeta Archivos/ ni una ruta absoluta, lo forzamos a Archivos/ */
        if (strncmp(user_dest, "Archivos/", 9) == 0 || strncmp(user_dest, "Archivos\\", 9) == 0 ||
            user_dest[0] == '/' || (strlen(user_dest) > 1 && user_dest[1] == ':')) {
            snprintf(dest, sizeof(dest), "%s", user_dest);
        } else {
            snprintf(dest, sizeof(dest), "Archivos/%s", user_dest);
        }

        struct stat st;
        // Revisar si el origen existe antes de intentar copiar nada
        if (stat(src, &st) == -1) {
            /* Fallback: intentar buscarlo automáticamente en Archivos/ si no está en la raíz */
            char fallback_src[MAX_PATH_LEN];
            snprintf(fallback_src, sizeof(fallback_src), "Archivos/%s", user_src);
            if (stat(fallback_src, &st) == 0) {
                snprintf(src, sizeof(src), "%s", fallback_src);
            } else {
                perror("Error comprobando el directorio/archivo de origen");
                return EXIT_FAILURE;
            }
        }

        /* Configurar el motor de copia: Sobrescribir, preservar permisos, verboso y log */
        CopyStats stats = {0, 0, 0, 0, 0};
        int flags = SCOPY_OVERWRITE | SCOPY_PRESERVE | SCOPY_VERBOSE | SCOPY_LOG;
        if (opt_compress) {
            flags |= SCOPY_COMPRESS;
            printf("--- Modo de compresión activado (Algoritmo: %d) ---\n", algo);
        } else if (opt_restore) {
            flags |= SCOPY_RESTORE;
            printf("--- Modo de restauración activado (Algoritmo: %d) ---\n", algo);
        }
        
        int ret = SC_OK;

        if (S_ISDIR(st.st_mode)) {
            printf("--- Iniciando respaldo del directorio '%s' en '%s' ---\n", src, dest);
            ret = sys_smart_copy_dir(src, dest, flags, algo, &stats);
            printf("--- Respaldo completado ---\n");
        } else if (S_ISREG(st.st_mode)) {
            printf("--- Iniciando respaldo del archivo '%s' en '%s' ---\n", src, dest);
            ret = sys_smart_copy(src, dest, flags, algo, &stats);
            printf("--- Respaldo completado ---\n");
        } else {
            fprintf(stderr, "Error: El origen no es válido. Debe ser carpeta o archivo.\n");
            return EXIT_FAILURE;
        }

        /* Imprimir las métricas recolectadas por nuestro motor */
        print_stats(&stats);

        /* Validar si hubo algún problema reportado por el engine */
        if (ret != SC_OK) {
            fprintf(stderr, "\n[ATENCIÓN] El respaldo finalizó con errores (Código: %d)\n", ret);
            return EXIT_FAILURE;
        }

        /* Mostrar comparación de tamaños si el usuario lo solicitó */
        if (opt_compare) {
            printf("\n--- COMPARACIÓN DE TAMAÑOS ---\n");
            printf("Tamaño Original : %lld bytes\n", (long long)stats.original_bytes);
            printf("Tamaño Final    : %lld bytes\n", (long long)stats.bytes_copied);
            if (stats.original_bytes > 0) {
                double ratio = (1.0 - ((double)stats.bytes_copied / (double)stats.original_bytes)) * 100.0;
                if (ratio > 0) {
                    printf("Ahorro de espacio: %.2f%%\n", ratio);
                } else {
                    printf("Aumento de tamaño: %.2f%%\n", -ratio);
                }
            }
            printf("------------------------------\n");
        }
    } else {
        print_help(argv[0]);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
