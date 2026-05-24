# Smart Backup OS - Utility (Kernel-Space)

**Proyecto Parcial: Sistemas Operativos - EAFIT**

Una utilidad de copias de seguridad de alto rendimiento desarrollada en **C**. A diferencia de los programas tradicionales, esta herramienta interactúa directamente con el núcleo del sistema operativo (Kernel) a través de **System Calls POSIX** puras (`open`, `read`, `write`, `stat`, `opendir`), evadiendo el sobrecosto de las librerías estándar de C (`stdio.h`).

---

## 🚀 Características Principales

1. **Copia de Bajo Nivel:** Uso estricto de descriptores de archivo (`fd`) en lugar de `FILE*` para maximizar el *throughput* de Entrada/Salida (I/O).
2. **Recursividad Segura:** Capacidad de clonar árboles de directorios completos evitando ciclos infinitos mediante la detección de enlaces simbólicos (`lstat`).
3. **Preservación de Metadatos:** Copia exacta de los permisos (Modo POSIX) de los archivos y directorios originales.
4. **Tolerancia a Fallos:** Manejo robusto de escrituras parciales, detección de disco lleno (`ENOSPC`) y validación de rutas largas.
5. **Sistema de Log:** Registro de operaciones con marcas de tiempo en `smart_backup.log`, simulando el comportamiento `dmesg` del kernel.
6. **Benchmark Integrado:** Suite de pruebas para comparar el rendimiento real frente a las funciones convencionales de la librería C.
7. **Compresión al Vuelo:** Integración de algoritmos de entropía (RLE, LZ77) adaptados para operar sobre memoria RAM (Framing) sin usar `stdio.h`.

> 💡 **Nota:** Para conocer la arquitectura profunda, el manejo de memoria (SysCalls) y los códigos de error del motor, consulta el archivo [DETALLES_TECNICOS.md](DETALLES_TECNICOS.md).

---

## 🛠️ Compilación e Instalación

El proyecto utiliza `make` para automatizar su construcción. En una terminal (Linux, WSL o MinGW), ejecuta:

```bash
# Compilar el programa principal
make

# Correr todos los benchmarks automatizados
make bench

# (Opcional) Limpiar archivos binarios y temporales
make clean
```

Esto generará un archivo ejecutable llamado `backup_EAFITos`.

---

## 📖 Guía de Uso (CLI)

La herramienta se ejecuta desde la interfaz de línea de comandos (Terminal). Su sintaxis general es:

```bash
./backup_EAFITos [OPCIÓN] [ORIGEN] [DESTINO]
```

### Opciones Disponibles:

| Opción | Descripción |
| :--- | :--- |
| `-h`, `--help` | Muestra la pantalla de ayuda con ejemplos. |
| `-b`, `--backup` | Realiza el respaldo de un archivo o directorio. |
| `-c`, `--comp` | Comprime el respaldo al vuelo (requiere `-b`). |
| `-r`, `--restore` | Restaura/descomprime un respaldo (requiere `-b`). |
| `-a`, `--algo` | Algoritmo de compresión a utilizar (1: TurboQuant+LZ77, 2: LZ77, 3: RLE). Por defecto: 1. |
| `-e`, `--encrypt` | Encripta el respaldo (1: XOR, 2: RC4 Stream, 3: AES-Mock). |
| `-k`, `--key`  | Contraseña de cifrado personalizada (Default: EAFIT2026). |
| `-p`, `--perf` | Ejecuta el Benchmark de rendimiento de Entrada/Salida (Syscalls vs Stdio). |
| `-P`, `--perf-enc`| Ejecuta el Benchmark comparativo de algoritmos de Encriptación. |
| `-O`, `--perf-comp`| Ejecuta el Benchmark comparativo de algoritmos de Compresión. |
| `-g`, `--generate` | Genera un archivo dummy del tamaño indicado en MB (ej. `-g 10`). |
| `-t`, `--type` | Patrón para el archivo dummy (0: Constante, 1: Repetitivo, 2: Incremental, 3: Aleatorio). |
| `-C`, `--cc` | Compara el tamaño original vs final al terminar el proceso. |

> ⚠️ **Nota Importante sobre la Restauración (`-r`):** El motor actualmente realiza la encriptación y desencriptación bidireccional sin ningún problema. Sin embargo, las funciones de **descompresión** en memoria (ej. `mem_decompress_rle` y `mem_decompress_lz77`) se encuentran pendientes de implementación como trabajo a futuro. Si utilizas el flag `-r` sobre un archivo comprimido, la operación finalizará sin errores de I/O, pero el archivo resultante conservará su formato comprimido.

### Ejemplos de Uso Práctico:

**1. Respaldar un solo archivo:**
```bash
./backup_EAFITos -b documento.txt copia_seguridad.txt
```

**2. Respaldar un directorio completo recursivamente:**
```bash
./backup_EAFITos -b /home/usuario/proyectos /tmp/backup_proyectos
```
*Al finalizar, se imprimirá un resumen en consola detallando la cantidad de bytes transferidos, archivos procesados y posibles fallos.*

**3. Correr el Benchmark Interno:**
```bash
./backup_EAFITos --perf
```
*Este comando creará archivos "dummy" de 1KB, 1MB y 1GB, los copiará usando tanto llamadas puras del sistema (`read/write`) como la librería estándar de C (`fread/fwrite`), y arrojará una tabla comparativa del tiempo de reloj (Wall-clock time) que tomó cada método. Al finalizar, limpiará el disco automáticamente.*

---

## 🧪 Ejecutar Suite de Pruebas (Testing)

Para verificar que todas las funciones operan correctamente en tu entorno sin tener que crear archivos manualmente, utiliza:

```bash
make test
```

El Makefile automáticamente:
1. Preparará un entorno creando carpetas y archivos de prueba (`src_test`).
2. Ejecutará una prueba de archivo individual.
3. Ejecutará una prueba recursiva sobre todo el directorio.
4. Mostrará el contenido resultante para comprobación visual.

---

## 📁 Estructura del Proyecto

```text
smart-backup-so/
├── backup.c                 # Interfaz de Línea de Comandos (CLI) y Benchmark.
├── backup_engine.c          # Motor del Kernel (Lógica cruda de SysCalls).
├── smart_copy.h             # Cabecera central (Constantes, Flags y Firmas).
├── Makefile                 # Reglas de compilación y tests automatizados.
├── .gitignore               # Exclusión de binarios y logs del control de versiones.
├── smart_backup.log         # (Generado) Registro de operaciones realizadas.
└── apoyoTematico/           # Pruebas aisladas demostrando SysCalls vs LibC.
```

---

## ⚠️ Códigos de Error (Troubleshooting)

Si el motor encuentra un problema insalvable, abortará y mostrará un código de error en la consola:

- **-1 (SC_ERR_OPEN):** El archivo original no existe o está bloqueado.
- **-2 (SC_ERR_CREATE):** Imposible crear el archivo de destino (verifica permisos de la carpeta contenedora).
- **-4 (SC_ERR_WRITE):** Error al escribir. Probablemente el disco duro está lleno (`ENOSPC`).
- **-6 (SC_ERR_PERM):** No tienes permisos suficientes para leer el archivo original. Eres usuario normal intentando copiar un archivo de `root`?