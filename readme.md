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

---

## ⚙️ ¿Cómo funciona bajo el capó?

El corazón del sistema reside en `backup_engine.c`, estructurado en dos funciones principales: `sys_smart_copy` y `sys_smart_copy_dir`.

### 1. Minimización de Cambios de Contexto (Context Switches)
Toda interacción con el disco duro requiere un cambio desde el **Modo Usuario** al **Modo Kernel**. Hacer esto byte por byte destruiría el rendimiento. Nuestro motor utiliza un **Buffer de 4 KB** (`BUFFER_SIZE = 4096`).
*¿Por qué 4 KB?* Porque coincide con el tamaño estándar de una **Página de Memoria** en la arquitectura x86/Linux. Esto permite que el Kernel escriba bloques enteros en el disco de la manera más natural y eficiente posible.

### 2. Gestión de Permisos Estrictos
Al crear una copia de un directorio que originalmente era de *solo lectura* (ej. `0555`), el motor realiza lo siguiente:
1. Extrae los permisos originales limpiando la máscara (`st_mode & 07777`).
2. Crea temporalmente la carpeta destino forzando permisos de Escritura (`S_IRWXU`) para permitir que el motor inserte archivos dentro de ella.
3. Al finalizar, restaura los permisos originales usando `chmod()`.

### 3. I/O Loop (Ciclo de Copia)
La lectura y escritura utilizan las SysCalls `read()` y `write()`. Se ha implementado un sistema defensivo que:
- Valida que los `bytes_written` coincidan con los `bytes_read`.
- En caso de interrupciones o buffers llenos del kernel (escritura parcial), iterará automáticamente para terminar de vaciar el contenido remanente sin corromper el archivo.

---

## 🛠️ Compilación e Instalación

El proyecto utiliza `make` para automatizar su construcción. En una terminal (Linux, WSL o MinGW), ejecuta:

```bash
# Compilar el programa principal
make

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
| `-p`, `--perf` | Ejecuta el Benchmark de rendimiento de Entrada/Salida. |

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