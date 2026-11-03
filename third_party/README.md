# Código de terceros (vendorizado)

| Carpeta | Proyecto | Versión | Licencia |
|---|---|---|---|
| `uxplay/lib` | [UxPlay](https://github.com/FDH2/UxPlay) (núcleo AirPlay: RAOP, mirroring, FairPlay) | v1.73.7 | GPL-3.0 (partes LGPL-2.1) |
| `libplist` | [libplist](https://github.com/libimobiledevice/libplist) | 2.6.0 | LGPL-2.1 |

Los archivos `CMakeLists.txt` originales fueron eliminados; la compilación se hace desde
`app/src/main/cpp/CMakeLists.txt`. `dns_sd` (Bonjour/Avahi) se reemplaza por un shim propio que
publica los servicios usando `NsdManager` de Android (ver `app/src/main/cpp/dnssd_shim`).
