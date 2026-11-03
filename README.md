# AirPlay Fire

Receptor **AirPlay** para **Fire TV**: duplica la pantalla de tu iPhone en el TV, pensado para jugar
(emulación retro con control) con el menor retraso posible.

- Sin app en el iPhone: el Fire TV aparece en **Centro de Control → Duplicar pantalla**.
- Video decodificado por hardware (H.264 o H.265), siempre se muestra el cuadro más reciente.
- Audio por el TV (AAC-ELD), sin colas que acumulen retraso.
- Wi-Fi del Fire TV en modo de baja latencia mientras la app está abierta.
- Ajustes con el control remoto: calidad, fps, audio, estadísticas, nombre.

Basado en el núcleo AirPlay de [UxPlay](https://github.com/FDH2/UxPlay) (GPL-3.0).

## Instalar en el Fire TV

1. En el Fire TV: **Configuración → Mi Fire TV → Opciones para desarrolladores** → activa
   **Instalar apps desconocidas** (o "Apps de origen desconocido") para la app *Downloader*.
   Si no ves "Opciones para desarrolladores": *Mi Fire TV → Acerca de* y presiona OK 7 veces
   sobre el nombre del dispositivo.
2. Instala **Downloader** (de AFTVnews) desde la tienda de Amazon.
3. En Downloader escribe esta dirección y descarga:

   ```
   https://github.com/camiool/airplay-firestick/releases/latest/download/AirPlayFire.apk
   ```

4. Instala y abre **AirPlay Fire**.

> Si el repositorio es privado, el enlace no funcionará desde el TV. Descarga el APK desde la
> pestaña *Releases* de GitHub en tu computador e instálalo con `adb install AirPlayFire.apk`,
> o haz público el repositorio.

## Usar

1. Abre **AirPlay Fire** en el TV (debe estar abierta para recibir).
2. En el iPhone: **Centro de Control → Duplicar pantalla → «Fire TV …»**.
3. Pon el iPhone en horizontal y abre tu juego/emulador.

Controles en el TV:

| Botón | Acción |
|---|---|
| ☰ (menú) | Ajustes (en cualquier momento) |
| OK | Ajustes (solo en la pantalla de espera) |
| Atrás | Salir (dos veces si estás duplicando) |

### Calidad

| Modo | Resolución | Cuándo usarlo |
|---|---|---|
| **Fluido** (por defecto) | 1080p · H.264 | Juegos: el menor retraso |
| **Nítido** | 1440p · H.265 | Más definición con poco costo |
| **Máximo** | 4K · H.265 | Video/fotos; más retraso y más exigente para el Wi-Fi |

Después de cambiar la calidad, vuelve a elegir el TV en "Duplicar pantalla".

### Consejos para emulación retro

- **Delta** (emulador de iOS) soporta pantalla externa por AirPlay: el juego se muestra a pantalla
  completa en 16:9 en el TV y el iPhone puede quedar como control o con la pantalla al mínimo. La
  resolución que usa es la del modo elegido en esta app.
- Conecta el control al **iPhone** por Bluetooth (no al Fire TV).
- Usa Wi-Fi de **5 GHz**, con el router cerca del Fire TV. Es lo que más influye en el retraso.
- Activa el modo "Juego" de tu TV para quitar el procesamiento de imagen del televisor.
- Con *Mostrar estadísticas* ves los fps, el tiempo de decodificación y los cuadros descartados.

## Limitaciones conocidas

- Retraso típico esperado: ~80–150 ms de extremo a extremo (depende del Wi-Fi y del TV).
- No se admite "AirPlay video" de YouTube/Netflix (solo duplicar pantalla y audio). Las apps con
  DRM bloquean la duplicación.
- El audio ALAC (AirPlay solo audio, p. ej. desde Música) depende de que el Fire TV tenga ese
  decodificador; la duplicación de pantalla usa AAC-ELD, que sí está soportado.
- La app recibe solo mientras está abierta.

## Compilar

GitHub Actions compila el APK en cada push (`.github/workflows/build.yml`) y lo publica en
*Releases*. Localmente: Android SDK + NDK 27, luego `./gradlew assembleRelease`.

Estructura:

```
app/src/main/java/...      UI (Java, sin dependencias), mDNS con NsdManager
app/src/main/cpp/          puente JNI, decodificador de video/audio (NDK MediaCodec + AAudio)
app/src/main/cpp/dnssd_shim  reemplazo de dns_sd que delega en NsdManager
third_party/uxplay/lib     núcleo AirPlay de UxPlay v1.73.7
third_party/libplist       libplist 2.6.0
```

## Licencia

GPL-3.0 (heredada de UxPlay). Uso personal; AirPlay es una marca de Apple, este proyecto no está
afiliado a Apple ni a Amazon.
