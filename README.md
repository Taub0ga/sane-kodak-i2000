# Kodak i2000 series on Linux

A native Linux driver and SANE backend for the **Kodak i2000 family** of
document scanners (i2400 / i2600 / i2800), reverse-engineered from the
discontinued vendor driver. Colour and greyscale, simplex and duplex,
autocrop, the operator panel's display and buttons, and a daemon that turns
the green button into a PDF in [paperless-ngx](https://docs.paperless-ngx.com/).

Tested on a **Kodak i2600 (USB `040a:601d`)**. The vendor's last Linux driver
was released in December 2016 for Ubuntu 16.04, is closed source, and no
longer builds or runs on a current system. This project replaces it; the only
thing still taken from it is the scanner's own firmware, which the device
needs and which is loaded from your copy of the vendor package at run time.

The complete USB protocol is documented in [`PROTOKOLL.md`](PROTOKOLL.md),
[`INITIALISIERUNG.md`](INITIALISIERUNG.md) and
[`native/NATIVES-BACKEND.md`](native/NATIVES-BACKEND.md) — in German, with
evidence markers throughout, so every claim can be traced to a measurement, a
piece of vendor code, or a test on the device. If you own any scanner of this
family, that documentation is probably more useful to you than our code.

---

## Was das ist

Ein von Grund auf neu geschriebener Linux-Treiber für den Kodak i2600. Der
letzte offizielle Treiber stammt von Dezember 2016, ist closed source und
läuft auf keinem aktuellen System mehr. Dieses Projekt ersetzt ihn
vollständig — bis auf die Firmware des Geräts, die aus dem Herstellerpaket
gelesen wird.

### Was funktioniert

| | |
|---|---|
| Kaltstart | Firmware für den Cypress FX2 und den FPGA-Bitstrom |
| Scannen | Simplex und Duplex, beide Kameras parallel gelesen |
| Farbe | 24 bit Farbe und 8 bit Grau, 300 dpi |
| Bild | Autobeschnitt, wahlweise 180-Grad-Drehung |
| Shading | Werkskalibrierung aus dem Geräte-Flash ins SDRAM |
| SANE | `scanimage` und `simple-scan` sprechen mit dem Gerät |
| Bedienfeld | Die drei Tasten als SANE-Sensoren, eigener Meldungstext |
| paperless | `i2600-scand`: Knopfdruck, PDF, Energiesparmodus |

Nicht möglich ist bisher eine andere Auflösung als 300 dpi.

## Voraussetzung: das Herstellerpaket

Der Scanner lädt seine Firmware bei jedem Kaltstart vom Rechner. Die Images
stecken im Herstellerpaket, das **nicht** Teil dieses Repositorys ist und
nicht sein darf. Besorge es dir selbst — Prüfsummen und Fundstellen stehen in
[`HERSTELLERTREIBER.md`](HERSTELLERTREIBER.md) — und lege es so ab, dass
`/opt/kodak/kds_i2000/lib/devicemanager.so` existiert. Ein anderer Pfad geht
auch, dann mit `--firmware`.

## Bauen und installieren

```bash
make -C native
sudo make -C native install
scanimage -L
```

Gebraucht werden `libusb-1.0`, `libsane` und `libjpeg` samt Entwicklungsdateien.
Die Optionen des Backends zeigt `scanimage --help -d i2600`.

## Knopfdruck statt Rechnerbedienung

`i2600-scand` legt den Scanner schlafen, zeigt einen Text im Display und
wartet auf die grüne Taste. Ein Druck zieht den Stapel ein, legt eine PDF ins
Consume-Verzeichnis und schickt das Gerät zurück in den Energiesparmodus.

```bash
i2600-scand --color --duplex -o ~/paperless-consume
```

Als Dienst: `native/i2600-scand.service` ist eine User-Unit für den
Arbeitsplatz. Auf einem kopflosen Server (Container, VM) braucht es
stattdessen eine System-Unit als root.

## Werkzeuge

| | |
|---|---|
| `i2600-grab` | Rohbilder ohne SANE, für die Fehlersuche |
| `i2600-status` | Statusblock lesen und deuten |
| `i2600-power` | Energiesparmodus lesen und schalten |
| `i2600-calibrate` | Shading-Tabellen Flash ↔ SDRAM |
| `i2600-lcd` | Bitmap ins Bedienfeld-Display |
| `i2600-buttons` | Tastendrücke mitlesen |

## Hardware-Hinweise

Der Scanner muss an einem **Intel-USB-Port** hängen. Am ASMedia-Controller
(ASM1042A) meldet er ungültige Port-IDs und enumeriert nie — das kostet sonst
einen Abend. Ein Papierstau löst sich meist nur über den Netzschalter.

## Dokumentation

| | |
|---|---|
| [`native/NATIVES-BACKEND.md`](native/NATIVES-BACKEND.md) | Der Treiber: Aufbau, Messungen, offene Fragen |
| [`PROTOKOLL.md`](PROTOKOLL.md) | Das USB-Protokoll |
| [`INITIALISIERUNG.md`](INITIALISIERUNG.md) | Kaltstart und Firmware-Download |
| [`HERSTELLERTREIBER.md`](HERSTELLERTREIBER.md) | Das Herstellerpaket paketieren, Fehlersuche |

Die Belegmarker durchgehend: **[M]** gemessen, **[C]** aus Herstellercode,
**[E]** am Gerät erprobt, **[?]** offen. Wo etwas unbelegt ist, steht es da.

## Lizenz und Herkunft

GPL-2.0-or-later, siehe [`LICENSE`](LICENSE).

Der Treiber wurde aus usbmon-Mitschnitten und aus den nicht gestrippten
Bibliotheken des Herstellers erschlossen. **Es ist kein Herstellercode
enthalten**, weder übersetzt noch übernommen; die Firmware-Images werden zur
Laufzeit aus der Kopie gelesen, die der Benutzer selbst besitzt. Kodak und
Alaris sind an diesem Projekt nicht beteiligt.
