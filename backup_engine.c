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

#define _GNU_SOURCE   /* Habilita madvise, ftruncate, lstat y MADV_* */

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
#include <stdint.h>      /* Tipos enteros para compresión (uint8_t)     */
#include <sys/mman.h>    /* mmap, munmap, msync — mapeo en memoria      */
#include <stdlib.h>      /* malloc, free — gestión dinámica de memoria  */

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
 * FUNCIONES DE COMPRESIÓN EN MEMORIA (In-memory Compression)
 * ========================================================================= */

/**
 * compress_rle_buffer — Comprime un bloque de datos usando Run-Length Encoding.
 * En lugar de usar fwrite (stdio), trabaja puramente con buffers en memoria RAM
 * para mantener compatibilidad con las SysCalls crudas del motor.
 *
 * @param in_data   Buffer con los datos originales leídos del disco.
 * @param in_size   Cantidad de bytes reales en in_data.
 * @param out_data  Buffer donde se escribirán los datos comprimidos.
 * @return          Cantidad de bytes que resultaron de la compresión.
 */
size_t compress_rle_buffer(const char *in_data, size_t in_size, char *out_data) {
    size_t i = 0, out_size = 0;
    while (i < in_size) {
        unsigned char count = 1;
        /* Contar bytes repetidos consecutivos (máx 255 por bloque) */
        while (i + count < in_size && in_data[i] == in_data[i + count] && count < 255) {
            count++;
        }
        out_data[out_size++] = (char)count;     /* Guardar cantidad */
        out_data[out_size++] = in_data[i];      /* Guardar el byte */
        i += count;
    }
    return out_size;
}

size_t decompress_rle_buffer(const char *in_data, size_t in_size, char *out_data) {
    size_t i = 0, out_size = 0;
    while (i + 1 < in_size) {
        unsigned char count = (unsigned char)in_data[i++];
        unsigned char val = (unsigned char)in_data[i++];
        for (int j = 0; j < count; j++) out_data[out_size++] = val;
    }
    return out_size;
}

#define LZ77_WINDOW_SIZE 4095
#define LZ77_LOOKAHEAD_SIZE 15

size_t compress_lz77_buffer(const char *in_data, size_t in_size, char *out_data) {
    size_t i = 0, out_size = 0;
    const uint8_t *data = (const uint8_t *)in_data;
    uint8_t *out = (uint8_t *)out_data;

    while (i < in_size) {
        int match_length = 0, match_offset = 0;
        int window_start = (i > LZ77_WINDOW_SIZE) ? i - LZ77_WINDOW_SIZE : 0;

        for (size_t j = window_start; j < i; j++) {
            int len = 0;
            while (len < LZ77_LOOKAHEAD_SIZE && i + len < in_size && data[j + len] == data[i + len]) len++;
            if (len > match_length) {
                match_length = len;
                match_offset = (int)(i - j);
            }
        }

        if (match_length >= 3) {
            out[out_size++] = 1; /* flag match */
            uint16_t token = (match_offset << 4) | (match_length & 0x0F);
            out[out_size++] = token & 0xFF;         /* byte bajo */
            out[out_size++] = (token >> 8) & 0xFF;  /* byte alto */
            i += match_length;
        } else {
            out[out_size++] = 0; /* flag literal */
            out[out_size++] = data[i];
            i++;
        }
    }
    return out_size;
}

size_t decompress_lz77_buffer(const char *in_data, size_t in_size, char *out_data) {
    size_t i = 0, out_size = 0;
    const uint8_t *data = (const uint8_t *)in_data;
    uint8_t *out = (uint8_t *)out_data;

    while (i < in_size) {
        uint8_t flag = data[i++];
        if (flag == 1) {
            if (i + 1 >= in_size) break; /* Seguridad contra lectura fuera de límites */
            uint16_t token = data[i++];
            token |= (data[i++] << 8);
            int match_offset = token >> 4;
            int match_length = token & 0x0F;
            for (int j = 0; j < match_length; j++) {
                if (out_size >= match_offset) {
                    out[out_size] = out[out_size - match_offset];
                    out_size++;
                }
            }
        } else {
            if (i >= in_size) break;
            out[out_size++] = data[i++];
        }
    }
    return out_size;
}

/* =========================================================================
 * FUNCIONES DE ENCRIPTACIÓN / SEGURIDAD
 * ========================================================================= */

/**
 * mem_encrypt_decrypt_xor — Encripta o desencripta in-place usando XOR.
 * Es una operación simétrica: f(f(x)) = x.
 */
static void mem_encrypt_decrypt_xor(uint8_t *data, size_t size) {
    const uint8_t key = 0x5A; /* Clave secreta estática (90 en decimal) */
    for (size_t i = 0; i < size; i++) {
        data[i] ^= key;
    }
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
                   int flags, int algo, CopyStats *stats) {

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
    /*
     * GESTIÓN DINÁMICA DE MEMORIA:
     * Usamos malloc() en lugar de buffers en el stack por dos razones:
     *   1. El stack tiene tamaño limitado (~8MB típico). Para archivos grandes
     *      o llamadas recursivas profundas, stack buffers de 8KB+ son riesgosos.
     *   2. malloc() permite al OS asignar memoria en páginas alineadas,
     *      optimizando el DMA y el acceso al bus I/O.
     */
    char *buffer     = (char *)malloc(BUFFER_SIZE);
    char *comp_buffer = (char *)malloc(BUFFER_SIZE * 2);

    if (!buffer || !comp_buffer) {
        free(buffer);
        free(comp_buffer);
        close(fd_src);
        close(fd_dest);
        if (stats != NULL) stats->files_failed++;
        return SC_ERR_WRITE; /* reutilizamos como error de recurso */
    }
    
    ssize_t bytes_read;
    ssize_t bytes_written;
    off_t   total_bytes = 0;
    off_t   current_offset = 0;
    off_t   total_src_size = st_src.st_size;
    int     ret = SC_OK;

    while (1) {
        if (flags & SCOPY_RESTORE) {
            /* --- MODO RESTAURACIÓN (Lectura de bloques entrelazados) --- */
            uint8_t header[6];
            bytes_read = read(fd_src, header, 6);
            if (bytes_read == 0) break; /* Fin de archivo */
            
            /* Validar firma de seguridad (Magic Number 'S', 'B') y longitud */
            if (bytes_read < 6 || header[0] != 0x53 || header[1] != 0x42) { 
                log_operation("ERROR", "Archivo no reconocido como respaldo comprimido válido");
                ret = SC_ERR_READ; 
                break; 
            }
            
            current_offset += bytes_read;
            
            uint16_t orig_size = (uint16_t)header[2] | ((uint16_t)header[3] << 8);
            uint16_t comp_size = (uint16_t)header[4] | ((uint16_t)header[5] << 8);
            
            /* Prevención crítica de Buffer Overflow */
            if (comp_size > BUFFER_SIZE * 2) {
                log_operation("ERROR", "El tamaño del bloque reportado excede el límite seguro de memoria");
                ret = SC_ERR_READ;
                break;
            }

            bytes_read = read(fd_src, comp_buffer, comp_size);
            if (bytes_read != comp_size) { ret = SC_ERR_READ; break; }
            current_offset += bytes_read;
            
            if (flags & SCOPY_ENCRYPT) {
                mem_encrypt_decrypt_xor((uint8_t *)comp_buffer, comp_size);
            }

            size_t decomp_size = 0;
            if (algo == ALG_RLE) {
                decomp_size = decompress_rle_buffer(comp_buffer, comp_size, buffer);
            } else if (algo == ALG_LZ77 || algo == ALG_TURBOQUANT_LZ) {
                decomp_size = decompress_lz77_buffer(comp_buffer, comp_size, buffer);
            } else {
                memcpy(buffer, comp_buffer, comp_size);
                decomp_size = comp_size;
            }
            
            bytes_written = write(fd_dest, buffer, decomp_size);
            if (bytes_written < 0) { ret = SC_ERR_WRITE; break; }
            total_bytes += bytes_written;
            
        } else {
            /* --- MODO NORMAL O COMPRESIÓN --- */
            bytes_read = read(fd_src, buffer, BUFFER_SIZE);
            if (bytes_read == 0) break; /* Fin de archivo */
            if (bytes_read < 0) { ret = SC_ERR_READ; break; }
            current_offset += bytes_read;
            
            if (flags & SCOPY_COMPRESS) {
                size_t comp_size = 0;
                if (algo == ALG_RLE) {
                    comp_size = compress_rle_buffer(buffer, (size_t)bytes_read, comp_buffer);
                } else if (algo == ALG_LZ77 || algo == ALG_TURBOQUANT_LZ) {
                    /* Mapeamos TurboQuant a LZ77 por ahora al trabajar con streams binarios crudos */
                    comp_size = compress_lz77_buffer(buffer, (size_t)bytes_read, comp_buffer);
                } else {
                    memcpy(comp_buffer, buffer, bytes_read);
                    comp_size = bytes_read;
                }
                
                /* Escribir cabecera (Framing) para poder identificar tamaño al restaurar */
                uint8_t header[6];
                header[0] = 0x53; /* 'S' */
                header[1] = 0x42; /* 'B' */
                header[2] = (uint8_t)(bytes_read & 0xFF);
                header[3] = (uint8_t)((bytes_read >> 8) & 0xFF);
                header[4] = (uint8_t)(comp_size & 0xFF);
                header[5] = (uint8_t)((comp_size >> 8) & 0xFF);
                write(fd_dest, header, 6);
                
                if (flags & SCOPY_ENCRYPT) {
                    mem_encrypt_decrypt_xor((uint8_t *)comp_buffer, comp_size);
                }
                bytes_written = write(fd_dest, comp_buffer, comp_size);
            } else {
                if (flags & SCOPY_ENCRYPT) {
                    mem_encrypt_decrypt_xor((uint8_t *)buffer, (size_t)bytes_read);
                }
                bytes_written = write(fd_dest, buffer, (size_t)bytes_read);
            }
            
            if (bytes_written < 0) {
                if (errno == ENOSPC) log_operation("ERROR", "Disco lleno durante la escritura");
                ret = SC_ERR_WRITE;
                break;
            }
            total_bytes += bytes_written;
        }

        /* --- Barra de Progreso (Se actualiza por cada bloque) --- */
        if ((flags & SCOPY_VERBOSE) && total_src_size > 0) {
            int percent = (int)((current_offset * 100) / total_src_size);
            if (percent > 100) percent = 100;
            printf("\r[PROGRESO] %3d%% [", percent);
            int bar_width = 40;
            int pos = (percent * bar_width) / 100;
            for (int p = 0; p < bar_width; ++p) {
                if (p < pos) printf("=");
                else if (p == pos) printf(">");
                else printf(" ");
            }
            printf("] %lld / %lld B", (long long)current_offset, (long long)total_src_size);
            fflush(stdout); /* Forzar impresión inmediata en pantalla */
        }
    }

    if ((flags & SCOPY_VERBOSE) && total_src_size > 0) {
        printf("\n"); /* Salto de línea para no sobreescribir la barra al terminar */
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

    /* Liberar buffers dinámicos */
    free(buffer);
    free(comp_buffer);

    /* --- 7. Actualizar estadísticas y log --- */
    if (ret == SC_OK) {
        if (stats != NULL) {
            stats->files_copied++;
            stats->bytes_copied += total_bytes;
            stats->original_bytes += st_src.st_size;
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
                       int flags, int algo, CopyStats *stats) {

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
            int sub_ret = sys_smart_copy_dir(path_src, path_dest, flags, algo, stats);
            if (sub_ret != SC_OK) ret = sub_ret;

        } else if (S_ISREG(st_entry.st_mode)) {
            /* Archivo regular → copiar */
            int file_ret = sys_smart_copy(path_src, path_dest, flags, algo, stats);
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

/* =========================================================================
 * FUNCIÓN: sys_mmap_copy
 *
 * Copia un archivo usando mmap() en lugar del bucle read()/write().
 *
 * DIFERENCIA ARQUITECTÓNICA vs sys_smart_copy:
 *
 *   sys_smart_copy (read/write con buffer 4KB):
 *     - Cada read()  = 1 syscall = 1 context switch user→kernel
 *     - Cada write() = 1 syscall = 1 context switch user→kernel
 *     - Para un archivo de 50MB: ~12.800 pares read/write = ~25.600 c.s.
 *
 *   sys_mmap_copy (mapeo en memoria):
 *     - 2 syscalls mmap() + 1 memcpy() + 1 msync()
 *     - El kernel gestiona el I/O mediante page faults (demand paging)
 *     - Para un archivo de 50MB: ~4 syscalls explícitas (~25.600 menos)
 *
 *   strace -c validará esta diferencia empíricamente.
 * ========================================================================= */
int sys_mmap_copy(const char *src, const char *dest, CopyStats *stats) {
    if (!src || !dest) return SC_ERR_NULLPTR;

    /* 1. Obtener tamaño del archivo origen */
    struct stat st;
    if (stat(src, &st) == -1) return SC_ERR_STAT;

    /* Caso especial: archivo vacío */
    if (st.st_size == 0) {
        int fd = open(dest, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) return SC_ERR_CREATE;
        close(fd);
        if (stats) stats->files_copied++;
        return SC_OK;
    }

    /* 2. Abrir origen en solo lectura */
    int fd_src = open(src, O_RDONLY);
    if (fd_src < 0) return SC_ERR_OPEN;

    /*
     * 3. Mapear el archivo origen en memoria virtual.
     *    MAP_PRIVATE: cambios no se propagan (copy-on-write).
     *    El proceso puede leer src_map[0..st.st_size) directamente.
     *    El kernel trae las páginas del disco bajo demanda (page faults).
     */
    void *src_map = mmap(NULL, (size_t)st.st_size,
                         PROT_READ, MAP_PRIVATE, fd_src, 0);
    close(fd_src); /* fd puede cerrarse; el mapeo permanece */

    if (src_map == MAP_FAILED) return SC_ERR_OPEN;

    /* 4. Crear/abrir archivo destino */
    int fd_dest = open(dest, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd_dest < 0) {
        munmap(src_map, (size_t)st.st_size);
        return SC_ERR_CREATE;
    }

    /* 5. Extender el archivo destino al tamaño requerido */
    if (ftruncate(fd_dest, st.st_size) == -1) {
        close(fd_dest);
        munmap(src_map, (size_t)st.st_size);
        return SC_ERR_WRITE;
    }

    /*
     * 6. Mapear el archivo destino en memoria para escritura.
     *    MAP_SHARED: los cambios SÍ se propagan al archivo en disco.
     */
    void *dest_map = mmap(NULL, (size_t)st.st_size,
                          PROT_WRITE, MAP_SHARED, fd_dest, 0);
    close(fd_dest);

    if (dest_map == MAP_FAILED) {
        munmap(src_map, (size_t)st.st_size);
        return SC_ERR_CREATE;
    }

    /*
     * 7. Transferir datos: una llamada a memcpy() en lugar de miles de
     *    read()/write(). El kernel optimiza esto a nivel de página.
     *    madvise() le indica al kernel que la lectura es secuencial.
     */
    madvise(src_map,  (size_t)st.st_size, MADV_SEQUENTIAL);
    madvise(dest_map, (size_t)st.st_size, MADV_SEQUENTIAL);
    memcpy(dest_map, src_map, (size_t)st.st_size);

    /* 8. Forzar escritura a disco (flush de páginas sucias) */
    msync(dest_map, (size_t)st.st_size, MS_SYNC);

    /* 9. Liberar mapeos */
    munmap(src_map,  (size_t)st.st_size);
    munmap(dest_map, (size_t)st.st_size);

    /* 10. Actualizar estadísticas */
    if (stats) {
        stats->files_copied++;
        stats->bytes_copied    += st.st_size;
        stats->original_bytes  += st.st_size;
    }

    return SC_OK;
}
