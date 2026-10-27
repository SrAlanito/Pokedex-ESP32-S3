# Pokédex

**v1.0.0** — Pokédex libre y sin conexión para ESP32-S3. Consulta las especies 1 a 493, juega a «¿Quién es ese Pokémon?» y genera contraseñas Wonder Mail de Pokémon Mystery Dungeon: Red/Blue Rescue Team.

La interfaz del aparato está en español. El aparato no se conecta a internet: los datos se empaquetan al compilar.

Este proyecto no está afiliado a Nintendo, The Pokémon Company ni Spike Chunsoft. Pokémon es una marca de sus respectivos dueños.

## Qué incluye

- Pokédex nacional (`001`–`493`) y por región: Kanto, Johto, Hoenn y Sinnoh, con nombre, tipos, estadísticas y debilidades de la tabla de la cuarta generación.
- ¿Quién es ese Pokémon?, con filtro de regiones antes de jugar, silueta e ilustración al revelarla.
- Wonder Mail de rescate. Mazmorra, piso y recompensa se eligen en listas propias. El cliente y el mensaje salen al azar.

## Hardware

- Módulo ESP32-S3 con 16 MB de flash y 8 MB de PSRAM octal (N16R8).
- Pantalla ST7789 de 240×280.
- Joystick analógico con pulsador.

La alimentación de la pantalla y del joystick es 3,3 V. El monitor serie va a 115200 baudios por el USB del puente UART. El USB nativo del chip no se usa.

## Conexiones

| Señal | GPIO | Notas |
| --- | --- | --- |
| ST7789 BLK | 9 | Retroiluminación, activa en alto |
| ST7789 CS | 10 | |
| ST7789 MOSI / SDA | 11 | |
| ST7789 SCLK / SCL | 12 | |
| ST7789 DC | 13 | |
| ST7789 RST / RES | 14 | |
| ST7789 VCC | 3V3 | |
| ST7789 GND | GND | |
| Joystick VRx | 4 | Eje horizontal, ADC |
| Joystick VRy | 5 | Eje vertical, ADC |
| Joystick SW | 6 | Pulsador a GND, activo en bajo |
| Joystick VCC | 3V3 | Aunque el módulo diga 5 V |
| Joystick GND | GND | |
| CS de microSD | 7 | Opcional. Se mantiene en alto para que un lector en el mismo SPI no responda |
| SPI MISO | 21 | Sin conectar. Evita que el bus asigne MISO sobre MOSI |

No conectes nada a GPIO35, GPIO36 ni GPIO37: en el módulo N16R8 son el bus de la PSRAM octal. GPIO43 y GPIO44 son el UART de programación. GPIO19 y GPIO20 son el USB nativo. GPIO0 es el botón de arranque.

## Controles

- En el menú principal, izquierda y derecha cambian de tarjeta.
- Arriba y abajo recorren listas y especies. Si se mantiene, el paso se acelera y la lista da la vuelta.
- Izquierda vuelve atrás (en el menú principal no; ahí es solo desplazamiento).
- Derecha salta especies en la Pokédex, o avanza de golpe en las listas largas de Wonder Mail.
- El pulsador entra, marca regiones, confirma una opción de la lista, revela la silueta o genera la contraseña.

La intro de la Pokébola se puede saltar con el pulsador.

## Compilar y grabar

Hace falta [PlatformIO](https://platformio.org/), Python 3 y [Pillow](https://pypi.org/project/pillow/). La primera compilación descarga de PokéAPI los datos y las ilustraciones oficiales. Esa descarga queda en caché local y no forma parte del repositorio.

```bash
pip install pillow
pio run -e esp32-s3-n16r8 -t upload
```

`tools/patch_tft.py` corrige el divisor de reloj SPI de Arduino core 3 en la copia local de TFT_eSPI. Sin ese ajuste, `tft.init()` no regresa y la pantalla se queda en negro.

## Datos

`data/dex.bin` se genera al compilar. Contiene nombres, tipos, estadísticas e ilustraciones en JPEG de 216×216. No se versiona: las ilustraciones oficiales no están cubiertas por la licencia de este código.

`src/mail_data.h` trae las mazmorras, recompensas y clientes de Rescue Team que usa el generador. Si no está la fuente original junto al proyecto, la compilación conserva ese archivo.

## Licencia y créditos

El código de este repositorio se publica bajo la licencia MIT. Mira `LICENSE`.

- Datos e ilustraciones: [PokéAPI](https://pokeapi.co/). Se descargan al compilar. No se redistribuyen aquí y siguen bajo sus propios términos. Las ilustraciones oficiales de Pokémon no son software libre.
- Codificador Wonder Mail: Peter O., The Ultimate Pokemon Center, 2007.
- Pantalla: [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI), de Bodmer.
- JPEG: [TJpg_Decoder](https://github.com/Bodmer/TJpg_Decoder), de Bodmer, sobre TJpgDec de ChaN.
- Soporte: Arduino-ESP32, de Espressif.
