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

    create_dummy_file("Origenes/bench_1KB.bin", size_1KB, -1);
    create_dummy_file("Origenes/bench_1MB.bin", size_1MB, -1);
    create_dummy_file("Origenes/bench_1GB.bin", size_1GB, -1);

    struct {
        const char* name;
        const char* filename;
    } tests[] = {
        {"1 KB", "Origenes/bench_1KB.bin"},
        {"1 MB", "Origenes/bench_1MB.bin"},
        {"1 GB", "Origenes/bench_1GB.bin"}
    };

    printf("%-10s | %-20s | %-20s\n", "Tamaño", "sys_smart_copy (s)", "stdio (fread/write)");
    printf("--------------------------------------------------------------\n");

    for (int i = 0; i < 3; i++) {
        double start, end, time_syscall, time_stdio;
        
        /* 1. Test Syscall (Nuestro motor crudo) */
        start = get_current_time();
        sys_smart_copy(tests[i].filename, "Resultados/bench_out_sys.bin", SCOPY_OVERWRITE, 0, 0, "", NULL);
        end = get_current_time();
        time_syscall = end - start;

        /* 2. Test Stdio (Librería estándar C) */
        start = get_current_time();
        std_copy(tests[i].filename, "Resultados/bench_out_std.bin");
        end = get_current_time();
        time_stdio = end - start;

        printf("%-10s | %-20.6f | %-20.6f\n", tests[i].name, time_syscall, time_stdio);
    }

    printf("==============================================================\n");
    printf("Limpiando disco...\n");
    
    remove("Origenes/bench_1KB.bin");
    remove("Origenes/bench_1MB.bin");
    remove("Origenes/bench_1GB.bin");
    remove("Resultados/bench_out_sys.bin");
    remove("Resultados/bench_out_std.bin");
    printf("Benchmark finalizado.\n\n");
}

/* Ejecutar la suite de pruebas enfocada en Encriptación */
void run_encryption_benchmark() {
    printf("\n================================================================================\n");
    printf("       BENCHMARK DE ENCRIPTACIÓN: XOR vs RC4 vs AES-Mock (Híbrido)      \n");
    printf("================================================================================\n");
    printf("Generando archivo de prueba (50 MB) con datos aleatorios... \n\n");

    size_t test_size = 50 * 1024 * 1024; /* 50 MB */
    const char *orig_file = "Origenes/bench_enc_50MB.bin";
    create_dummy_file(orig_file, test_size, 3); /* Patrón 3: Aleatorio */

    struct {
        int enc_algo;
        const char* name;
        const char* dest_file;
        const char* security;
    } algos[] = {
        {ENC_XOR, "XOR Dinámico", "Resultados/bench_enc_xor.bin", "BAJA"},
        {ENC_RC4, "RC4 Stream", "Resultados/bench_enc_rc4.bin", "MEDIA"},
        {ENC_AES_MOCK, "AES-Mock", "Resultados/bench_enc_aes.bin", "ALTA"}
    };

    printf("%-20s | %-12s | %-15s | %-10s\n", "Algoritmo", "Tiempo (s)", "Tamaño (bytes)", "Seguridad");
    printf("--------------------------------------------------------------------------------\n");

    for (int i = 0; i < 3; i++) {
        double start, end, time_taken;
        CopyStats stats = {0, 0, 0, 0, 0};

        start = get_current_time();
        sys_smart_copy(orig_file, algos[i].dest_file, SCOPY_OVERWRITE | SCOPY_ENCRYPT, 0, algos[i].enc_algo, "BenchmarkKey", &stats);
        end = get_current_time();
        time_taken = end - start;

        printf("%-20s | %-12.6f | %-15lld | %-10s\n", 
               algos[i].name, time_taken, (long long)stats.bytes_copied, algos[i].security);
    }

    printf("--------------------------------------------------------------------------------\n");
    printf("Conclusión del Análisis:\n");
    printf("- XOR Dinámico: Cero impacto de tamaño, extremadamente rápido, criptográficamente débil.\n");
    printf("- RC4 Stream  : Agrega 16 bytes (IV), velocidad moderada, seguridad estándar de flujo.\n");
    printf("- AES-Mock    : Agrega 16 bytes (IV), alto consumo de CPU (reflejado en el tiempo).\n");
    printf("================================================================================\n\n");

    printf("Limpiando disco...\n");
    remove(orig_file);
    for(int i = 0; i < 3; i++) remove(algos[i].dest_file);
    printf("Benchmark de encriptación finalizado.\n\n");
}

/* Ejecutar la suite de pruebas enfocada en Compresión */
void run_compression_benchmark() {
    printf("\n================================================================================\n");
    printf("       BENCHMARK DE COMPRESIÓN: TQLZ vs LZ77 vs RLE      \n");
    printf("================================================================================\n");
    printf("Generando archivo de prueba (50 MB) con datos repetitivos... \n\n");

    size_t test_size = 50 * 1024 * 1024; /* 50 MB */
    const char *orig_file = "Origenes/bench_comp_50MB.bin";
    create_dummy_file(orig_file, test_size, 1); /* Patrón 1: Repetitivo (ideal para comprimir) */

    struct {
        int algo;
        const char* name;
        const char* dest_file;
    } algos[] = {
        {ALG_TURBOQUANT_LZ, "TurboQuant+LZ77", "Resultados/bench_comp_tqlz.bin"},
        {ALG_LZ77, "LZ77 Estándar", "Resultados/bench_comp_lz77.bin"},
        {ALG_RLE, "RLE (Run-Length)", "Resultados/bench_comp_rle.bin"}
    };

    printf("%-20s | %-12s | %-15s | %-10s\n", "Algoritmo", "Tiempo (s)", "Tamaño Final", "Ahorro (%)");
    printf("--------------------------------------------------------------------------------\n");

    for (int i = 0; i < 3; i++) {
        double start, end, time_taken;
        CopyStats stats = {0, 0, 0, 0, 0};

        start = get_current_time();
        sys_smart_copy(orig_file, algos[i].dest_file, SCOPY_OVERWRITE | SCOPY_COMPRESS, algos[i].algo, 0, "", &stats);
        end = get_current_time();
        time_taken = end - start;

        double ratio = 0.0;
        if (stats.original_bytes > 0) {
            ratio = (1.0 - ((double)stats.bytes_copied / (double)stats.original_bytes)) * 100.0;
        }
        printf("%-20s | %-12.6f | %-15lld | %-9.2f%%\n", 
               algos[i].name, time_taken, (long long)stats.bytes_copied, ratio);
    }

    printf("--------------------------------------------------------------------------------\n");
    printf("Limpiando disco...\n");
    remove(orig_file);
    for(int i = 0; i < 3; i++) remove(algos[i].dest_file);
    printf("Benchmark de compresión finalizado.\n\n");
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
    printf("  -e, --encrypt [ALGO] Encripta el respaldo (1: XOR, 2: RC4 Stream, 3: AES-Mock).\n");
    printf("  -k, --key     [CLAVE] Contraseña de cifrado (Default: EAFIT2026).\n");
    printf("  -p, --perf    Ejecutar benchmark de rendimiento (1KB, 1MB, 1GB).\n");
    printf("  -P, --perf-enc Ejecutar benchmark comparativo de los algoritmos de encriptación.\n");
    printf("  -O, --perf-comp Ejecutar benchmark comparativo de algoritmos de compresión.\n");
    printf("  -g, --generate [MB] Genera un archivo dummy del tamaño indicado en MB.\n");
    printf("  -t, --type    Patrón para -g (0: Constante, 1: Repetitivo, 2: Incremental, 3: Aleatorio) [Default: 0]\n");
    printf("  -C, --cc      Compara el tamaño original vs final al terminar.\n");
    printf("\nEjemplos:\n");
    printf("  %s -b origen destino\n", prog_name);
    printf("  %s -b -c -a 3 origen destino\n\n", prog_name);
    printf("  %s -b -c -e 2 -k \"MiPassword\" origen destino_seguro.bin\n", prog_name);
    printf("  %s -g 10 Resultados/prueba.bin\n", prog_name);
    printf("  %s -g 5 -t 3 aleatorio.bin\n", prog_name);
    printf("  %s -b -r -a 3 destino_comprimido destino_restaurado\n\n", prog_name);
}

int main(int argc, char *argv[]) {
    int opt;
    int opt_backup = 0;
    int opt_perf = 0;
    int opt_perf_enc = 0;
    int opt_perf_comp = 0;
    int opt_compress = 0;
    int opt_restore = 0;
    int opt_encrypt = 0;
    int opt_generate = 0;
    size_t gen_size = 0;
    int opt_pattern = 0;
    int opt_compare = 0;
    int algo = 1; // Por defecto
    int enc_algo = 1; // Por defecto XOR Dinámico si -e se pone sin argumentos
    char enc_key[256] = "EAFIT2026"; // Clave por defecto si no pasa -k

    static struct option long_options[] = {
        {"help",   no_argument,       0,  'h' },
        {"backup", no_argument,       0,  'b' },
        {"perf",   no_argument,       0,  'p' },
        {"perf-enc",no_argument,      0,  'P' },
        {"perf-comp",no_argument,     0,  'O' },
        {"comp",   no_argument,       0,  'c' },
        {"restore",no_argument,       0,  'r' },
        {"encrypt",required_argument, 0,  'e' },
        {"key",    required_argument, 0,  'k' },
        {"algo",   required_argument, 0,  'a' },
        {"generate",required_argument,0,  'g' },
        {"type",   required_argument, 0,  't' },
        {"cc",     no_argument,       0,  'C' },
        {0, 0, 0, 0}
    };

    int option_index = 0;
    while ((opt = getopt_long(argc, argv, "hbpPOpcre:k:a:g:t:C", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'h': print_help(argv[0]); return EXIT_SUCCESS;
            case 'b': opt_backup = 1; break;
            case 'p': opt_perf = 1; break;
            case 'P': opt_perf_enc = 1; break;
            case 'O': opt_perf_comp = 1; break;
            case 'c': opt_compress = 1; break;
            case 'r': opt_restore = 1; break;
            case 'e': opt_encrypt = 1; enc_algo = atoi(optarg); break;
            case 'k': strncpy(enc_key, optarg, sizeof(enc_key)-1); break;
            case 'a': algo = atoi(optarg); break;
            case 'g': opt_generate = 1; gen_size = (size_t)atoi(optarg) * 1024 * 1024; break;
            case 't': opt_pattern = atoi(optarg); break;
            case 'C': opt_compare = 1; break;
            default: print_help(argv[0]); return EXIT_FAILURE;
        }
    }

    /* Asegurar que las carpetas existan para alojar logs, orígenes y benchs */
#if defined(_WIN32)
    mkdir("Resultados");
    mkdir("Origenes");
#else
    mkdir("Resultados", 0777);
    mkdir("Origenes", 0777);
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

        /* Forzar guardado en Resultados/ si no se especifica (creación de archivos) */
        if (strncmp(user_filename, "Resultados/", 11) == 0 || strncmp(user_filename, "Resultados\\", 11) == 0 ||
            user_filename[0] == '/' || (strlen(user_filename) > 1 && user_filename[1] == ':')) {
            snprintf(filename, sizeof(filename), "%s", user_filename);
        } else {
            snprintf(filename, sizeof(filename), "Resultados/%s", user_filename);
        }

        printf("--- Generando archivo dummy de %zu bytes en '%s' (Patrón: %d) ---\n", gen_size, filename, opt_pattern);
        create_dummy_file(filename, gen_size, opt_pattern);
        printf("--- Archivo generado exitosamente ---\n");
    }

    if (opt_perf) {
        run_benchmark();
        return EXIT_SUCCESS;
    }

    /* Modo Benchmark de Encriptación */
    if (opt_perf_enc) {
        run_encryption_benchmark();
        return EXIT_SUCCESS;
    }

    /* Modo Benchmark de Compresión */
    if (opt_perf_comp) {
        run_compression_benchmark();
        return EXIT_SUCCESS;
    }

    if (opt_backup) {
        if (optind + 1 >= argc) {
            fprintf(stderr, "Error: Faltan rutas de origen y destino.\n");
            return EXIT_FAILURE;
        }

        const char *user_src = argv[optind];
        char src[MAX_PATH_LEN];

        /* Si el usuario no especificó la carpeta Origenes/ ni una ruta absoluta, lo forzamos a Origenes/ */
        if (strncmp(user_src, "Origenes/", 9) == 0 || strncmp(user_src, "Origenes\\", 9) == 0 ||
            user_src[0] == '/' || (strlen(user_src) > 1 && user_src[1] == ':')) {
            snprintf(src, sizeof(src), "%s", user_src);
        } else {
            snprintf(src, sizeof(src), "Origenes/%s", user_src);
        }
        
        const char *user_dest = argv[optind + 1];
        char dest[MAX_PATH_LEN];

        /* Si el usuario no especificó la carpeta Resultados/ ni una ruta absoluta, lo forzamos a Resultados/ */
        if (strncmp(user_dest, "Resultados/", 11) == 0 || strncmp(user_dest, "Resultados\\", 11) == 0 ||
            user_dest[0] == '/' || (strlen(user_dest) > 1 && user_dest[1] == ':')) {
            snprintf(dest, sizeof(dest), "%s", user_dest);
        } else {
            snprintf(dest, sizeof(dest), "Resultados/%s", user_dest);
        }

        struct stat st;
        // Revisar si el origen existe antes de intentar copiar nada
        if (stat(src, &st) == -1) {
            /* Fallback 1: Buscarlo en la raíz tal como lo pasó el usuario originalmente */
            if (stat(user_src, &st) == 0) {
                snprintf(src, sizeof(src), "%s", user_src);
            } else {
                /* Fallback 2: Intentar en Resultados/ por si quiere restaurar algo procesado antes */
                char fallback_src[MAX_PATH_LEN];
                snprintf(fallback_src, sizeof(fallback_src), "Resultados/%s", user_src);
                if (stat(fallback_src, &st) == 0) {
                    printf("[INFO] Archivo origen no hallado en Origenes/, pero sí en Resultados/ (Usando ese)\n");
                    snprintf(src, sizeof(src), "%s", fallback_src);
                } else {
                    perror("Error comprobando el directorio/archivo de origen");
                    return EXIT_FAILURE;
                }
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
        if (opt_encrypt) {
            flags |= SCOPY_ENCRYPT;
            printf("--- Modo de encriptación segura activado (Algo: %d) ---\n", enc_algo);
        }
        
        int ret = SC_OK;

        if (S_ISDIR(st.st_mode)) {
            printf("--- Iniciando respaldo del directorio '%s' en '%s' ---\n", src, dest);
            ret = sys_smart_copy_dir(src, dest, flags, algo, enc_algo, enc_key, &stats);
            printf("--- Respaldo completado ---\n");
        } else if (S_ISREG(st.st_mode)) {
            printf("--- Iniciando respaldo del archivo '%s' en '%s' ---\n", src, dest);
            ret = sys_smart_copy(src, dest, flags, algo, enc_algo, enc_key, &stats);
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

        /* Mostrar análisis si usó encriptación y pidió comparar métricas */
        if (opt_compare && opt_encrypt) {
            printf("\n--- ANÁLISIS DE ENCRIPTACIÓN ---\n");
            printf("Algoritmo usado : ");
            if (enc_algo == 1) {
                printf("XOR Dinámico Multibyte\n");
                printf("Seguridad       : BAJA (Vulnerable a análisis de frecuencia)\n");
                printf("Impacto en peso : Nulo (0 bytes adicionales)\n");
            } else if (enc_algo == 2) {
                printf("RC4 Stream Cipher\n");
                printf("Seguridad       : MEDIA (Pseudoaleatorio robusto)\n");
                printf("Impacto en peso : +16 bytes (Vector de Inicialización artificial)\n");
            } else if (enc_algo == 3) {
                printf("AES-256 (Híbrido Simulado)\n");
                printf("Seguridad       : ALTA (Cifrado de grado militar simulado)\n");
                printf("Impacto en peso : +16 bytes (IV) + Alta Sobrecarga CPU\n");
            }
        }
    } else {
        print_help(argv[0]);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
