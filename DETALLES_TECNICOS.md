# ⚙️ Detalles Técnicos y Arquitectura

Este documento complementa al `README.md` principal, detallando el funcionamiento interno del motor de copias de seguridad a nivel de llamadas al sistema (System Calls).

## ¿Cómo funciona bajo el capó?

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

## ⚠️ Códigos de Error (Troubleshooting)

Si el motor encuentra un problema insalvable, abortará y mostrará un código de error en la consola:

- **-1 (SC_ERR_OPEN):** El archivo original no existe o está bloqueado.
- **-2 (SC_ERR_CREATE):** Imposible crear el archivo de destino (verifica permisos de la carpeta contenedora).
- **-3 (SC_ERR_READ):** Error durante la lectura del origen.
- **-4 (SC_ERR_WRITE):** Error al escribir. Probablemente el disco duro está lleno (`ENOSPC`).
- **-5 (SC_ERR_STAT):** No se pudo obtener la información de los nodos (inodos) del archivo.
- **-6 (SC_ERR_PERM):** No tienes permisos suficientes para leer el archivo original. ¿Eres usuario normal intentando copiar un archivo de `root`?
- **-8 (SC_ERR_MKDIR):** No se pudo crear el directorio destino.
- **-9 (SC_ERR_OPENDIR):** No se pudo abrir el directorio origen.