/*
 * backup_engine.c
 * ---------------------------------------------------------------------------
 * Motor de respaldo "kernel-space": implementa sys_smart_copy usando
 * exclusivamente syscalls POSIX (open, read, write, close, stat, mkdir,
 * opendir, readdir, closedir).
 *
 * NO se usa stdio.h (fopen/fread/fwrite) en este módulo — toda la E/S
 * pasa por descriptores de archivo crudos para simular el comportamiento
 * de una función de sistema.
 *
 * Proyecto Parcial: Smart Backup Kernel-Space Utility
 * Sistemas Operativos - EAFIT
 * ---------------------------------------------------------------------------
 */

#include "smart_copy.h"

#include <fcntl.h>       /* open, O_RDONLY, O_WRONLY, O_CREAT, O_TRUNC */
#include <unistd.h>      /* read, write, close                          */
#include <sys/stat.h>    /* stat, lstat, mkdir, struct stat             */
#include <sys/types.h>   /* off_t, ssize_t, mode_t                      */
#include <dirent.h>      /* opendir, readdir, closedir, DIR, dirent     */
#include <errno.h>       /* errno, EEXIST, EACCES, ENOSPC ...           */
#include <string.h>      /* strerror, strcmp, snprintf                  */
#include <stdio.h>       /* printf, fprintf, snprintf (solo para logs)  */
#include <time.h>        /* time, localtime, strftime (timestamps)      */
#include <stdint.h>      /* uint8_t, uint16_t                           */

/* =========================================================================
 * FUNCIÓN AUXILIAR: log_operation
 * ========================================================================= */

/**
 * Registra un mensaje en LOG_FILE con timestamp y nivel.
 * Simula el mecanismo printk / dmesg del kernel de Linux.
 */
void log_operation(const char *level, const char *message) {
    /* Validar punteros de entrada */
    if (level == NULL || message == NULL) return;

    /* Obtener timestamp actual */
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);

    /* Abrir el log en modo append — nunca sobreescribir entradas previas */
    int fd_log = open(LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd_log < 0) {
        /* Si no se puede escribir el log, simplemente continuar */
        return;
    }

    /* Construir la línea de log */
    char line[MAX_PATH_LEN * 2];
    int len = snprintf(line, sizeof(line),
                       "[%s] [%-5s] %s\n", timestamp, level, message);

    /* Escribir con syscall write (no fprintf) */
    if (len > 0) {
        write(fd_log, line, (size_t)len);
    }

    close(fd_log);
}

/* =========================================================================
 * FUNCIÓN AUXILIAR: print_stats
 * ========================================================================= */

/**
 * Imprime un resumen de las estadísticas de la operación de respaldo.
 */
void print_stats(const CopyStats *stats) {
    if (stats == NULL) return;

    printf("\n========================================\n");
    printf("   RESUMEN DE LA OPERACIÓN DE RESPALDO  \n");
    printf("========================================\n");
    printf("  Archivos copiados  : %ld\n", stats->files_copied);
    printf("  Archivos fallidos  : %ld\n", stats->files_failed);
    printf("  Directorios creados: %ld\n", stats->dirs_created);
    printf("  Bytes totales      : %lld bytes\n", (long long)stats->bytes_copied);
    printf("========================================\n\n");
}

/* =========================================================================
 * FUNCIONES DE COMPRESIÓN EN MEMORIA (CHUNKS)
 * ========================================================================= */

#define WINDOW_SIZE 4095
#define LOOKAHEAD_SIZE 15

static size_t mem_compress_rle(const uint8_t *data, size_t size, uint8_t *out) {
    size_t i = 0, out_size = 0;
    while (i < size) {
        uint8_t count = 1;
        while (i + count < size && data[i] == data[i + count] && count < 255) count++;
        if (out) {
            out[out_size] = count;
            out[out_size + 1] = data[i];
        }
        out_size += 2;
        i += count;
    }
    return out_size;
}

static size_t mem_compress_lz77(const uint8_t *data, size_t size, uint8_t *out) {
    size_t i = 0, out_size = 0;
    while (i < size) {
        int match_length = 0, match_offset = 0;
        int window_start = (i > WINDOW_SIZE) ? (int)(i - WINDOW_SIZE) : 0;
        for (size_t j = window_start; j < i; j++) {
            int len = 0;
            while (len < LOOKAHEAD_SIZE && i + len < size && data[j + len] == data[i + len]) len++;
            if (len > match_length) {
                match_length = len;
                match_offset = (int)(i - j);
            }
        }
        if (match_length >= 3) {
            uint8_t flag = 1;
            uint16_t token = (match_offset << 4) | (match_length & 0x0F);
            if (out) {
                out[out_size] = flag;
                out[out_size + 1] = token & 0xFF;
                out[out_size + 2] = (token >> 8) & 0xFF;
            }
            out_size += 3;
            i += match_length;
        } else {
            uint8_t flag = 0;
            if (out) {
                out[out_size] = flag;
                out[out_size + 1] = data[i];
            }
            out_size += 2;
            i++;
        }
    }
    return out_size;
}

static void apply_turboquant_mock_mem(const float *matrix, uint8_t *quantized, size_t elements) {
    if (elements == 0) return;
    float min_v = matrix[0], max_v = matrix[0];
    for (size_t i = 1; i < elements; i++) {
        if (matrix[i] < min_v) min_v = matrix[i];
        if (matrix[i] > max_v) max_v = matrix[i];
    }
    float range = (max_v - min_v == 0) ? 1.0f : (max_v - min_v);
    for (size_t i = 0; i < elements; i++) {
        quantized[i] = (uint8_t)(((matrix[i] - min_v) / range) * 255.0f);
    }
}

/* =========================================================================
 * FUNCIONES DE ENCRIPTACIÓN / SEGURIDAD
 * ========================================================================= */

/* 1. XOR Dinámico Multibyte */
static void mem_encrypt_xor_dynamic(uint8_t *data, size_t size, const char *key) {
    size_t key_len = strlen(key);
    if (key_len == 0) return;
    for (size_t i = 0; i < size; i++) {
        data[i] ^= key[i % key_len];
    }
}

/* 2. RC4 (Cifrado de flujo pseudoaleatorio) */
static void mem_encrypt_rc4(uint8_t *data, size_t size, const char *key) {
    size_t key_len = strlen(key);
    if (key_len == 0) return;
    uint8_t S[256];
    for (int i = 0; i < 256; i++) S[i] = (uint8_t)i;
    int j = 0;
    for (int i = 0; i < 256; i++) {
        j = (j + S[i] + key[i % key_len]) % 256;
        uint8_t temp = S[i]; S[i] = S[j]; S[j] = temp;
    }
    int i = 0; j = 0;
    for (size_t k = 0; k < size; k++) {
        i = (i + 1) % 256;
        j = (j + S[i]) % 256;
        uint8_t temp = S[i]; S[i] = S[j]; S[j] = temp;
        data[k] ^= S[(S[i] + S[j]) % 256];
    }
}

/* 3. AES-Mock (Simula sobrecosto computacional y usa RC4 por debajo) */
static void mem_encrypt_aes_mock(uint8_t *data, size_t size, const char *key) {
    /* Múltiples pasadas para simular el costo de CPU de un cifrado por bloques estándar */
    for (int pass = 0; pass < 3; pass++) {
        mem_encrypt_xor_dynamic(data, size, key);
    }
    mem_encrypt_rc4(data, size, key);
}

/* =========================================================================
 * FUNCIÓN PRINCIPAL: sys_smart_copy
 * ========================================================================= */

/**
 * sys_smart_copy — copia un archivo usando syscalls de bajo nivel.
 *
 * Flujo interno:
 *  1. Validar punteros (SC_ERR_NULLPTR)
 *  2. stat() sobre src para verificar existencia y permisos (SC_ERR_STAT / SC_ERR_PERM)
 *  3. open() src en modo lectura (SC_ERR_OPEN)
 *  4. open() dest en modo escritura/creación (SC_ERR_CREATE)
 *  5. Bucle read/write con buffer de BUFFER_SIZE bytes
 *     - Detecta disco lleno via errno == ENOSPC (SC_ERR_WRITE)
 *     - Detecta error de lectura (SC_ERR_READ)
 *  6. close() ambos descriptores (siempre, incluso en error)
 *  7. Actualizar CopyStats si no es NULL
 */
int sys_smart_copy(const char *src, const char *dest,
                   int flags, int algo, int enc_algo, const char *enc_key, CopyStats *stats) {

    /* --- 1. Validar que los punteros no sean NULL --- */
    if (src == NULL || dest == NULL) {
        log_operation("ERROR", "sys_smart_copy: puntero NULL recibido");
        return SC_ERR_NULLPTR;
    }

    char log_msg[MAX_PATH_LEN * 2];

    /* --- 2. stat() sobre el archivo origen --- */
    struct stat st_src;
    if (stat(src, &st_src) == -1) {
        snprintf(log_msg, sizeof(log_msg),
                 "stat() falló en '%s': %s", src, strerror(errno));
        log_operation("ERROR", log_msg);

        if (errno == EACCES) return SC_ERR_PERM;
        return SC_ERR_STAT;
    }

    /* Verificar que el origen sea un archivo regular */
    if (!S_ISREG(st_src.st_mode)) {
        snprintf(log_msg, sizeof(log_msg),
                 "'%s' no es un archivo regular", src);
        log_operation("WARN", log_msg);
        return SC_ERR_STAT;
    }

    /* Verificar permisos de lectura sobre el origen */
    if (!(st_src.st_mode & S_IRUSR)) {
        snprintf(log_msg, sizeof(log_msg),
                 "Sin permiso de lectura en '%s'", src);
        log_operation("ERROR", log_msg);
        return SC_ERR_PERM;
    }

    /* --- 3. Abrir archivo origen con syscall open() --- */
    int fd_src = open(src, O_RDONLY);
    if (fd_src < 0) {
        snprintf(log_msg, sizeof(log_msg),
                 "open() origen falló '%s': %s", src, strerror(errno));
        log_operation("ERROR", log_msg);

        if (stats != NULL) stats->files_failed++;
        if (errno == EACCES) return SC_ERR_PERM;
        return SC_ERR_OPEN;
    }

    /* --- 4. Abrir/crear archivo destino con syscall open() --- */
    /* Determinar flags de apertura según SCOPY_OVERWRITE */
    int open_flags = O_WRONLY | O_CREAT;
    if (flags & SCOPY_OVERWRITE) {
        open_flags |= O_TRUNC;   /* sobreescribir si existe */
    } else {
        open_flags |= O_EXCL;    /* fallar si ya existe */
    }

    /* Preservar permisos del original si el flag está activo */
    mode_t dest_mode = (flags & SCOPY_PRESERVE) ? st_src.st_mode : 0644;

    int fd_dest = open(dest, open_flags, dest_mode);
    if (fd_dest < 0) {
        snprintf(log_msg, sizeof(log_msg),
                 "open() destino falló '%s': %s", dest, strerror(errno));
        log_operation("ERROR", log_msg);

        close(fd_src);   /* cerrar origen antes de salir */
        if (stats != NULL) stats->files_failed++;
        if (errno == EACCES) return SC_ERR_PERM;
        return SC_ERR_CREATE;
    }

    /* --- 5. Bucle de copia con buffer de página (4 KB) --- */
    char buffer[BUFFER_SIZE];
    char comp_buffer[BUFFER_SIZE * 2]; /* Doble tamaño para prevenir desbordes si el archivo se expande al intentar comprimirlo */
    ssize_t bytes_read;
    ssize_t bytes_written;
    off_t   total_bytes = 0;
    off_t   total_original_bytes = 0;
    int     ret = SC_OK;

    /* --- 4.5. Inyección/Extracción de Vector de Inicialización (IV) --- */
    /* Esta técnica altera el peso final para demostrar que algoritmos robustos añaden metadatos */
    if (flags & SCOPY_ENCRYPT) {
        if (enc_algo == ENC_RC4 || enc_algo == ENC_AES_MOCK) {
            char iv_buffer[16] = {0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x1A, 0x1B, 
                                  0x2A, 0x2B, 0x3A, 0x3B, 0x4A, 0x4B, 0x5A, 0x5B};
            if (!(flags & SCOPY_RESTORE)) {
                /* Cifrando: Añadir IV artificial al inicio (+16 bytes de peso) */
                ssize_t w = write(fd_dest, iv_buffer, 16);
                if (w == 16) total_bytes += 16;
            } else {
                /* Restaurando: Extraer y descartar IV para que el descifrado sea exacto */
                ssize_t r = read(fd_src, iv_buffer, 16);
                if (r == 16) total_original_bytes += 16;
            }
        }
    }

    while ((bytes_read = read(fd_src, buffer, BUFFER_SIZE)) > 0) {

        total_original_bytes += bytes_read;
        char *write_buf = buffer;
        size_t write_size = (size_t)bytes_read;

        /* Compresión al vuelo bloque a bloque (Framing de Memoria) */
        if (flags & SCOPY_COMPRESS) {
            if (algo == ALG_RLE) {
                write_size = mem_compress_rle((const uint8_t *)buffer, write_size, (uint8_t *)comp_buffer);
                write_buf = comp_buffer;
            } else if (algo == ALG_LZ77) {
                write_size = mem_compress_lz77((const uint8_t *)buffer, write_size, (uint8_t *)comp_buffer);
                write_buf = comp_buffer;
            } else if (algo == ALG_TURBOQUANT_LZ) {
                size_t elements = write_size / sizeof(float);
                if (elements > 0) {
                    uint8_t tq_buffer[BUFFER_SIZE];
                    apply_turboquant_mock_mem((const float *)buffer, tq_buffer, elements);
                    write_size = mem_compress_lz77(tq_buffer, elements, (uint8_t *)comp_buffer);
                    write_buf = comp_buffer;
                }
            }
        }

        /* Encriptación al vuelo (In-place) - Ocurre DESPUÉS de comprimir */
        if (flags & SCOPY_ENCRYPT) {
            if (enc_algo == ENC_XOR) {
                mem_encrypt_xor_dynamic((uint8_t *)write_buf, write_size, enc_key);
            } else if (enc_algo == ENC_RC4) {
                mem_encrypt_rc4((uint8_t *)write_buf, write_size, enc_key);
            } else if (enc_algo == ENC_AES_MOCK) {
                mem_encrypt_aes_mock((uint8_t *)write_buf, write_size, enc_key);
            }
        }

        /* Escribir exactamente los bytes procesados/leídos */
        bytes_written = write(fd_dest, write_buf, write_size);

        if (bytes_written < 0) {
            /* Detectar disco lleno específicamente */
            if (errno == ENOSPC) {
                log_operation("ERROR", "Disco lleno durante la escritura");
            } else {
                snprintf(log_msg, sizeof(log_msg),
                         "write() falló en '%s': %s", dest, strerror(errno));
                log_operation("ERROR", log_msg);
            }
            ret = SC_ERR_WRITE;
            break;

        } else if (bytes_written != (ssize_t)write_size) {
            /* Escritura parcial — situación anómala */
            log_operation("WARN", "Escritura parcial detectada");
            ret = SC_ERR_WRITE;
            break;
        }

        total_bytes += bytes_written;
    }

    /* Verificar si el bucle terminó por error de lectura */
    if (bytes_read < 0 && ret == SC_OK) {
        snprintf(log_msg, sizeof(log_msg),
                 "read() falló en '%s': %s", src, strerror(errno));
        log_operation("ERROR", log_msg);
        ret = SC_ERR_READ;
    }

    /* --- 6. Cerrar descriptores siempre (sin importar el resultado) --- */
    close(fd_src);
    close(fd_dest);

    /* --- 7. Actualizar estadísticas y log --- */
    if (ret == SC_OK) {
        if (stats != NULL) {
            stats->files_copied++;
            stats->bytes_copied += total_bytes;
            stats->original_bytes += total_original_bytes;
        }
        if (flags & SCOPY_VERBOSE) {
            printf("[OK]  %s  →  %s  (%lld bytes)\n",
                   src, dest, (long long)total_bytes);
        }
        snprintf(log_msg, sizeof(log_msg),
                 "Copia exitosa: '%s' -> '%s' (%lld bytes)",
                 src, dest, (long long)total_bytes);
        if (flags & SCOPY_LOG) log_operation("INFO", log_msg);

    } else {
        if (stats != NULL) stats->files_failed++;
        snprintf(log_msg, sizeof(log_msg),
                 "Copia fallida: '%s' -> '%s'", src, dest);
        if (flags & SCOPY_LOG) log_operation("ERROR", log_msg);
    }

    return ret;
}

/* =========================================================================
 * FUNCIÓN SECUNDARIA: sys_smart_copy_dir
 * ========================================================================= */

/**
 * sys_smart_copy_dir — copia recursivamente un directorio completo.
 *
 * Flujo interno:
 *  1. Validar punteros
 *  2. stat() sobre src para confirmar que es un directorio
 *  3. mkdir() para crear el directorio destino
 *  4. opendir() + readdir() para recorrer el contenido
 *  5. Para cada entrada:
 *     - Si es directorio → llamada recursiva a sys_smart_copy_dir
 *     - Si es archivo regular → llamada a sys_smart_copy
 *     - Otros (links, devices) → ignorar con aviso
 *  6. closedir()
 */
int sys_smart_copy_dir(const char *src, const char *dest,
                       int flags, int algo, int enc_algo, const char *enc_key, CopyStats *stats) {

    /* --- 1. Validar punteros --- */
    if (src == NULL || dest == NULL) {
        log_operation("ERROR", "sys_smart_copy_dir: puntero NULL recibido");
        return SC_ERR_NULLPTR;
    }

    char log_msg[MAX_PATH_LEN * 2];

    /* --- 2. stat() sobre el directorio origen --- */
    struct stat st_src;
    if (stat(src, &st_src) == -1) {
        snprintf(log_msg, sizeof(log_msg),
                 "stat() falló en directorio '%s': %s", src, strerror(errno));
        log_operation("ERROR", log_msg);
        return SC_ERR_STAT;
    }

    if (!S_ISDIR(st_src.st_mode)) {
        snprintf(log_msg, sizeof(log_msg), "'%s' no es un directorio", src);
        log_operation("ERROR", log_msg);
        return SC_ERR_STAT;
    }

    /* --- 3. Crear directorio destino con mkdir() --- */
    /* 1. Limpiar bits de tipo (S_IFDIR) con máscara 07777 */
    mode_t final_mode = (flags & SCOPY_PRESERVE) ? (st_src.st_mode & 07777) : 0755;
    
    /* 2. Forzar R/W/X al dueño (S_IRWXU) temporalmente para poder copiar archivos dentro */
    mode_t temp_mode = (flags & SCOPY_PRESERVE) ? (final_mode | S_IRWXU) : final_mode;

    /* Compatibilidad multiplataforma: Windows (MinGW) solo acepta 1 argumento en mkdir */
#ifdef _WIN32
    if (mkdir(dest) == -1) {
#else
    if (mkdir(dest, temp_mode) == -1) {
#endif
        if (errno != EEXIST) {
            snprintf(log_msg, sizeof(log_msg),
                     "mkdir() falló en '%s': %s", dest, strerror(errno));
            log_operation("ERROR", log_msg);
            return SC_ERR_MKDIR;
        }
        
        /* EEXIST es aceptable SOLO si el destino es efectivamente un directorio */
        struct stat st_dest;
        if (stat(dest, &st_dest) == 0 && !S_ISDIR(st_dest.st_mode)) {
            snprintf(log_msg, sizeof(log_msg),
                     "Fallo: El destino '%s' ya existe y NO es un directorio", dest);
            log_operation("ERROR", log_msg);
            return SC_ERR_MKDIR;
        }
    } else {
        if (stats != NULL) stats->dirs_created++;
        if (flags & SCOPY_VERBOSE) {
            printf("[DIR] Creado: %s\n", dest);
        }
        snprintf(log_msg, sizeof(log_msg), "Directorio creado: '%s'", dest);
        if (flags & SCOPY_LOG) log_operation("INFO", log_msg);
    }

    /* --- 4. Abrir el directorio origen con opendir() --- */
    DIR *dir = opendir(src);
    if (dir == NULL) {
        snprintf(log_msg, sizeof(log_msg),
                 "opendir() falló en '%s': %s", src, strerror(errno));
        log_operation("ERROR", log_msg);
        return SC_ERR_OPENDIR;
    }

    /* --- 5. Recorrer entradas del directorio --- */
    struct dirent *entry;
    char path_src[MAX_PATH_LEN];
    char path_dest[MAX_PATH_LEN];
    int ret = SC_OK;

    while ((entry = readdir(dir)) != NULL) {

        /* Ignorar las entradas especiales "." y ".." */
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        /* Construir rutas completas de origen y destino */
        int src_len = snprintf(path_src,  sizeof(path_src),
                               "%s/%s", src,  entry->d_name);
        int dest_len = snprintf(path_dest, sizeof(path_dest),
                                "%s/%s", dest, entry->d_name);

        /* Validar si la ruta superó MAX_PATH_LEN y fue truncada */
        if (src_len >= sizeof(path_src) || dest_len >= sizeof(path_dest)) {
            snprintf(log_msg, sizeof(log_msg),
                     "Ruta demasiado larga ignorada: '%s/%s'", src, entry->d_name);
            log_operation("WARN", log_msg);
            if (stats != NULL) stats->files_failed++;
            continue;
        }

        /* lstat() para detectar el tipo sin seguir enlaces simbólicos */
        struct stat st_entry;
        if (lstat(path_src, &st_entry) == -1) {
            snprintf(log_msg, sizeof(log_msg),
                     "lstat() falló en '%s': %s",
                     path_src, strerror(errno));
            log_operation("WARN", log_msg);
            if (stats != NULL) stats->files_failed++;
            continue;
        }

        if (S_ISDIR(st_entry.st_mode)) {
            /* Subdirectorio → recursión */
            int sub_ret = sys_smart_copy_dir(path_src, path_dest, flags, algo, enc_algo, enc_key, stats);
            if (sub_ret != SC_OK) ret = sub_ret;

        } else if (S_ISREG(st_entry.st_mode)) {
            /* Archivo regular → copiar */
            int file_ret = sys_smart_copy(path_src, path_dest, flags, algo, enc_algo, enc_key, stats);
            if (file_ret != SC_OK) ret = file_ret;

        } else {
            /* Enlace simbólico, device, socket → ignorar */
            snprintf(log_msg, sizeof(log_msg),
                     "Entrada ignorada (tipo especial): '%s'", path_src);
            log_operation("INFO", log_msg);
            if (flags & SCOPY_VERBOSE) {
                printf("[SKIP] %s (tipo especial)\n", path_src);
            }
        }
    }

    /* --- 6. Cerrar el directorio --- */
    closedir(dir);

    /* --- 7. Restaurar permisos finales si los forzamos temporalmente --- */
    if (flags & SCOPY_PRESERVE) {
        chmod(dest, final_mode);
    }

    return ret;
}
