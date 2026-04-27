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
void create_dummy_file(const char* filename, size_t size) {
    FILE *f = fopen(filename, "wb");
    if (!f) {
        perror("Error creando archivo dummy");
        return;
    }
    char buffer[BUFFER_SIZE];
    memset(buffer, 'A', BUFFER_SIZE);
    size_t written = 0;
    while (written < size) {
        size_t to_write = (size - written > BUFFER_SIZE) ? BUFFER_SIZE : (size - written);
        fwrite(buffer, 1, to_write, f);
        written += to_write;
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

    create_dummy_file("bench_1KB.bin", size_1KB);
    create_dummy_file("bench_1MB.bin", size_1MB);
    create_dummy_file("bench_1GB.bin", size_1GB);

    struct {
        const char* name;
        const char* filename;
    } tests[] = {
        {"1 KB", "bench_1KB.bin"},
        {"1 MB", "bench_1MB.bin"},
        {"1 GB", "bench_1GB.bin"}
    };

    printf("%-10s | %-20s | %-20s\n", "Tamaño", "sys_smart_copy (s)", "stdio (fread/write)");
    printf("--------------------------------------------------------------\n");

    for (int i = 0; i < 3; i++) {
        double start, end, time_syscall, time_stdio;
        
        /* 1. Test Syscall (Nuestro motor crudo) */
        start = get_current_time();
        sys_smart_copy(tests[i].filename, "bench_out_sys.bin", SCOPY_OVERWRITE, NULL);
        end = get_current_time();
        time_syscall = end - start;

        /* 2. Test Stdio (Librería estándar C) */
        start = get_current_time();
        std_copy(tests[i].filename, "bench_out_std.bin");
        end = get_current_time();
        time_stdio = end - start;

        printf("%-10s | %-20.6f | %-20.6f\n", tests[i].name, time_syscall, time_stdio);
    }

    printf("==============================================================\n");
    printf("Limpiando disco...\n");
    
    remove("bench_1KB.bin");
    remove("bench_1MB.bin");
    remove("bench_1GB.bin");
    remove("bench_out_sys.bin");
    remove("bench_out_std.bin");
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
    printf("  -p, --perf    Ejecutar benchmark de rendimiento (1KB, 1MB, 1GB).\n");
    printf("\nEjemplos:\n");
    printf("  %s -b archivo.txt backup_archivo.txt\n", prog_name);
    printf("  %s -b /home/user/documentos /tmp/backup_documentos\n\n", prog_name);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_help(argv[0]);
        return EXIT_FAILURE;
    }

    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        print_help(argv[0]);
        return EXIT_SUCCESS;
    }

    if (strcmp(argv[1], "-p") == 0 || strcmp(argv[1], "--perf") == 0) {
        run_benchmark();
        return EXIT_SUCCESS;
    }

    if (strcmp(argv[1], "-b") == 0 || strcmp(argv[1], "--backup") == 0) {
        if (argc != 4) {
            fprintf(stderr, "Error: Faltan argumentos. Forma correcta: %s -b origen destino\n", argv[0]);
            return EXIT_FAILURE;
        }

        const char *src = argv[2];
        const char *dest = argv[3];

        struct stat st;
        // Revisar si el origen existe antes de intentar copiar nada
        if (stat(src, &st) == -1) {
            perror("Error comprobando el directorio/archivo de origen");
            return EXIT_FAILURE;
        }

        /* Configurar el motor de copia: Sobrescribir, preservar permisos, verboso y log */
        CopyStats stats = {0, 0, 0, 0};
        int flags = SCOPY_OVERWRITE | SCOPY_PRESERVE | SCOPY_VERBOSE | SCOPY_LOG;
        int ret = SC_OK;

        if (S_ISDIR(st.st_mode)) {
            printf("--- Iniciando respaldo del directorio '%s' en '%s' ---\n", src, dest);
            ret = sys_smart_copy_dir(src, dest, flags, &stats);
            printf("--- Respaldo completado ---\n");
        } else if (S_ISREG(st.st_mode)) {
            printf("--- Iniciando respaldo del archivo '%s' en '%s' ---\n", src, dest);
            ret = sys_smart_copy(src, dest, flags, &stats);
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
    } else {
        fprintf(stderr, "Error: Opción no reconocida '%s'.\n", argv[1]);
        print_help(argv[0]);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
