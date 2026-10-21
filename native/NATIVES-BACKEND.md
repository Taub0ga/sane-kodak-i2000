# Kodak i2600 — Machbarkeitsnachweis für ein natives Backend

Diese Datei dokumentiert `native/`: ein eigenständiges C-Programm, das ohne
den Kodak-Herstellertreiber ein Rohbild vom i2600 holt. Sie ergänzt
`PROTOKOLL.md` (Kommandoformat), `INITIALISIERUNG.md` (Ablauf) und
`README.md` (Installation) und überschreibt nichts davon.

Belegmarker wie in den anderen Dokumenten: **[M]** am Mitschnitt gemessen,
**[C]** im Code belegt, **[MC]** beides, **[?]** offen. Neu hier:
**[E]** — in dieser Session am Gerät experimentell ermittelt.

---

## 1. Das Ergebnis in einem Satz

**Es funktioniert.** `i2600-grab` scannt ohne jede Kodak-Bibliothek drei
Blätter in Folge, bekommt dieselben Blockgrößen wie der Herstellertreiber,
parst den Bildfooter fehlerfrei und schreibt lesbare PGM-Dateien. **[E]**

Und es nimmt ein **kalt angestecktes** Gerät selbst in Betrieb: der
Firmware-Download, der dafür fehlte, ist nachgebaut und am Gerät geprüft —
Abschnitt 2. Der Herstellertreiber wird für nichts mehr gebraucht. **[CE]**

Von den 362 Vendor-Control-Transfers der Mindestsequenz genügen am warmen
Gerät **112**. Gegen einen echten Kaltstart gemessen kommen `d9regs` und
`daregs` zwingend dazu — Abschnitt 5.

Protokoll des erfolgreichen Laufs (gekürzt):

```
[  0.295] phase open         100 transfers
[  0.839] phase prepare      171 transfers
[  0.839] === reference pass ===
[  1.013] ep 82: block of 51824 bytes in 4 transfers (len mod 508 = 8)
[  1.124] footer: 224 bytes, junk=94 format=1 type=1 width=2580 px
                 xres=300 dpi yres=300 dpi segment=0 last=0 doccount=1
[  1.124] image: 51600 bytes = 20 lines of 2580 pixels
[  1.125] === scan pass ===
[  1.444] EP88  12 Transport State      id=0 value1=1
[  3.646] ep 82: block of 10526784 bytes in 643 transfers (len mod 508 = 8)
[  3.646] footer: 384 bytes, junk=174 ... image: 10526400 bytes = 4080 lines
[  3.748] EP88  41 Page Exit            id=1
[  4.843] ep 82: block of 10480048 bytes in 640 transfers (len mod 508 = 8)
[  4.843] footer:  88 bytes, junk=26  ... image: 10479960 bytes = 4062 lines
[  4.964] EP88  41 Page Exit            id=2
[  6.042] ep 82: block of 10508496 bytes in 642 transfers (len mod 508 = 8)
[  6.042] footer: 156 bytes, junk=60  ... image: 10508340 bytes = 4073 lines
[  6.180] EP88  41 Page Exit            id=3
[  7.844] EP88  12 Transport State      id=0 value1=0
```

Jede Aussage der Vorgängersessions, die dabei geprüft wurde, hat gehalten:

| Aussage aus den Vorgängerdokumenten | Prüfung in dieser Session |
|---|---|
| Kommandos sind Vendor-Control-Transfers, kein Framing | bestätigt — 361 Transfers, kein einziger Stall im Normalbetrieb **[E]** |
| EP 0x82 liefert Bilddaten in 16384-Byte-Blöcken, Kurzpaket = Blockende | bestätigt, 640–643 Transfers je Blatt **[E]** |
| Blocklänge ≡ 8 (mod 508) | bestätigt für alle vier Blöcke **[E]** |
| Footer rückwärts gelesen, 4-Byte-TLV, Wert big endian | bestätigt — C-Reimplementierung parst alle Blöcke fehlerfrei **[E]** |
| 2580 Pixel, 8 bit, 300 dpi, keine Parameter dafür | bestätigt — Footer meldet 2580 px und 300/300 dpi, Bilddaten restlos durch 2580 teilbar **[E]** |
| Tag `0xff` JunkWords ist die Füllung auf 508-Byte-Granularität | bestätigt — 174, 26 und 60 Füllwörter je nach Zeilenzahl **[E]** |
| Interrupt-Ereignistabelle auf EP 0x88 | bestätigt — Transport State, Page Exit, Imaging Complete, Paper Jam alle wie beschrieben **[E]** |

Die erzeugten Bilder liegen in `captures/scans/native-page1.pgm`,
`native-reference.pgm` und als Vorschau `native-scans.png`.

### Vergleich mit den Referenzscans

Gleiche Vorlage, gleicher Inhalt, gleiche Orientierung wie
`captures/scans/f-simplex-gray.pnm`. Die Unterschiede sind genau die
erwarteten:

| | Herstellertreiber (`f-simplex-gray.pnm`) | `i2600-grab` |
|---|---|---|
| Format | P5, 1657 × 2350 | P5, 2580 × 4080 |
| Auflösung | 200 dpi (vom Host skaliert) | 300 dpi, wie vom Gerät geliefert |
| Beschnitt | auf das Blatt zugeschnitten | ungeschnitten, mit Transportrand |
| Entzerrung, Kantenglättung | Perfect Page (`hippo.so`) | keine |

Das ist der dokumentierte Entwurf: Das Backend liefert Rohbilder,
Skalierung, Bittiefe und Bitonal-Wandlung macht der Host.

---

## 2. Die Voraussetzung, die in Abschnitt 12 fehlt: der Firmware-Download

`INITIALISIERUNG.md` Abschnitt 12 klammert den Firmware-Download
ausdrücklich aus. Zu Recht — aber nicht, weil er entbehrlich wäre, sondern
weil alle sechs Mitschnitte von einem Gerät stammen, das ihn schon hinter
sich hatte. Nach jedem Kaltstart (Netzstecker, USB-Abzug) ist er nötig.

Dieser Abschnitt hat zwei Schichten. Die ersten beiden Unterabschnitte sind
der Befund der Vorgängersession: *dass* etwas fehlt, und woran man es sieht.
Alles ab „Wo die Firmware steckt" ist neu: *was* fehlt, und dass es
nachgebaut und am Gerät bewiesen ist.

### Die Beobachtung (Vorgängersession)

Der erste Versuch scheiterte sofort: `C0 00 GetStatus` antwortete, **jeder
andere Vendor-Request stallte** (`LIBUSB_ERROR_PIPE`). Der Control-Endpunkt
war dabei nicht blockiert — `GetStatus` ging zwischen den Fehlversuchen
jedes Mal weiter durch. **[E]**

```
status         IN  req=00 len=32 -> 32    01 00 04 01 0d 00 00 00 ...
fwversions     IN  req=02 len=56 -> LIBUSB_ERROR_PIPE
status-after   IN  req=00 len=32 -> 32    01 00 04 01 0d 00 00 00 ...
eol            IN  req=34 len=55 -> LIBUSB_ERROR_PIPE
lowpower       IN  req=32 len=2  -> LIBUSB_ERROR_PIPE
```

Nach **einem** Öffnen durch den Herstellertreiber (`scanimage -A -d
kds_i2000:i2000`, ohne Scan) antwortete dasselbe Gerät auf alles — das war
damals der Behelf, heute macht `i2600-grab` das selbst:

```
status         IN  req=00 len=32 -> 32    03 00 01 02 0f 00 09 00 ...
fwversions     IN  req=02 len=56 -> 56    00 04 01 0d 00 04 01 0e 00 01 02 0f ...
eol            IN  req=34 len=55 -> 55    04 0a 60 1d 00 03 00 4b 4f 44 41 4b ...
```

### Die Deutung — im Code belegt (Vorgängersession)

`DeviceManager::InitializeScanner::IsFirmwareComponentRunning(u8, bool)`
liest `GetStatus()` und protokolliert **[C]**:

```
"Component: %d  Version: %08X"
"Component: %d, Mismatched Version"
```

Damit sind die ersten fünf Bytes des Statusblocks benannt — eine Lücke, die
`INITIALISIERUNG.md` Abschnitt 13 noch als offen führt:

| Offset | Feld | kalt | nach Treiberstart |
|---|---|---|---|
| `0x00` | **Component** | `01` = Booter | `03` = Scanner |
| `0x01`–`0x04` | **Version** (u32 BE) | `00 04 01 0d` | `00 01 02 0f` |

Und `GetFirmwareVersions` (`C0 02`) listet genau diese drei Versionen
hintereinander: `00 04 01 0d | 00 04 01 0e | 00 01 02 0f`. **[E]**
Komponente 1 = Booter, 2 = Loader, 3 = Scanner — die Argumentwerte, mit
denen `LoadFirmware` die Funktion aufruft. **[C]**

`InitializeScanner::LoadFirmware(bool)` verzweigt genau danach **[C]**:

```
L777  "Booter Firmware running at device connect"
          LoadLoader() ; SetupFpga() ; LoadBankedFirmware()
L747  "Loader Firmware running at device connect"
          LoadLoader() ; SetupFpga() ; LoadBankedFirmware()
L740  "Scanner Firmware running at device connect...check if booter update is needed"
          IsBooterUpdateNeeded()        <- kein Download
L800  "Confirmed Scanner Firmware is running"
L804  "Failed to start the Scanner Firmware"
```

`LoadLoader()` und `LoadBankedFirmware()` holen ihre Daten über
`InitializeScanner::InitializeBuffer(file, resource, …)` und schieben sie mit
`LoadInternalFirmware(...)` bzw. viermal `LoadFirmwareBank(bank 0..3, …)`
ins Gerät — das ist der Pfad über Bulk-OUT EP 0x02, der in allen
Mitschnitten bei **null** Transfers liegt. **[MC]**

Die Namen der Quellen stehen als Datensymbole im Binary **[C]**:

```
DM_BOOTER_FIRMWARE_FILE   -> booter.bin      DM_BOOTER_FIRMWARE_RESOURCE   -> BOOTER
DM_LOADER_FIRMWARE_FILE   -> loader.bin      DM_LOADER_FIRMWARE_RESOURCE   -> LOADER
DM_FPGA_FIRMWARE_FILE     -> fpga.bin        DM_FPGA_FIRMWARE_RESOURCE     -> FPGA
DM_SCANNER0..3_FIRMWARE_FILE -> scanner0..3.bin   DM_SCANNER0..3_FIRMWARE_RESOURCE
DM_UDDS/DSP/OCP/ELEVATOR/SCANMOD/BOOTIMAGE_FIRMWARE_{FILE,RESOURCE}
```

Im Installationsverzeichnis liegt **keine** dieser `.bin`-Dateien — die
Firmware steckt also als benannte Ressource in den gelieferten Bibliotheken
und wird bei jedem Kaltstart ins Gerät geladen. **[E]**

### Wo die Firmware steckt — gelöst **[CE]**

`InitializeScanner::InitializeBuffer(resource, file, path, …)` probiert zuerst
`path/file` auf der Platte und fällt sonst auf `InitializeBufferWithResource`
zurück. Dort steht **[C]**:

```c
COsResource::Open(local_38, "OsResourceGetLocal", a_szResourceName, 1);
```

`OsResourceGetLocal` ist kein Symbol — es ist ein Pseudomodulname mit der
Bedeutung „die Ressourcen des Moduls, in dem ich gelinkt bin". Und die liegen
**hinter der letzten ELF-Section**: `devicemanager.so` trägt 1.077.833 Byte
hinter dem Ende von `.strtab`, `driver.so` 1.038.817, `hippo.so` 1.245.256.
**[E]**

Am Dateiende steht ein Verzeichnis aus 56-Byte-Einträgen, abgeschlossen von
einem Eintrag mit der Signatur `@!@kds@!@` als Namen **[E]**:

```c
struct kds_resource_entry {     /* 56 Byte */
    char     name[32];
    uint64_t offset;            /* Dateioffset der Nutzdaten */
    uint64_t size;
    uint64_t reserved;          /* immer 0 */
};
```

Die Einträge stehen in **umgekehrter** Nutzdatenreihenfolge, man läuft also von
der Signatur rückwärts, bis ein Name nicht mehr druckbar ist. Die Nutzdaten
selbst liegen lückenlos hintereinander — jedes `offset + size` ist das
`offset` des nächsten Blobs, was die Deutung unabhängig bestätigt. **[E]**

`tools/kds-resources.py` listet und extrahiert sie; `native/i2600-firmware.c`
enthält denselben Parser in C (beide liefern byteidentische Images).

21 Ressourcen stecken in `devicemanager.so`, sieben davon braucht der
Download **[E]**:

| Ressource | Größe | Inhalt | Ziel |
|---|---|---|---|
| `LOADER` | 16.384 | 8051-Code, beginnt mit `02 07 bf` (`LJMP 0x07bf`) | FX2-internes RAM `0x0000` |
| `FPGA` | 718.569 | Altera-Bitstrom (Vorspann aus `0xff`) | Bulk-OUT EP 0x02 |
| `SCANNER` | 16.384 | 8051-Code, `02 26 23` | FX2-internes RAM `0x0000` |
| `SCANNER0` | 32.768 | **durchgehend null** | Codebank 0 |
| `SCANNER1` | 32.768 | 8051-Code | Codebank 1 |
| `SCANNER2` | 32.768 | 8051-Code | Codebank 2 |
| `SCANNER3` | 32.768 | 8051-Code | Codebank 3 |

**Alle Images sind rohe Binärabbilder: kein Kopf, kein Längenfeld, keine
Prüfsumme.** Länge steht im Ressourcenverzeichnis, Zieladresse im Code. Der
Rest sind Diagnosebilder (`COUNTONLYRGB`, `COUNTONLYFOOTER`,
`TP_GOLDEN_5336X1`), XML-Label-Tabellen (`LABELSEVENT`, `LABELSMETER`,
`LABELSOCP`, `DMDATABASE`), `BOOTER`/`BOOTIMAGE` für das Booter-Update und
vier Versionsstempel.

Die Versionsstempel sind `u32` **little endian** — also byteverdreht zu dem,
was der Statusblock big endian meldet **[E]**:

| Ressource | Bytes | ergibt | Gerät meldet |
|---|---|---|---|
| `FWVERSION_LOADER` | `0e 01 04 00` | `0004010E` | `00 04 01 0e` ✓ |
| `FWVERSION_SCANNER` | `0f 02 01 00` | `0001020F` | `00 01 02 0f` ✓ |
| `FWVERSION_BOOTER` | `0d 02 01 00` | `0001020D` | `00 04 01 0d` ✗ |
| `FWPKGVERSION` | `"01800000"` | — | — |

Loader und Scanner stimmen damit exakt; der mitgelieferte Booter ist ein
anderer als der im Gerät — genau die Frage, die `IsBooterUpdateNeeded()`
klärt. Ein Backend hat dort nichts zu tun.

### Der Download — das Gerät ist ein Cypress FX2 **[CE]**

Das ist der Schlüssel, der alles erklärt. `LoadInternalFirmware` schreibt mit
**`bRequest 0xa0`**, und `0xa0` ist der FX2-eigene `RW_INTERNAL`-Request, den
der USB-Core in Hardware beantwortet. `0xe600` ist das FX2-Register `CPUCS`,
dessen Bit 0 den 8051 im Reset hält (die Dumpstrings heißen „Hold Fx2 Reset"
und „Release Fx2 Reset"). **Darum lässt sich ein Gerät programmieren, dessen
Firmware gar nicht läuft** — und darum beantwortet der Booter zwar nichts
außer `GetStatus`, nimmt aber den ganzen Download an.

`DeviceSettings::ScannerOperation(_ScannerOp)` ist eine Sprungtabelle von 27
Operationen auf `(Opcode, wValue, wIndex)`; `Set(opcode, wValue, wIndex, 0,
NULL)` schickt sie ab. Drei Einträge gehören zum Download **[C]**:

| `_ScannerOp` | Transfer | Bedeutung |
|---|---|---|
| `0x11` | `40 20 wValue=1` | FPGA-Download start |
| `0x12` | `40 20 wValue=0` | FPGA-Download stop |
| `0x13` | `40 21 wValue=0` | vor dem Halten des FX2-Reset |

Gegenprobe: dieselbe Tabelle bildet `_ScannerOp 0x0d` auf `40 1b` ab — das ist
die Gruppe `op1b` aus Abschnitt 5, deren Bedeutung dort noch **[?]** war.
Damit sind die Opcodes `0x20` und `0x21` benannt.

Die drei Primitiven, byteexakt **[C]**:

```
LoadInternalFirmware(data, len, addr, bAlreadyInReset, bLeaveInReset):
    40 21 wValue=0                                   ScannerOperation 0x13
    40 a0 wValue=0xe600 wIndex=0 len=1 data=01        Hold Fx2 Reset
    je 0x800 Byte:
        40 a0 wValue=addr+off wIndex=0 len=n data=chunk
    40 a0 wValue=addr+len wIndex=0 len=0             Schleifenabbruch
    40 a0 wValue=0xe600 wIndex=0 len=1 data=00        Release Fx2 Reset

LoadFirmwareBank(bank, data, len):
    40 a3 wValue=0xd80e wIndex=0 len=1 data=bank      Bank wählen
    je 0x800 Byte:
        40 a3 wValue=0x4000+off wIndex=off len=n data=chunk
    40 a3 wValue=0xc000 wIndex=0x8000 len=0          Schleifenabbruch

ProgramFpga():
    PuffBuffer(fpga, len, 1000, 300, &puffed, &puffedlen)
    40 20 wValue=1                                   Download start
    Bulk-OUT EP 0x02: der ganze expandierte Strom
    40 20 wValue=0                                   Download stop
    „Resetting all pipes"
```

`PuffBuffer()` ist keine Dekompression, sondern eine **Bitspreizung**: jedes
Bit des Bitstroms wird ein eigenes Byte 0 oder 1, LSB first, dahinter 300
Nullbytes als Nachlauftakte. Aus 718.569 Byte werden 5.748.852. **[C]**

```c
out[i * 8 + b] = (in[i] >> b) & 1;          /* b = 0..7 */
```

Der Treiber stückelt den Bulk-OUT in einer Größe, die er sich von seiner
USB-Schicht geben lässt (`EOSUSBIOCTL 0x1d`, Dumpstring
`SendBulkOut(chunk size=0x%x bytes)`). Das ist eine Hosteigenschaft, keine
Protokollvorgabe; `i2600-firmware.c` nimmt 32.768 Byte.

Die Quittierung zwischen den Blöcken: **keine.** Weder `LoadInternalFirmware`
noch `LoadFirmwareBank` lesen etwas zurück, und `IsFirmwareComponentRunning`
pollt nicht — ein einzelnes `GetStatus`. **[C]**

Reihenfolge in `LoadFirmware(bool)` für Komponente 1 oder 2 **[C]**:

```
LoadLoader()                 -> Komponente muss 2 werden
SetupFpga()                  = ProgramFpga() + TestFpgaMemory()
LoadBankedFirmware()         = LoadFirmwareBank(0..3)
                               + LoadInternalFirmware(SCANNER, addr 0)
                             -> Komponente muss 3 werden
```

`TestFpgaMemory()` (Diagnose) und `IsBooterUpdateNeeded()` (Booter-Flash) sind
übersprungen: das eine ist eine Prüfung, das andere darf ein Backend nicht
anfassen. Ein optionales siebtes Image `SCANNERX` würde nach `0xc000` geladen;
die Ressource existiert in dieser Treiberversion nicht.

### Am Gerät geprüft **[E]**

Kaltes Gerät (Strom aus, USB ab, wieder an), Herstellertreiber nicht
angefasst, Papier im Einzug, `./i2600-grab` ohne jede Vorbereitung und ohne
Option. Ein einziger Lauf, gekürzt um die 70 Blockzeilen:

```
[  0.047] status: component = 1 (1=Booter 2=Loader 3=Scanner)
[  0.050] firmware images from /opt/kodak/kds_i2000/lib/devicemanager.so:
          loader 16384, fpga 718569, scanner 16384, banks 32768/32768/32768/32768
[  0.150] firmware: component 1 (booter) version 0004010D
[  0.150] firmware: LoadLoader (16384 bytes)
[  0.201] firmware: hold FX2 reset
[  0.203] firmware: release FX2 reset
[  0.458] firmware: loader running, version 0004010E
[  0.459] firmware: ProgramFpga
[  0.471] firmware: FPGA bitstream 718569 bytes -> 5748852 expanded bytes
[  1.231] firmware: LoadFirmwareBank 0 (32768 bytes)
[  2.618] firmware: LoadFirmwareBank 1 (32768 bytes)
[  4.160] firmware: LoadFirmwareBank 2 (32768 bytes)
[  5.702] firmware: LoadFirmwareBank 3 (32768 bytes)
[  7.244] firmware: LoadInternalFirmware SCANNER (16384 bytes)
[  7.449] firmware: hold FX2 reset
[  7.450] firmware: release FX2 reset
[  7.880] firmware: scanner firmware running, version 0001020F
[  7.882] firmware: versions booter=0004010D loader=0004010E scanner=0001020F
[  8.196] phase open         100 transfers, 0 skipped
[  8.744] phase prepare      171 transfers, 0 skipped
[  8.744] === reference pass ===
[  8.923] ep 82: block of 51824 bytes in 4 transfers (len mod 508 = 8)
[  9.055] footer: 224 bytes, junk=94 format=1 type=1 width=2580 px
                 xres=300 dpi yres=300 dpi segment=0 last=0 doccount=1
[  9.055] === scan pass ===
[ 11.590] ep 82: block of 10645148 bytes in 650 transfers (len mod 508 = 8)
[ 11.590] footer: 68 bytes, junk=16 ... image: 10645080 bytes = 4126 lines
[ 11.734] EP88  41 Page Exit            id=1
[ 12.771] ep 82: block of 10498336 bytes in 641 transfers (len mod 508 = 8)
[ 12.771] footer: 316 bytes, junk=140 ... image: 10498020 bytes = 4069 lines
[ 12.886] EP88  41 Page Exit            id=2
[ 13.968] ep 82: block of 10557772 bytes in 645 transfers (len mod 508 = 8)
[ 13.968] footer: 412 bytes, junk=188 ... image: 10557360 bytes = 4092 lines
[ 14.102] EP88  41 Page Exit            id=3
[ 21.976] scan pass: 3 page(s)
[ 22.152] phase teardown     82 transfers, 0 skipped
```

**Das ist der Nachweis**: Komponente 1 beim Anstecken, Komponente 3 nach acht
Sekunden, danach drei Blätter in Folge mit gültigen Footern und lesbarem
Inhalt (Mittelwert 198,7 / 168,6 / 204,9 bei voller Spanne 0–255; Vorschau in
`captures/scans/native-coldstart-page1.png`). Kein einziger fehlgeschlagener
Transfer im ganzen Lauf.

Zeitaufteilung des Downloads: 0,3 s für den Loader, **0,76 s für den
FPGA-Bitstrom** (5,75 MB über Bulk-OUT), **je 1,5 s pro Codebank** — die 16
Control-Transfers einer Bank brauchen jeweils etwa 80 ms, das Gerät schreibt
sie also in echten Speicher und nicht in einen Puffer —, 0,6 s für das
Scanner-Image.

Drei Dinge waren offen und sind jetzt beantwortet **[E]**:

* **Der Booter nimmt `40 21` an.** Die Vermutung aus der Vorgängersession, er
  beantworte wirklich ausschließlich `GetStatus`, stimmt so nicht:
  `ScannerOperation 0x13` geht durch. (`i2600-firmware.c` behandelt einen
  Stall dort trotzdem als unkritisch, weil `0xa0` ohnehin von der
  FX2-Hardware bedient wird.)
* **Keine Neuanmeldung am Bus.** Der FX2 renumeriert nicht: dasselbe
  `libusb_device_handle` und dasselbe geclaimte Interface tragen über beide
  Reset-Zyklen hinweg. Es ist auch kein Warten nötig — der Loader antwortet
  rund 250 ms nach dem Freigeben des Reset, die Anwendungsfirmware nach 430 ms,
  und dazwischen liegt nur die normale Laufzeit der Control-Transfers.
* **Der Weg über Komponente 2 funktioniert auch.** Ein abgebrochener Download
  hinterlässt das Gerät im Loader; der nächste Lauf erkennt Komponente 2 und
  fährt dieselbe Kette ab `LoadLoader()` neu — genau wie der
  Herstellertreiber („Loader Firmware running at device connect"). Gemessen,
  nachdem ein erster Versuch mitten im Laden von Bank 0 abgebrochen wurde.

### Was das für ein natives Backend bedeutet

* **Der Kaltstart ist nachgebaut.** `i2600-grab` nimmt ein kalt angestecktes
  Gerät selbst in Betrieb; der Herstellertreiber wird nicht mehr gebraucht.
* **Die Firmware selbst kann ein Backend nicht mitliefern** — sie ist
  Kodak-Eigentum. Es kann sie aber aus einer vorhandenen Treiberinstallation
  lesen, und genau das tut `i2600_fw_load()`: Standardquelle ist
  `/opt/kodak/kds_i2000/lib/devicemanager.so`. Alternativ ein Verzeichnis mit
  `loader.bin`, `fpga.bin`, `scanner.bin` und `scanner0..3.bin` — das ist der
  Dateipfad, den der Hersteller in `InitializeBufferWithFile` selbst vorsieht.
  Dieselbe Lage wie bei den SANE-Backends, die Firmware aus einer vom Anwender
  bereitgestellten Datei nachladen.
* **Der Statusblock zeigt den Zustand sofort**, und `i2600-grab` liest ihn
  jetzt beim Start selbst:

```
[  0.010] status: component = 1 (1=Booter 2=Loader 3=Scanner)   <- lädt
[  0.010] status: component = 3                                  <- bereit
```

---

## 3. Gerätezugriff: was nötig war, und was nicht

**Nichts.** Kein `udev`-Regelwerk, kein Entladen eines Treibers, kein
`root`. **[E]**

* **Kein Kernelmodul beansprucht das Gerät.** `lsusb -t` zeigt für
  Interface 0 `Driver=[none]`; der Herstellertreiber ist ein
  Userspace-Client über openusb/libusb. `libusb_claim_interface(0)` gelingt
  auf Anhieb. Das Programm ruft trotzdem `libusb_kernel_driver_active()` auf
  und würde einen künftigen Kerneltreiber abhängen.
* **Die Rechte stehen schon.** sane-backends 1.4.0 führt den i2600 in
  `/usr/lib/udev/hwdb.d/20-sane.hwdb`, und `70-uaccess.rules` setzt daraufhin
  eine ACL auf den Geräteknoten:

  ```
  # file: dev/bus/usb/001/005
  user:tauboga:rw-
  group:saned:rw-
  ```

  Das ist derselbe Mechanismus, von dem auch der Herstellertreiber lebt
  (`README.md` Abschnitt zu den Rechten). Ein natives SANE-Backend erbt ihn
  unverändert.
* **Der Herstellertreiber ist kein Konkurrent**, solange er nicht gerade
  läuft: er hält das Interface nur während einer Sitzung. Läuft er doch,
  scheitert `libusb_claim_interface` mit `LIBUSB_ERROR_BUSY`, und das
  Programm sagt das auch so.
* **Der Scanner muss an Bus 1 (Intel-Port) hängen**, nicht an der
  ASMedia-Buchse — dort enumeriert er nicht. Das ist ein Hostproblem, kein
  Treiberproblem.

---

## 4. Aufbau des Programms

```
native/
  gen-sequence.py    erzeugt sequence.h aus einem pcapng-Mitschnitt
  sequence.h         362 Vendor-Control-Transfers als benannte Tabelle
  i2600-grab.c       Ablaufsteuerung, Bulk-Lesen, Footer-Parser, PGM
  i2600-firmware.c   Ressourcenparser + Firmware-Download (Abschnitt 2)
  i2600-firmware.h
  Makefile
```

### Warum die Sequenz generiert und nicht abgetippt ist

Die Initialisierung programmiert sechs Kameraseiten à 32 Byte je Kamera,
zwei Dutzend FPGA-Register und zwei 120-Byte-`SDmScanConfig`-Blöcke. Die
*Bedeutung* dieser Werte steht in `PROTOKOLL.md` Abschnitt 4a und
`INITIALISIERUNG.md` Abschnitt 8; ihre *Bytes* von Hand abzuschreiben wäre
reines Übertragungsrisiko ohne Erkenntnisgewinn. `gen-sequence.py` liest sie
deshalb byteexakt aus `captures/pcapng/b-simplex-lineart-200.pcapng` und
schreibt sie als C-Tabelle:

```c
static const struct i2600_step i2600_seq_prepare[] = {
    { GRP_D9REGS,   RT_HOST_TO_DEVICE, OP_FPGA_REGISTER,    0xd900, 0x0000, 1,  412 },
    ...
    { GRP_SCAN_CFG, RT_HOST_TO_DEVICE, OP_SCANNER_CONFIGURATION, 0x0000, 0x0000, 120, 931 },
};
```

Die Opcodes sind benannte Konstanten mit den Originalfunktionsnamen des
Herstellers (`OP_SCANNER_OPERATION`, `OP_START_CAPTURE`,
`OP_SCANNER_CONFIGURATION`, `OP_FPGA_REGISTER`, `OP_CAMERA_REGISTER`, …) —
keine Magic Numbers. Der Ablauf selbst (Reihenfolge der Phasen, Warten auf
Ereignisse, Bulk-Lesen, Abbau) steht als Code in `i2600-grab.c`.

Jeder Schritt trägt zusätzlich eine **Gruppe**. `--skip` schaltet Gruppen ab;
das ist das Werkzeug, mit dem Abschnitt 5 entstanden ist.

### Ablauf

```
libusb_open(040a:601d) ; claim_interface(0)
GetStatus -> Component                (Abschnitt 2; bei 1 oder 2 Firmware laden)
  LoadLoader ; ProgramFpga ; LoadFirmwareBank 0..3 ; LoadInternalFirmware
Interrupt-Thread auf EP 0x88          (8-Byte-Ereignisse, nur Protokoll + Warten)
phase open      (100 Transfers)       Identität, AFE-Standby, Interrupts an
phase prepare   (171 Transfers)       d9xx/daxx-Register, Kameraseiten, Gains, Weißabgleich
--- Referenzdurchgang ---
phase ref_config (0x30, CaptureMode=2) ; 0x10 wValue=1 ; auf 0x02 warten ; 0x17 wValue=1
EP 0x82 lesen bis Kurzpaket -> Footer -> PGM ; 0x10 wValue=0
--- Scandurchgang ---
phase scan_prep  (FixedPageLength = 4650) ; phase scan_config (0x30, CaptureMode=1)
0x10 wValue=1 ; je Blatt EP 0x82 lesen bis Kurzpaket -> Footer -> PGM
--- Abbau ---
phase teardown  (82 Transfers)        Stop, Halt, Lampe aus, AFE-Standby, Interrupts aus
```

Der Footer-Parser ist eine direkte Reimplementierung von
`Image::ParseImageFooter()`: rückwärts vom Blockende, 4-Byte-Datensätze
`<WertHi> <WertLo> <01> <Tag>`, Wert big endian, `0x01` beendet. Die
Auflösungscodes kommen aus der Sprungtabelle von `ResolutionToDPI`
(0=600, 1=400, 2=300, 3=200, 4=150).

### Benutzung

```bash
cd native && make
./i2600-grab                  # Referenzdurchgang + Scan bis der Einzug leer ist
./i2600-grab -S               # nur Referenzdurchgang, braucht kein Papier
./i2600-grab -o /tmp/foo      # Dateipräfix
./i2600-grab -s ident,flash   # Gruppen überspringen
./i2600-grab -l               # Gruppen auflisten
./i2600-grab -v               # Firmware-Download Block für Block protokollieren
./i2600-grab -vv              # dazu jeden Control-Transfer
```

Zur Firmware (Abschnitt 2) — beim Start wird die Component aus dem Statusblock
gelesen und der Download nur gefahren, wenn sie nicht 3 ist:

```bash
./i2600-grab -F PFAD          # devicemanager.so oder Verzeichnis mit *.bin
                              # Standard: /opt/kodak/kds_i2000/lib/devicemanager.so
./i2600-grab -N               # nie laden, bei kaltem Gerät abbrechen
./i2600-grab -f               # laden, auch wenn Komponente 3 schon läuft
python3 ../tools/kds-resources.py /opt/kodak/kds_i2000/lib/devicemanager.so [ZIEL]
                              # Ressourcen eines Kodak-Moduls listen/extrahieren
```

---

## 5. Was von der Mindestsequenz wirklich nötig war

Methode: Gruppen abschalten und einen echten Scan mit Papier fahren. **Als
Kriterium zählt nicht, ob ein Block ankommt, sondern ob das Bild stimmt** —
siehe den Fehlschlag mit der Lampe weiter unten. Geprüft wurden deshalb
jedes Mal Zeilenzahl, Footer und Bildinhalt. **[E]**

**Einschränkung und wie sie aufgelöst wurde:** Das Gerät behält seinen
Registerzustand zwischen zwei Läufen. Ein „geht auch ohne" heißt deshalb
nicht in jedem Fall „wird nie gebraucht", sondern kann heißen „stand vom
vorigen Lauf noch richtig". Die erste Messreihe lief ausschließlich am
warmen Gerät und konnte das nicht trennen; am schärfsten stand der
Vorbehalt bei `d9regs`, `daregs` und `gains`, die genau die Register
setzen, die ein vorangegangener vollständiger Lauf schon richtig
hinterlässt.

Seit der Firmware-Download nachgebaut ist (Abschnitt 2), lässt sich das
messen. **Diese drei Gruppen sind gegen echte Kaltstarts geprüft** — Gerät
stromlos, neu angesteckt, `status[0] = 01` (Booter) vor jedem Messlauf, so
dass kein einziges dieser Register seit dem Einschalten je geschrieben
wurde. Je Kaltstart ein Messlauf `./i2600-grab -R -p 1 -s <gruppen>` und
unmittelbar danach ein vollständiger Lauf derselben Sitzung als Referenz,
damit Papier, Beleuchtung und Gerätezustand belegt sind. Ergebnis: **zwei
der drei Gruppen hatten nur vom Vorlauf profitiert**, `d9regs` und `daregs`
stehen jetzt unter „Unverzichtbar". **[E]**

Die fünf Kaltstarts im Einzelnen. „Messlauf" ist immer der erste Lauf nach
dem Anstecken (`status[0] = 01`), „Referenz" der vollständige Lauf
unmittelbar danach **[E]**:

| Kaltstart | übersprungen | Zeilen | Footer | Bild | Referenz derselben Sitzung |
|---|---|---|---|---|---|
| 1 | `d9regs,daregs,gains` (37 Transfers) | 465 | `format=0 width=0 px`, **600/600 dpi** | graues Bandmuster, Mittelwert 150.7, Zeilenmittel-Spanne nur 17.6 | 4031 Zeilen, `width=2580`, voll lesbar |
| 2 | `d9regs` (11) | — | **parst nicht**, Block 4315648 Byte, Länge ≢ 8 (mod 508) | kein Bild; danach Papierstau | 4020 Zeilen, voll lesbar (nach Stauentfernung) |
| 3 | `daregs` (17) | 465 | `format=0 width=0 px`, 300/300 dpi | dunkle Streifen, Mittelwert 59.0, max 181 | 4029 Zeilen, voll lesbar |
| 4 | `gains` (9) | 4011 | korrekt, `width=2580`, 300/300 dpi | **voll lesbar**, Mittelwert 187.6, 256 Grauwerte | 4019 Zeilen, voll lesbar |
| 5 | `gains` (9), **dasselbe Blatt zweimal** | 4056 | korrekt | **voll lesbar**, Mittelwert 200.6, Anteil 255 = 74.5 % | 4064 Zeilen, Mittelwert 205.9, Anteil 255 = 78.9 % |

Kaltstart 5 schließt die Lücke, an der die alte Messung hängen blieb: weil
dasselbe Blatt ein zweites Mal eingelegt wurde, ist die Helligkeit direkt
vergleichbar. Der Unterschied beträgt 5.3 Graustufen von 255 — `gains` hebt
das Bild messbar, aber unwesentlich an.

Für die übrigen Einträge der Entbehrlich-Tabelle gilt der Vorbehalt
unverändert weiter — sie wurden nur am warmen Gerät abgeschaltet. Für ein
Backend, das ein Gerät in unbekanntem Zustand vorfindet, bleibt die sichere
Lesart: **alle Register schreiben, die Werte setzen**; die Messung sagt,
welche davon die Hardware bei *jedem* Durchgang neu braucht.

### Das Ergebnis

**112 der 362 Vendor-Control-Transfers genügen für ein vollständiges,
lesbares Blatt — am warmen Gerät.** Gemessener Lauf **[E]**:

```
phase open         0 Transfers, 100 übersprungen
phase prepare    108 Transfers,  63 übersprungen      (campages 91 + wbal 16 + lamp 1)
phase scan_prep    3 Transfers
phase scan_config  1 Transfer
ep 82: block of 10477508 bytes in 640 transfers
footer: 128 bytes, junk=46, width=2580 px, 300/300 dpi
image: 10477380 bytes = 4061 lines of 2580 pixels     Mittelwert 129.8, voll lesbar
phase teardown    82 Transfers
```

Dieser Lauf schaltet unter anderem `d9regs` und `daregs` ab und profitierte
damit vom Vorzustand: **von einem Kaltstart aus trägt er nicht**. Die 28
Transfers dieser beiden Gruppen kommen dann dazu. Ob 140 Transfers als
geschlossene Mindestmenge vom Kaltstart aus genügen, ist **nicht** gemessen
— geprüft wurden die drei Gruppen einzeln und gemeinsam, nicht der volle
Restsatz. **[E]**

### Unverzichtbar

| Gruppe | Inhalt | Transfers | Befund |
|---|---|---|---|
| `campages` | `40 e0`: sechs Kameraseiten à 32 Byte je Kamera, Seitenwahl über Register `0x1f` | 91 | Ohne sie **kein Bildblock**, auch nicht im Referenzdurchgang **[E]** |
| `wbal` | `40 a3 d901 = 0x28` (CommonGeneralControl), dann `dd0a`/`dd0b` und die `da11`–`da1a`-Lesungen | 16 | Ohne sie **kein Bildblock**. Das `d901 = 0x28` ist das Camera-Enable-Bit **[E]** |
| `lamp` | `40 11 wValue=1` | 1 | Ohne sie kommt ein **formal einwandfreier Block** — 4057 Zeilen, gültiger Footer — aber das Bild ist **komplett schwarz** (Mittelwert 0.0, max 5) **[E]** |
| `d9regs` | `a3 d900/d901/d903–d907/d920` und der 78-Byte-`dc00`-Block | 11 | **Kaltstart:** Block von 4315648 Byte, Länge ≢ 8 (mod 508), **Footer parst nicht**, Blockende fast reines `0xff`, kein Bild. Dazu `Transport State value1=3` und danach **Papierstau** — das Blatt musste von Hand heraus. Hier steckt auch der Auflösungswert: ohne `d9regs` meldet der Footer 600 dpi statt 300 **[E]** |
| `daregs` | `a3 daxx/dbxx`: ImageWidth, PageDetect, IP_Control, HorizontalCropLeft | 17 | **Kaltstart:** 465 statt ~4030 Zeilen, Footer meldet **`width = 0 px`** und `format = 0`, Bilddaten nicht durch 2580 teilbar, Inhalt nur dunkle Streifen (Mittelwert 59.0, max 181) **[E]** |
| `scan_prep` | `40 a3 da0e=0x12, da06=0x12, da07=0x2a` | 3 | Ohne sie liefert der Scandurchgang **20 Zeilen** statt 4061: `FixedPageLength` steht noch auf dem Referenzwert **[E]** |
| `scan_cfg` | der 120-Byte-`SDmScanConfig`-Block (`0x30`), CaptureMode=1, FeedStyle=1, SpeedId=4 | 1 | Der Durchgang existiert nicht ohne ihn |
| `ref_cfg` | derselbe Block mit CaptureMode=2 (shutter) | 1 | nur für den Referenzdurchgang |

### Entbehrlich — belegt durch Scan mit Papier und geprüftem Bild

Jede dieser Gruppen wurde abgeschaltet und der Scan lieferte weiterhin ein
korrektes, lesbares Blatt **[E]**:

| Gruppe | Inhalt | Transfers | gemessen |
|---|---|---|---|
| `ident` | GetStatus ×3, GetFirmwareVersions, GetEndOfLine, GetBoardSerialNumber, GetSessionInfo, SetTime, GetMachineConstants, GetNvRamData | 10 | reine Lesevorgänge |
| `flash` | `C0 e3` ReadFlashMemory wIndex=0xfc00 | 1 | reiner Lesevorgang |
| `session_rd`, `session_w`, `docid2` | GetSessionInfo, SetSessionInfo, DocumentId zurücklesen | 8 | Buchhaltung |
| `housekeep` | GetBatchData, Lampe aus, LowPowerTimeout, die beiden 768-Byte-`0x62`-Blöcke, SetNvRamData, SetButtonSequenceNumber, GetMeters | 24 | Buchhaltung |
| `poll1`, `poll2` | GetStatus-Polling, LowPowerTimeout | 15 | reine Lesevorgänge |
| `estar`, `batch`, `lamp2` | SetEnergyStar, SetBatchData, Lampe nochmals an | 5 | |
| `docid` | `40 a3 da27/db27 = DocumentId` | 2 | Bild korrekt ohne |
| `standby` | `SetAfeStandby` beider Kameras + `a3 d901` | 59 | Bild korrekt ohne — die Kameraseiten (`campages`) überschreiben das ohnehin |
| `op1b` | `40 1b` (ScannerOperation 0x0d, Bedeutung weiter **[?]**) | 1 | Bild korrekt ohne |
| `intrevents` | `40 3a wValue=1 wIndex=1` und damit der ganze EP-0x88-Kanal | 1 | **Bild korrekt ohne jedes Ereignis.** Der Interrupt-Kanal ist Komfort — Blattzählung, Jam-Meldung, Transportzustand —, keine Voraussetzung für den Bilddatenfluss **[E]** |
| `gains` | `40 e0` Seite 1 Register 0, 3 Byte AFE-Gains, je Kamera | 9 | **Gegen Kaltstart gemessen, Vorzustand ausgeschlossen:** 4011 bzw. 4056 Zeilen, Footer korrekt, Bild vollständig lesbar. Helligkeit am **selben, zweimal gezogenen Blatt** verglichen: ohne `gains` Mittelwert 200.6, mit 205.9; Papierweiß in beiden gesättigt (Anteil 255: 74.5 % gegen 78.9 %). Die AFE-Gains nach dem Einschalten liegen also schon im brauchbaren Bereich, knapp unter den Herstellerwerten **[E]** |

Alle Einträge dieser Tabelle außer `gains` sind nur am warmen Gerät
gemessen. Bei `ident`, `flash`, `poll*`, `housekeep`, `session_*` und
`docid2` ist der Wegfall nicht nur gemessen, sondern erklärbar: sie lesen
oder führen Buch und berühren keinen Bildweg. Bei `standby`, `op1b` und
`docid`, die auf die Hardware schreiben, steht die Kaltstartprüfung
dagegen noch aus — dort kann derselbe Vorzustandseffekt stecken, der sich
bei `d9regs` und `daregs` gezeigt hat.

Für ein SANE-Backend bleiben aus dem entbehrlichen Teil nur zwei Requests
interessant: `GetEndOfLine` (`C0 34`, Modellstring für `sane_get_devices`)
und `GetStatus` (`C0 00`) für `sane_start` — Papierweg, ADF leer, Deckel
offen, kritischer Fehler, siehe `INITIALISIERUNG.md` Abschnitt 9.

### Die Fehlschläge, ehrlich protokolliert

Drei Dinge gingen schief; sie sind für den nächsten Schritt mehr wert als
die Erfolge.

1. **Kaltes Gerät: jeder Request außer `GetStatus` stallt.** Opcode `0x02`,
   `LIBUSB_ERROR_PIPE`, und danach `0x34`, `0x32`, `0xa3`, `0xe0` ebenso —
   während `GetStatus` zwischen den Fehlversuchen jedes Mal weiter
   durchging, EP0 also nicht blockiert war. Ursache: Booter-Firmware statt
   Anwendungsfirmware, siehe Abschnitt 2. Im Mitschnitt gibt es diese
   Stelle nicht: alle sechs Mitschnitte beginnen mit `status[0] = 03`,
   meiner mit `01`. Das war der entscheidende Hinweis. **Nachtrag:** die
   Ursache ist gefunden und der Download nachgebaut (Abschnitt 2); `40 21`
   nimmt der Booter übrigens doch an.
2. **`--pages 1` erzeugte einen Papierstau.** Wird der Transport gestoppt,
   während ein Blatt zwischen den Rollen liegt, meldet das Gerät danach bei
   jedem Start `EP88 30 Paper Jam` (Value1 = 64), und `status[0x10]` steht
   auf 1 statt 2 („Papierweg frei"). Das Gerät zieht das Blatt **nicht**
   von selbst durch; es muss von Hand entfernt werden. Das Programm
   stoppt jetzt nach dem Seitenlimit erst den Transport und liest leer,
   bevor es aufhört — danach trat der Stau nicht wieder auf. **[E]**
3. **„Ein Block kam an" ist kein Erfolgskriterium.** Die erste Messreihe
   lief nur gegen Blockgröße und Footer und hielt deshalb `lamp` für
   entbehrlich. Mit Inhaltsprüfung zeigte sich: ohne Lampe liefert das
   Gerät 4057 Zeilen gültig geformter Schwärze. Alle Zahlen in diesem
   Abschnitt stammen aus der wiederholten Messreihe mit Bildprüfung.
4. **`i2600-grab` hängt, wenn gar kein Bildblock kommt.** Beim ersten
   Kaltstartversuch (`-s d9regs,daregs,gains`, mit Referenzdurchgang) blieb
   EP 0x82 stumm, und das Programm stand unbegrenzt: Haupt-Thread im
   Futex-Wait, Interrupt-Thread pollte weiter. Ursache ist die synchrone
   libusb-API in zwei Threads — `libusb_bulk_transfer` im Haupt-Thread,
   `libusb_interrupt_transfer` im Ereignis-Thread; bleibt der Bulk-Transfer
   aus, läuft sein Timeout nicht ab. Für ein Backend heißt das: die
   asynchrone libusb-API nehmen oder beide Kanäle über einen Thread fahren.
   Die Kaltstartmessreihe lief deshalb mit `-R` und einer äußeren Zeitgrenze
   (`timeout -s KILL 90`). **[E]**

## 6. Was als Nächstes dran wäre

1. ~~**Den Firmware-Download nachbauen**~~ — **erledigt**, siehe Abschnitt 2.
   `i2600-grab` nimmt ein kalt angestecktes Gerät selbst in Betrieb.
   Offen geblieben ist davon nur, was ein Backend ohnehin nicht anfassen
   sollte: `TestFpgaMemory()` (FPGA-Speichertest) und `ProgramBooter()` /
   `IsBooterUpdateNeeded()` (Booter-Flash über Opcode `0xa2`). **[?]**
2. ~~**`d9regs`, `daregs` und `gains` gegen einen echten Kaltstart messen.**~~
   — **erledigt**, siehe Abschnitt 5: `d9regs` und `daregs` sind vom
   Kaltstart aus unverzichtbar und stehen jetzt dort, `gains` ist auch kalt
   entbehrlich. Offen bleibt davon nur, ob die restlichen Schreibgruppen
   (`standby`, `op1b`, `docid`) einen Kaltstart ebenfalls überstehen, und ob
   eine geschlossene Mindestmenge vom Kaltstart aus trägt. **[?]**
3. **Duplex:** EP 0x86 parallel lesen. Das Programm liest bisher nur
   `EP_IMAGE_FRONT`; `c-duplex-lineart-200.pcapng` zeigt, dass der
   Rückseitenkanal strukturgleich ist.
4. **Beschnittgrenzen** aus den NvRam-Kalibrierdaten (`C0 35 wValue=0`),
   siehe `INITIALISIERUNG.md` Abschnitt 13 Punkt 2 — damit ließen sich die
   4080 Rohzeilen auf das Blatt zuschneiden, ohne Perfect Page.
5. **Als SANE-Backend verpacken:** `sane_get_parameters` meldet 2580 × n bei
   8 bit grau; Auflösung, Bittiefe und Bitonal-Wandlung macht der Host, weil
   das Gerät ohnehin nur den einen Modus hat.

---

## 7. Duplex — funktioniert **[E]**

`i2600-grab --duplex` scannt beidseitig: Vorder- und Rückseite kommen als
getrennte PGM-Dateien, `<prefix>-pageN.pgm` und `<prefix>-pageN-rear.pgm`.

```
[ 1.053] ep 82: block of 51824 bytes in 4 transfers (len mod 508 = 8)
[ 1.053] ep 86: block of 51824 bytes in 4 transfers (len mod 508 = 8)
[ 3.582] ep 86: block of 10469888 bytes in 640 transfers
[ 3.701] ep 82: block of 10495796 bytes in 641 transfers
         footer beide: width=2580 px, 300/300 dpi, Footer parst
[ 3.709] wrote native-duplex-page1.pgm      (2580 x 4068)
[ 3.716] wrote native-duplex-page1-rear.pgm (2580 x 4058)
```

Drei Blätter, sechs Bilder, Rückseiten inhaltlich verschieden von den
Vorderseiten und lesbar. Die Rückseite steht auf dem Kopf — die zweite Kamera
sieht das Blatt von der anderen Seite, und die Rotation macht beim Hersteller
Perfect Page. Für ein natives Backend ist das eine Drehung um 180°.

### Was Duplex ausmacht — und was der Irrweg war **[M][E]**

Duplex ist **nicht** die Simplexsequenz plus ein paar Rückseitenregister. Der
Herstellertreiber **verschränkt beide Kameras über die ganze Sequenz**. Drei
Anläufe, die das nicht taten, scheiterten alle am selben Symptom: EP 0x86
lieferte pro Seite genau einen 516-Byte-Block mit gültigem Footer, aber null
Bildzeilen, und mit widersprüchlichen Werten (`width=1030`, dann `width=0`,
einmal 600 statt 300 dpi).

| Anlauf | Idee | Ergebnis |
|---|---|---|
| 1 | 20 `dbxx`-Schreibzugriffe als Gruppe anhängen, zwei Bytes im `0x30`-Block | 516 Byte |
| 2 | `db0e`/`db06`/`db07` in die Scan-Vorbereitung, `StartCapture` 1 → 3 | 516 Byte |
| 3 | fehlende **Lesezugriffe** ergänzen (`db11`–`db16`, 6× `db19`/`db1a`), `dd0b`/`dd0a` ans Ende | 516 Byte |
| 4 | **die ganze Sequenz positionsgetreu aus dem Duplex-Mitschnitt erzeugen** | **funktioniert** |

Jeder einzelne Fund der Anläufe 1–3 war real — die Register fehlten
tatsächlich, die Lesezugriffe auch, und die `dd0b`/`dd0a`-Folge stand
wirklich an der falschen Stelle. Nur war keiner davon *die* Ursache. Was
zählte, war die Reihenfolge im Ganzen.

`gen-duplex.py` erzeugt deshalb jetzt dieselben Phasen wie `gen-sequence.py`,
aber aus `c-duplex-lineart-200.pcapng` und byte-exakt in Mitschnittreihenfolge:
`open` 100, `prepare` 213, `ref_config` 1, `scan_prep` 6, `scan_config` 1,
`teardown` 77 Transfers. Die Phasengrenzen stehen nicht als feste Indizes
darin, sondern werden über Landmarken gefunden (`ScannerOperation 0x0d`, die
beiden 120-Byte-`0x30`-Blöcke), weil der Duplex-Mitschnitt 405 statt 362
Vendor-Transfers hat. `--duplex` tauscht in `i2600-grab` die komplette
Tabelle samt Nutzdaten-Blob.

Der Unterschied ist in den Phasengrößen direkt sichtbar: `prepare` wächst von
171 auf 213 Transfers, `scan_prep` von 3 auf 6 — dort verschränkt der
Hersteller `da0e, db0e, da06, …` statt erst alle `da` und dann alle `db` zu
schreiben.

### Nebenbefund: der Papierstau ist weg **[E]**

Vorher verklemmte sich das Gerät in Duplex reproduzierbar am zweiten Blatt
(`status[0x10] = 1`, von Hand zu lösen, meist nur durch Stromlosmachen). Seit
die Rückseitenkamera richtig konfiguriert ist, tritt das nicht mehr auf —
offenbar wartete die Papierführung auf eine Bildquelle, die nie fertig wurde.
`i2600-grab` meldet einen Stau jetzt ausserdem im Klartext statt nur
„no further image block".

### Offen **[?]**

* Die Rückseite kommt um 180° gedreht; ein Backend muss sie drehen.
* Die Zeilenzahlen von Vorder- und Rückseite unterscheiden sich leicht
  (4068 gegen 4058), weil die Kameras das Blatt zu minimal verschiedenen
  Zeitpunkten sehen. Für einen Beschnitt ist das zu berücksichtigen.
* `--skip` kennt für Duplex nur die eine Gruppe `duplex`; die
  Einzelgruppenmessung aus Abschnitt 5 gilt nur für die Simplexsequenz.


---

## 8. Rückseitendrehung und die NvRam-Kalibrierdaten

### Drehung: zweimal umgedacht **[E]**

Erst gab es eine 180°-Drehung **nur für die Rückseite**. Das war falsch und
aus einer Einzelbeobachtung abgeleitet: beim ersten Duplexversuch stand die
Rückseite auf dem Kopf, also wurde sie gedreht — die Vorderseite stand
genauso, nur wurden die beiden nie nebeneinandergelegt. Ergebnis: die
Rückseite passte für sich, aber nicht mehr zur Vorderseite.

Die Gegenprobe am Herstellertreiber, dasselbe Blatt in Farbe und Duplex:

| | Vorderseite | Rückseite |
|---|---|---|
| Hersteller | auf dem Kopf | **auf dem Kopf** |

**Der Hersteller dreht nicht.** Beide Kameras liefern das Blatt gleich
herum.

Jetzt wird **alles** gedreht, Vorder- wie Rückseite, als Option `--rotate`
mit Vorgabe **an**. Begründung ist die Benutzung, nicht das Protokoll: die
Blätter liegen bei diesem Einzug naheliegenderweise mit der Schrift nach
oben, und dann kommt ohne Drehung alles verkehrt herum heraus. Dass der
Herstellertreiber dieselbe Unbequemlichkeit hat, ist kein Grund, sie
nachzubauen. Wer anders einlegt, setzt `--rotate=no`.

Die Drehung läuft **nach** dem Beschnitt und für beide Seiten gleich, damit
ein Paar konsistent bleibt. Automatische Drehung nach Bildinhalt, wie
Perfect Page sie macht, gibt es weiterhin nicht. **[?]**

### Wie die Drehung nachgewiesen wurde **[E]**

Ein Scan allein beweist sie nicht: legt man die Blätter andersherum ein,
sieht das Ergebnis mit und ohne Drehung gleich aus. Der Nachweis geht über
den **Referenzdurchgang**, der kein Papier braucht — zweimal gelaufen, einmal
mit und einmal ohne `--rotate`:

```
"mit Drehung" gegen "ohne"          : 1,98
"mit Drehung" gegen "ohne", gedreht : 0,47
```

(mittlere absolute Abweichung je Pixel; exakte Gleichheit ist nicht zu
erwarten, weil der Referenzdurchgang live vom Sensor liest und rauscht).
Das gedrehte Gegenstück passt deutlich besser — die Drehung greift.

### Der NvRam-Kalibrierblock **[E]**

`i2600-nvram` liest ihn direkt vom Gerät (`C0 35 wValue=0`, 128 Byte). Zwei
Dinge waren beim ersten Versuch falsch: der Block beginnt mit einem
**ASCII-Kalibrierdatum** (`2014/02/13 10:32:04:056`, 24 Byte mit NUL), und die
Zahlenfelder liegen deshalb auf **ungeraden** Offsets. Ein naives
Even-Offset-Raster übersieht jedes einzelne Feld.

Mit den vier in `INITIALISIERUNG.md` Abschnitt 8 bereits identifizierten
Werten als Anker ergibt sich die Feldlage eindeutig, alle `u16 LE`:

| Offset | Wert | Bedeutung |
|---|---|---|
| `0x00` | ASCII | Kalibrierdatum, 24 Byte mit NUL |
| `0x1b` | 10117 | vier Werte um 10100, Bedeutung **offen [?]** |
| `0x1d` | 10125 | |
| `0x1f` | 10157 | |
| `0x21` | 10064 | |
| `0x2b` | **901** | `u16FrontCameraDistance` |
| `0x2d` | **373** | `u16RearCameraDistance` |
| `0x2f` | **3126** | `u16MagnificationTargetFixedLength` |
| `0x31` | **3122** | `u16MagnificationTargetCalibrationLength` |
| `0x33` | **8** | `u16FrontLeftPixel` |
| `0x35` | **45** | `u16RearLeftPixel` |
| `0x37` | **2550** | `u16FrontLineWidth` |
| `0x39` | **2540** | `u16RearLineWidth` |
| `0x3d` | 1793 | **offen [?]** |

### Die Zuordnung ist gesichert **[C]**

Die Dumpstrings in `devicemanager.so` stehen in Strukturreihenfolge:

```
u16FrontCameraDistance ... u16RearCameraDistance
u16MagnificationTargetFixedLength ... u16MagnificationTargetCalibrationLength
u16FrontLeftPixel ... u16RearLeftPixel
u16FrontLineWidth ... u16RearLineWidth
```

Das deckt sich Feld für Feld mit der gemessenen Offsettabelle, und die beiden
unabhängig bekannten Anker 3126 und 3122 landen genau auf den Positionen 3
und 4, die diese Reihenfolge vorhersagt. Damit sind auch 901 und 373
benannt: es sind die **Kameraabstände**, nicht die Leading-/Trailing-Edge-
Offsets, als die `INITIALISIERUNG.md` Abschnitt 8 sie führt — dort stammen
sie aus einer anderen Struktur.

### Was das für den Beschnitt heißt: nichts **[C][M]**

Die Frage war falsch gestellt. **Der Beschnitt passiert im FPGA, nicht im
Host.** Zwei winzige Setter in `devicemanager.so` zeigen es:

```
FpgaBase::SetFrontHorizontalCropLeft(u16)  -> SetHiLoPair(0xda02, v)
FpgaBase::SetFrontImageWidth(u16)          -> SetHiLoPair(0xda04, v)
FpgaBase::SetRearHorizontalCropLeft(u16)   -> SetHiLoPair(0xdb02, v)
FpgaBase::SetRearImageWidth(u16)           -> SetHiLoPair(0xdb04, v)
```

`SetHiLoPair(reg, v)` legt das High-Byte nach `reg` und das Low-Byte nach
`reg+1`. Genau diese Register stehen in den Mitschnitten, und die Werte sind
**[M]**:

| | CropLeft | ImageWidth |
|---|---|---|
| Vorderseite | **4** | **1290** Paare = 2580 px |
| Rückseite | **22** | **1290** Paare = 2580 px |

Das Gerät liefert die Zeile also **bereits beschnitten**. Die 2580 Pixel, die
auf `0x82` und `0x86` ankommen, sind das fertige Ergebnis — horizontal ist
nichts mehr zu tun. `i2600-grab` schreibt sie unverändert ins PGM, und das
ist richtig so.

### Und die fünf Pixel lösen sich auf **[M]**

`CropLeft` zählt in **Pixelpaaren**, wie `ImageWidth` und die Breitenangabe im
Bildfooter auch. Die NvRam-Werte passen dazu:

```
FrontLeftPixel 8  -> CropLeft  4    (8/2)
RearLeftPixel  45 -> CropLeft 22    (45/2, abgerundet)
```

Die NvRam-Werte sind also **Eingaben** einer Rechnung, nicht selbst die
Grenzen. `8 + 2550` und `45 + 2540` direkt auf eine 2580-Pixel-Zeile zu
beziehen war der Fehler — die Summen mussten nirgends hineinpassen.

**Dass der Treiber genau `LeftPixel / 2` rechnet, ist allerdings nicht
belegt [?].** `SetupScanner::SetupHorizontalCrop(CAMERATYPE, EDMIMAGESCANTYPE)`
holt den Wert nicht aus den Kalibrierdaten, sondern aus einer Tabelle:

```c
uVar1 = *(u16 *)(PTR_s_dmscantypeinfo + (long)scantype * 0x14 + 4);
if (camera == 1) FpgaBase::SetFrontHorizontalCropLeft(uVar1);
else             FpgaBase::SetRearHorizontalCropLeft(uVar1);
```

`s_dmscantypeinfo` (`0x552da0`, 340 Byte, 17 Einträge à 0x14) hat an Offset +4
aber in **jedem** Eintrag eine Null — die Tabelle wird zur Laufzeit gefüllt.
Welcher Code das tut und ob dabei `LeftPixel` halbiert wird, ist noch
offen. Die Übereinstimmung 8→4 und 45→22 ist ein starker Hinweis, mehr
nicht.

`u16Front/RearLineWidth` (2550 / 2540) gehen **nicht** in `ImageWidth` ein:
das steht bei beiden Kameras auf 1290 Paare, während `LineWidth/2` 1275 bzw.
1270 ergäbe. Wofür sie dienen — vermutlich Magnifikationsprüfung, es gibt
`CMechanicalCalData::IsInSpec(u16, u16, double)` — ist **offen [?]**.

Ein Nebenbefund als Gegenprobe: die gemessenen linken Ränder der Bilder
unterscheiden sich um 42 px (vorne 95, hinten 53 unrotiert), und die
CropLeft-Differenz beträgt `(22-4) x 2 = 36 px`. Der Rest ist die Lage des
Blattes selbst. **[M]**

### Herkunft der Werte: vollständig geklärt **[C][E]**

Die Kette steht fest: `SetupFPGA` → `SetupHorizontalCrop` →
`SetFront/RearHorizontalCropLeft` → Register `0xda02`/`0xdb02`. Eine
Konstantensuche über das ganze Disassemblat zeigt, dass nur diese beiden
Setter die Register anfassen. Der geschriebene Wert kommt aus
`s_dmscantypeinfo[scantype].u16@+4`:

```asm
150ee8: movzwl 0x4(%rax),%r12d        ; rax = Tabelle + scantype*0x14
150f98: movzwl %r12w,%esi
150f9f: call   SetFrontHorizontalCropLeft
```

*(Ghidras Dekompilat behauptet hier, der Wert käme aus `DeviceSettings+0x140`,
und nennt den Tabellenwert nur im Logaufruf. Das Disassemblat widerlegt das:
`%esi` wird unmittelbar vor beiden Aufrufen aus `%r12w` geladen.)*

**Statisch ist dieses Feld in allen 17 Einträgen null und hat keinen
Schreibzugriff** — Ghidra findet nur zwei Leser. Das sah lange nach einem
Widerspruch zu den Mitschnitten aus, die 4 und 22 zeigen. Es war keiner: die
Tabelle wird **zur Laufzeit** gefüllt, und die Schreibzugriffe laufen über
einen wandernden Zeiger, den die statische Referenzanalyse nicht auflöst.

Nachgewiesen durch Mitlesen des Speichers eines laufenden
`scanimage`-Prozesses (`/proc/<pid>/mem`, 10 ms Abtastung, Scan mit dem
Herstellertreiber) **[E]**:

```
t=0.95s  Eintrag 8:  +2=    0  +4=  0      Ausgangszustand wie in der Datei
t=2.60s  Eintrag 8:  +2= 1290  +4=  4      Vorderseite
t=2.62s  Eintrag 8:  +2= 1290  +4= 22      Rückseite
```

Das `+4`-Feld wird **je Kamera neu beschrieben**, 20 ms auseinander, direkt
vor dem jeweiligen `SetupHorizontalCrop`-Aufruf. Damit ist die Rechnung
belegt, die vorher nur plausibel war:

```
FrontLeftPixel  8 >> 1 =  4   -> FPGA 0xda02/0xda03
RearLeftPixel  45 >> 1 = 22   -> FPGA 0xdb02/0xdb03
LineWidth    2580 >> 1 = 1290 -> FPGA 0xda04/0xda05 bzw. 0xdb04/0xdb05
```

**Alles zählt in Pixelpaaren**, Beschnittrand wie Breite wie die Angabe im
Bildfooter. Die `+2`-Hälfte stammt aus `SetupScanner::SetupScanner`:

```c
u16 w = *(u16 *)(DeviceSettings + 0x148);          /* 2580 */
for (i = 1; i <= 16; i++) {
    if ((1 << i) & 0x9a20)       entry[i].u16_at_2 = w >> 1;   /* 1290 */
    else if ((1 << i) & 0x16000) entry[i].u16_at_2 = w;        /* 2580 */
}
```

Für bestimmte Scantypen (Maske `0x16000`) unterbleibt die Halbierung; der im
Mitschnitt benutzte Scantyp ist **8**.

Zwei Sackgassen auf dem Weg dorthin, als Warnung für den nächsten: ein
`LD_PRELOAD`-Haken auf die Setter greift **nicht** — die Aufrufe innerhalb
von `devicemanager.so` sind direkt (`call 150dd0`) und laufen nicht über die
PLT. Und eine Abtastung mit 200 ms ist zu grob: sie sieht nur den
Rückseitenwert und lässt den Vorderseitenwert verschwinden.

### Praktische Folge für ein Backend **[M]**

Unabhängig davon, wie der Treiber auf 4 und 22 kommt: **ein Backend muss
horizontal gar nichts rechnen.** Das Gerät liefert 2580 beschnittene Pixel je
Zeile, und `i2600-grab` schreibt sie unverändert. Die Kalibrierdaten sind nur
dann interessant, wenn man den Beschnitt selbst verschieben will.

---

## 9. Das SANE-Backend

`libsane-i2600.so.1` macht den Scanner für `scanimage`, `simple-scan`, XSane
und alles andere nutzbar, was SANE spricht — ohne jede Kodak-Bibliothek.

### Aufteilung

Der Code ist in drei Teile zerlegt, damit CLI und Backend dieselbe
Gerätelogik benutzen:

| Datei | Inhalt |
|---|---|
| [i2600-core.c](i2600-core.c) / [.h](i2600-core.h) | USB-Transport, Vendersequenz, Bildblöcke, Footer, Firmware-Kaltstart |
| [i2600-grab.c](i2600-grab.c) | Kommandozeile, PGM-Ausgabe |
| [i2600-sane.c](i2600-sane.c) | SANE-API |
| [i2600-scand.c](i2600-scand.c) | Tastendienst, PDF-Ausgabe (Abschnitt 16) |

Die Kern-API ist klein: `i2600_device_open/close/prepare`,
`i2600_reference`, `i2600_scan_begin` / `_next_page` / `_end`, dazu
Footer-Parser, 180°-Drehung und `i2600_get_status`. Für Bedienfeld und
Zustände kommen `i2600_next_button`, `i2600_power_saver/_state`,
`i2600_lamp` und `i2600_lcd_write` dazu.

### Optionen **[E]**

```
--mode Gray [Gray]                 das Gerät kann nur 8 bit grau
--resolution 300dpi [300]          und nur 300 dpi
--source ADF Front|ADF Duplex      ein- oder beidseitig
-l -t -x -y                        Geometrie, gemeldet aber nicht befolgt
```

Bewusst klein gehalten: das Gerät hat genau einen Modus, und der Beschnitt
passiert im FPGA (Abschnitt 8). Skalierung, Bittiefe und Bitonal-Wandlung
sind Sache der Anwendung. Die Geometrieoptionen müssen trotzdem **setzbar**
sein, sonst scheitern Frontends, die eine Scanfläche setzen
(`scanimage --batch`, simple-scan) mit „Invalid argument".

### Drei Stolpersteine, die Arbeit gekostet haben **[E]**

1. **`libsane-dll` bindet die blanken `sane_*`-Symbole nicht.** Es sucht
   zuerst `_sane_<backend>_<op>`, und wenn das fehlschlägt, fällt es auf
   Stubs zurück statt auf die ungeprefixten Namen — `scanimage -L` meldet
   dann einfach „No scanners were identified". Backends im sane-backends-Baum
   bekommen die Präfixnamen aus `sanei_backend.h`, das nicht mitinstalliert
   wird. Lösung: beide Schreibweisen per `__attribute__((alias))` exportieren.
2. **Eine Bibliothek darf nicht nach stdout schreiben.** Die Logzeilen des
   Kerns landeten im PNM-Strom. `logline()` geht jetzt über eine Senke
   (`i2600_log_sink`) und schweigt, wenn kein Frontend Fortschritt will.
3. **Das Gerät darf nicht in `sane_open()` geöffnet werden.** Frontends
   setzen Optionen *nach* dem Öffnen, und die Duplexwahl entscheidet, welche
   Vendersequenz läuft — `--source ADF Duplex` wurde deshalb abgewiesen. Der
   USB-Teil kommt jetzt beim ersten `sane_start()` hoch. Nebeneffekt: ein
   `scanimage -L` lädt keine Firmware mehr in einen kalten Scanner.

### Ein vierter Stolperstein: `sane_cancel` **[E]**

Frontends rufen `sane_cancel()` nicht nur zum Abbrechen, sondern auch am
**normalen Seitenende** — `scanimage --batch` tut das. Die erste Fassung warf
dort die zurückgelegte Rückseite weg und beendete den Stapel: aus drei
Blättern kamen fünf statt sechs Bilder, und zwar fehlte genau die Rückseite
des letzten Blattes. Die CLI lieferte mit demselben Papier alle sechs, was
den Fehler im SANE-Pfad statt im Kern verortete.

Behoben, indem ein Abbruch nach vollständig gelesenem Bild als Seitengrenze
behandelt wird und nur ein Abbruch *mitten* im Bild den Stapel abräumt.

### Stand **[E]**

* **Simplex** über `scanimage`: zwei Blätter, 2580 × 4117 bzw. 2580 × 4071,
  8 bit grau, lesbar.
* **Duplex** über `scanimage --source "ADF Duplex"`: drei Blätter, **sechs**
  Bilder, Vorder- und Rückseiten abwechselnd, Rückseiten um 180° gedreht und
  aufrecht lesbar.
* **Leerer Einzug** meldet `Document feeder out of documents` (Exit 7) statt
  zu hängen.

### Bauen und installieren

```bash
cd native
make                 # alles: i2600-grab, i2600-status, i2600-nvram, Backend
make check           # Backend ohne Installation ausprobieren
sudo make install    # nach /usr/lib/sane + /etc/sane.d/dll.d/i2600
sudo make uninstall
```

---

## 10. Autobeschnitt **[E]**

Das Gerät liefert immer das ganze Transportfenster; das Blatt sitzt in einem
deutlich dunkleren Rand. Der Herstellertreiber trimmt das host-seitig in
Perfect Page — dafür brauchen wir ihn nicht, das ist gewöhnliche
Bildverarbeitung.

`i2600_autocrop()` im Kern bestimmt die Schwelle **aus dem Bild** statt fest,
damit eine dunkle Seite nicht für Hintergrund gehalten wird: das hellste
Zehntel der Zeilenmittel gilt als „so sieht Papier hier aus", und alles unter
einem Viertel davon als Rand. Erst die Zeilen, dann — nur innerhalb des
gefundenen Bandes — die Spalten. Wird keine Seite erkannt, bleibt das volle
Bild stehen, statt zu raten.

Im Backend als `--crop yes|no` (Vorgabe an). Am Gerät geprüft, drei Blätter:

| | Rohbild | zugeschnitten | entspricht |
|---|---|---|---|
| Seite 1 | 2580 × 4096 | 2445 × 3532 | 207 × 299 mm |
| Seite 2 | 2580 × 4066 | 2458 × 3522 | 208 × 298 mm |
| Seite 3 | 2580 × 4083 | 2458 × 3534 | 208 × 299 mm |

A4 sind 210 × 297 mm — die Erkennung trifft das konsistent auf wenige
Millimeter. Der Transportrand ist vollständig weg.

**Nicht enthalten:** Deskew und automatische Drehung. Perfect Page dreht ein
verkehrt eingezogenes Blatt zusätzlich richtig herum; das tut dieses Backend
nicht. **[?]**

---

## 11. Farbmodus **[M][E]**

Der frühere Schluss „das Gerät scannt immer gleich" war **über die Stichprobe
hinaus verallgemeinert**: alle sechs ursprünglichen Mitschnitte waren Lineart
oder Graustufen. Ein Farbmitschnitt des Herstellertreibers
(`scanimage -d kds_i2000:i2000 --mode Color --resolution 300`) zeigt ein
anderes Bild.

### Was Farbe am Draht ändert **[M]**

Die Datenmenge verdreifacht sich bei gleicher Geometrie:

| | Referenzblock | Seitenblock | EP 0x82 gesamt |
|---|---|---|---|
| Grau | 51 824 B | ~10,5 MB | 21,0 MB |
| Farbe | 154 948 B | ~31,4 MB | 94,2 MB |

`154948 / 51824 = 2,99` und `31 385 772 / (2580 × 3) = 4055` Zeilen bei
72 Byte Footer. Also **3 Byte je Pixel, RGB interleaved, unverändert 2580
Pixel je Zeile**. Die Zeilenbreite im Footer bleibt 1290 Paare.

### Was umgeschaltet wird **[M]**

Ein Registervergleich Grau gegen Farbe (mit Lesezugriffen) ergibt:

| Register | grau | farbe | |
|---|---|---|---|
| `a3 d900` | `01` | **`20`** | sieht nach dem Modusregister aus |
| `a3 da01` | `00` | `10` | |
| `a3 da12` | `41` | `40` | |
| `a3 da13`–`da16` | — | `01 40 01 4b` | nur in Farbe |
| `a3 da25` | `01` | `00` | |
| `a3 IN da19`/`da1a` | je 3× | je 12× | |
| `a3 IN da1b`–`da1e` | — | je 12× | Weißabgleich je Kanal |
| `e0` Kameraregister | `9155…` | `f154…` | andere Nutzdaten |

Also kein einzelnes Modusbit, sondern ein Bündel aus FPGA-Registern,
zusätzlicher Kamerakonfiguration und kanalweisem Weißabgleich — genau die
Lage, in der das Zusammensetzen aus Teilen bei Duplex dreimal scheiterte.
Deshalb erzeugt `gen-color.py` auch hier die **vollständige, positionsgetreue**
Sequenz nach `sequence-color.h`: `open` 100, `prepare` **240**, `ref_config` 1,
`scan_prep` 3, `scan_config` 1, `teardown` 77 Transfers.

### Zwei Fallstricke **[E]**

1. **`tools/protocol/ctrl.py` filterte fest auf Gerätenummer 6.** Der
   Farbmitschnitt hatte Nummer 7 — der Generator fand schlicht keine
   Landmarke. Die Nummer hängt davon ab, wann der Scanner zuletzt
   eingesteckt wurde; `ctrl.transfers()` ermittelt sie jetzt selbst.
2. **Die 180°-Drehung muss pixelweise sein.** Bytes umzukehren vertauscht
   bei RGB auch die Kanäle innerhalb jedes Pixels. `i2600_rotate180()`
   nimmt jetzt die Pixelgröße entgegen.

### Stand **[E]**

`scanimage --mode Color` über das eigene Backend, drei Blätter:

```
col-01.pnm  P6  2443 x 3541  = 207 x 300 mm
col-02.pnm  P6  2456 x 3522  = 208 x 298 mm
col-03.pnm  P6  2456 x 3530  = 208 x 299 mm
```

Bytezahlen stimmen mit `w × h × 3`, Autobeschnitt greift, Farben sind
plausibel (Kanalmittel R 88, G 66, B 78; bei vertauschtem R und B wäre das
Magenta des Prospekts cyan). Die CLI kennt `--color` und `--crop` ebenfalls.

### Farb-Duplex **[M][E]**

Eigener Mitschnitt mit `--mode Color --source "ADF Duplex"` (47 MB), daraus
`gen-color-duplex.py` nach `sequence-color-duplex.h`: `open` 100,
`prepare` **233**, `ref_config` 1, `scan_prep` **6**, `scan_config` 1,
`teardown` 80 Transfers. `scan_prep` mit sechs statt drei Transfers zeigt
wieder die verschränkten Kameras, und `StartCapture` trägt die Maske 3.

Damit gibt es **vier** Sequenzen — grau/farbig mal simplex/duplex — und jede
ist positionsgetreu aus ihrem eigenen Mitschnitt erzeugt. `RUN()` wählt sie
zur Laufzeit. Jeder Versuch, eine aus Teilen einer anderen zusammenzusetzen,
ist gescheitert; das gilt ausdrücklich auch hier.

Am Gerät geprüft, drei Blätter, sechs Bilder:

```
cd-01 Vorder  P6  2445 x 3534 = 207 x 299 mm
cd-02 Rueck   P6  2445 x 3402 = 207 x 288 mm
cd-03 Vorder  P6  2456 x 3523 = 208 x 298 mm
cd-04 Rueck   P6  2449 x 3539 = 207 x 300 mm
cd-05 Vorder  P6  2454 x 3526 = 208 x 299 mm
cd-06 Rueck   P6  2445 x 3542 = 207 x 300 mm
```

Rückseiten gedreht, Farben korrekt, Beschnitt greift. Die etwas kürzere
Seite 2 ist kein Datenverlust — das Bild ist vollständig, die Kante wurde
nur ein paar Millimeter anders erkannt. Für eine Heuristik ist das im
Rahmen. **[E]**

**Offen [?]:** Deskew und automatische Drehung verkehrt eingezogener Blätter


---

## 12. Vorabprüfung der Scanbereitschaft **[C][E]**

`i2600_check_ready()` bildet `Scanner::IsOkToStartScan()` des Herstellers
nach (INITIALISIERUNG.md Abschnitt 9) und wird in `sane_start()` vor dem
ersten Scan eines Stapels aufgerufen:

| Status | SANE-Meldung |
|---|---|
| `st[0x13] == 2` | `SANE_STATUS_COVER_OPEN` |
| `st[0x10] != 2` | `SANE_STATUS_JAMMED` |
| `st[0x0f] == 1` | `SANE_STATUS_NO_DOCS` |
| `st[0x0e] != 0` | `SANE_STATUS_DEVICE_BUSY` |
| `st[0x1f] & 0x80` | `SANE_STATUS_IO_ERROR` |

Vorher meldete das Backend einen Stau erst, nachdem das Gerät es vergeblich
versucht hatte; jetzt kommt die Meldung nach rund einer Sekunde und benennt
die Ursache. Frontends wie simple-scan zeigen damit „Papierstau" statt eines
allgemeinen Fehlers.

---

## 13. Die Tasten am Bedienfeld **[M][E]**

`i2600-grab`s Schwester `i2600-buttons` horcht am Gerät; das Backend macht die
Taste als SANE-Sensor verfügbar, womit `scanbd` daraus einen Scan auslösen
kann.

### Wo die Tasten melden — nicht dort, wo es im Code steht **[E]**

Die eingebettete Pipe-Konfiguration kennzeichnet **EP 0x81** mit
`OS_USBPIPEFLAGS_MONITORBUTTONEVENTS` und EP 0x88 mit
`...NOTMONITORBUTTONEVENTS`. Das legt nahe, dass Tasten auf 0x81 kommen.
**Sie tun es nicht.** Zwei Läufe mit bestätigtem Drücken brachten dort null
Nachrichten, während die Drücke zeitgleich auf **EP 0x88** erschienen:

```
[ 9.743] EP88  20 Button Press  id=1 value1=9
```

Ereignistyp **0x20**, auf dem Kanal, den das Backend ohnehin schon liest.

### Was im Mitschnitt nicht zu finden war **[M]**

Ein usbmon-Mitschnitt des Herstellertreibers half hier **nicht**: sein
SANE-Backend sendet beim blossen Offenhalten kein `0x3a
StartInterruptEvents` und pollt nicht — in 73 Sekunden gab es ausser dem
Öffnen und Schliessen keinen einzigen Transfer. Die Antwort kam erst, als
das Gerät direkt gefragt wurde.

### Die Zahl im Display **[E]**

`SetButtonSequenceNumber` (`0x16`) schreibt die angezeigte Zahl: mit
`wValue=3` steht am Gerät eine 3, am Nutzer bestätigt. Nach dem Einschalten
steht 0; die **9** stammt aus unserer eigenen Initialisierung, weil
`sequence.h` den Aufruf `0x16 wValue=0x0009 wIndex=0x01ff` aus dem
Herstellermitschnitt mitführt (Gruppe `housekeep`). Jeder Zugriff auf das
Gerät setzt sie damit zurück.

Die Pfeiltasten verstellen die Zahl **nicht** — in diesem Modus setzt der
Host sie. Ein Versuch mit einer nicht hinterlegten Zahl quittiert das Gerät
mit `EP88 60 Screen Missing value1=0x0103`; die Anzeigetexte müssten über
`0x62` hochgeladen werden (`OCP::LoadMessages`, `OCP::LcdPopulate`), was der
volle Herstellertreiber tut und unsere Sequenz nicht. **[?]**

### Als SANE-Sensor **[E]**

```
--scan[=(yes|no)] [no] [hardware]   wahr einmal nach einem Tastendruck
--function <int> [0] [read-only]    die Zahl, die der Druck mitschickt
```

`[hardware]` ist `SANE_CAP_HARD_SELECT` — genau das Merkmal, auf das `scanbd`
achtet. Das Lesen setzt das Flag zurück. Mit einem SANE-Client gegengeprüft,
der einmal öffnet und dann pollt, wie `scanbd` es täte:

```
option indices: scan=10 function=11
[ 15.2] BUTTON -> scan=1 function=9
...
done: 11 press(es) seen through SANE
```

### Die Tasten lassen sich unterscheiden — im Feld `id` **[M][E]**

Früher stand hier, alle Drücke sähen gleich aus. Das war falsch, und zwar
weil am falschen Feld geschaut wurde: `value1` ist **die Bildschirmnummer**
und deshalb konstant 9, nicht die Taste. Die Taste steckt in **`id`**.

Ein Lauf mit allen drei Tasten liefert `id` = 0, 1 und 3 — im Muster
`3, 0, 1, 3, 0, 1` im Halbsekundentakt, also je zweimal der Reihe nach. Ein
zweiter Lauf, in dem **nur** der grosse grüne Knopf gedrückt wurde, lieferte
dreimal `id=1` und nichts anderes:

```
[ 46.290] EP88  20 Button Press  id=1 value1=9
[ 48.530] EP88  20 Button Press  id=1 value1=9
[ 50.194] EP88  20 Button Press  id=1 value1=9
```

Also **`id=1` ist der grosse grüne Knopf**. Welche der beiden Pfeiltasten 0
und welche 3 ist, ist nicht getrennt gemessen **[?]**. Dass die Nummern 0, 1
und 3 sind und nicht 0, 1, 2, ist unerklärt.

Das Backend gab früher `value1` als `--function` heraus, also die
Displayzahl. Seit Abschnitt 16 liefert es `id`, und der Kern führt die Drücke
als Warteschlange (`i2600_next_button`).

### Tasten wecken das Gerät nicht, melden aber **[E]**

Im Energiesparmodus (Abschnitt 15) kommen Tastendrücke weiter als Ereignis
0x20 an, und das Gerät **bleibt dabei schlafen**: Statusbyte 0x0c steht
hinterher unverändert auf 1, und der hinterlegte Paneltext bleibt stehen. Das
ist die Grundlage für „Knopf drücken, Scan startet": ein Dienst hält das
Gerät offen, schickt es schlafen, wartet auf 0x20 mit `id=1` und weckt es
dann mit `0x39` wValue 2. Gebaut ist das als `i2600-scand`, Abschnitt 16 —
mit der dort gemessenen Falle, dass das Aufwachen die Scankonfiguration
**nicht** wiederbringt.

Voraussetzung ist, dass die Ereignismeldung **scharf** ist, und das macht die
Öffnungssequenz des Kerns. Ein Minimal-Mithörer, der nur die Schnittstelle
belegt und EP 0x88 liest, empfängt **nichts** — weder wach noch schlafend.
Das war in der ersten Messung die Fehlerquelle und hätte beinahe zum
falschen Schluss geführt, im Sparmodus kämen keine Tasten an.

---

## 14. Das Bedienfeld ist ein Grafikdisplay **[C][M]**

Das Display des i2600 ist **kein Textdisplay**, sondern eine Bitmap-Anzeige
von **128 × 48 Pixeln**. Der Herstellertreiber rendert seine Wörter mit
Pango und Cairo und lädt das Ergebnis hoch — `OCP::PopulateMessage(char
const*, u8, u8)` legt eine Cairo-Fläche `0x80 × 0x30` an, zeichnet den Text
mit Pango hinein und baut daraus einen 0x300 Byte großen Puffer. **[C]**

### Das Format **[C][M]**

```c
OCP::LcdPopulate(bitmap, a, b)
    -> DeviceSettings::Set(0x62, wValue = (b << 8) | a, wIndex = 0x680,
                           768 Byte)
```

Die Bitmap liegt im üblichen Grafik-LCD-Layout: **sechs Seiten zu je acht
übereinanderliegenden Pixelzeilen, ein Byte je Spalte innerhalb einer Seite,
Bit 0 oben**. 6 × 128 = 768 Byte.

Belegt ist das nicht durch Lesen des Codes allein, sondern durch Dekodieren
der **Bitmap des Herstellers aus dem Mitschnitt**: mit diesem Layout kommt
lesbarer Text heraus — „Rescan Documents". **[M]**

### Werkzeuge

* [tools/lcd-text.py](../tools/lcd-text.py) rendert eine oder zwei Zeilen
  Text mit PIL in genau dieses Format und zeigt eine ASCII-Vorschau.
* [native/i2600-lcd.c](i2600-lcd.c) lädt eine solche Datei hoch.

### Der Zielwert ist `(Bildschirm << 8) | Region` **[M][E]**

Der Upload adressiert einen **Bildschirm innerhalb einer Region**:

```
wValue = (Bildschirm << 8) | Region        Region 2, 3 oder 4
wIndex = 0x0680
768 Byte Bitmap, 128x48, 6 Seiten a 128 Spalten, Bit 0 oben
```

Also `0x0904` für Region 4 Bildschirm 9, `0x0102` für Region 2 Bildschirm 1.
In den Mitschnitten kommt nur Region 4 vor; dass es die anderen gibt, steht
in der Meldungstabelle und ist am Gerät bestätigt.

**Der Irrweg, der das lange verdeckt hat:** Die `Screen Missing`-Ereignisse
tragen `value1 = 0x0100 | N` — bei Bildschirm 8 also `0x0108`. Das sieht dem
Upload-Zielwert zum Verwechseln ähnlich, ist aber eine andere Kodierung. Der
Versuch, nach `0x0109` zu schreiben, stallte folgerichtig, und daraus wurde
der falsche Schluss gezogen, Bildschirm 9 sei nicht beschreibbar. Tatsächlich
hätte es `0x0904` heissen müssen.

Sichtbar wurde es erst in einem Mitschnitt des Herstellertreibers **nach
einem echten Kaltstart**: dort erscheint neben dem bekannten `0x0104` zum
ersten Mal ein `0x0904`. In den früheren Mitschnitten fehlte es, weil die
Bildschirme im Gerät schon belegt waren und der Treiber sie nicht neu schrieb.

### Die Meldungstabelle steht im Herstellertreiber **[C]**

`devicemanager.so` trägt eine eingebettete Liste mit Einträgen der Form
`<a>message_<Region>_<Bildschirm></a><c>Text</c>` — die Namen sind genau die
Koordinaten des Uploads:

| Region | Bildschirm | Text                               |
|--------|------------|------------------------------------|
| 2      | 0          | `Check USB Cable`                  |
| 2      | 1          | `Scanning...`                      |
| 2      | 2          | `Clear paper path and close cover` |
| 2      | 3          | `Power saver mode`                 |
| 3      | 1          | `Bad FPGA`                         |
| 3      | 2          | `Bad SRAM`                         |
| 3      | 3          | `Turn power off then on`           |
| 4      | 0          | `Unused`                           |
| 4      | 1          | `Rescan documents`                 |
| 4      | 2          | `Turn power off then on`           |
| 4      | 3          | `Rescan documents`                 |
| 4      | 4          | `Turn power off then on`           |
| 4      | 5          | `Rescan documents`                 |
| 4      | 6          | `Clear paper path and close cover` |
| 4      | 7          | `Turn power off then on`           |
| 4      | 8          | `Multifeed`                        |
| 4      | 9          | `Paper jam`                        |

Region 2 sind Betriebszustände, Region 3 Hardwarefehler, Region 4 die
Handlungsanweisung zum jeweiligen Zustand. Die Tabelle lässt sich gegen das
Gerät prüfen: die Bitmaps, die der Treiber in den Mitschnitten hochlädt,
ergeben zurückgelesen `Rescan documents` für Region 4 Bildschirm 1 und
`Paper jam` für Region 4 Bildschirm 9 — wortgleich (`tools/lcd-decode.py`).

Dass **in den Mitschnitten** nur Region 4 vorkommt, heisst also nicht, dass
das Gerät nur diese kennt: `Scanning...` liegt in Region 2 und wird bei jedem
Durchlauf gezeigt, ohne dass der Treiber es je neu schreiben müsste.

### Der Upload speichert, er zeigt nicht **[E]**

Das Gerät entscheidet, was auf dem Panel steht. Ein Upload füllt nur den
Speicher dahinter:

- Eine vollflächig gesetzte Bitmap (768 Byte `0xff`), 50-mal über 15 s auf
  Region 4 Bildschirm 9 geschrieben, alle Uploads erfolgreich: am Panel
  **keine sichtbare Änderung**, weil das Gerät im Ruhezustand war.
- `Bereit` auf demselben Weg war **nur während eines anliegenden Papierstaus**
  zu sehen — da zeigte das Gerät Region 4 Bildschirm 9, dessen `Paper jam` wir
  überschrieben hatten. Mit dem behobenen Stau verschwand die Meldung.
- Umgekehrt der Gegenbeweis, wiederholbar ohne Stau: eigener Text nach Region
  2 Bildschirm 1 (`wValue 0x0102`), dann drei Durchläufe des Transports. In
  **drei von drei** Durchläufen stand am Panel unser Wort statt `Scanning...`.
  Das ist der saubere Nachweis, weil dieser Zustand in jedem Scan eintritt.

```bash
tools/lcd-text.py /tmp/txt.bin "Es laeuft"
native/i2600-lcd /tmp/txt.bin 1 2     # Bildschirm 1, Region 2
```

### `Screen Missing` ist eine Anforderung, kein Fehler **[M]**

Das Ereignis (EP 0x88, Typ 0x60) trägt `value1 = 0x0100 | Bildschirm`. Das
Gerät verlangt damit eine Bitmap, die es nicht hat — der Host rendert sie und
lädt sie hoch. Deshalb bringt der Herstellertreiber Pango und Cairo mit: im
Gerät stecken keine Schriften.

**Es ist aber keine Ankündigung, dass der Bildschirm gleich gezeigt wird.**
Gemessen: nach einem Durchlauf mit leerem Einzug fordert das Gerät Bildschirm
9 an; die Anforderung innerhalb von 50 ms mit eigenem Text zu beantworten
ändert am Panel nichts. Gezeichnet wird ein Bildschirm nur, solange das Gerät
in dem zugehörigen Zustand ist.

Und Vorsicht: dieselbe Zahl in zwei Kodierungen. Die Anforderung meldet
`0x0100 | Bildschirm`, der Upload adressiert `(Bildschirm << 8) | Region`.

### Zurücklesen geht nicht **[M]**

Anfrage 0x62 existiert auch als IN (`i2600_ocp_read`), aber alle 32 Versuche
über `wValue = (N << 8) | 4`, `wIndex 0x0680` scheitern. Der Panelspeicher ist
schreibbar, nicht lesbar; die Tabelle oben ist daher die einzige Quelle für
die Zuordnung.

### Die erste Reihe belegt das Gerät nur in Region 4 **[M][E]**

In Region 4 setzt das Gerät links oben Statusbuchstaben und Bildschirmnummer
selbst — deshalb `U9 Bereit`, und deshalb lassen die Hersteller-Bitmaps dieser
Region die Zeilen 0–4 leer. Beeinflussbar ist davon nur die Ziffer, über die
Bildschirmauswahl (`SetButtonSequenceNumber`, Request 0x16) und nur als
**eine Stelle 1–9** — kein freier Text. Nach dem Einschalten steht dort 0,
nach dem Aufwärmen die vom Treiber gewählte 9.

In Region 2 ist sie **nicht** belegt: während unser Text dort stand, war die
Ziffer verschwunden. Dort gehören uns also alle 128x48 Pixel, und eine Bitmap
darf die obersten Zeilen mitbenutzen.

Der **Fortschrittsbalken beim Firmware-Laden** stammt ebenfalls vom Gerät:
im Kaltstart-Mitschnitt stehen in dieser Phase keine Panel-Transfers.

### Was damit möglich ist

Die Meldungen lassen sich umformulieren oder eindeutschen, und zwar wirksam
und wiederholbar — `Scanning...` durch eigenen Text zu ersetzen klappt in
jedem Durchlauf. Für eigene Anzeigen nach Belieben (Seitenzähler,
Fortschritt, Symbole) taugt der Weg nicht: wir wählen den Text, nicht den
Zeitpunkt.

### Die deutsche Fassung **[C]**

`tools/messages-de.tsv` hält die Übersetzung aller 16 benutzten Meldungen
(Region 4 Bildschirm 0 ist beim Hersteller `Unused` und bleibt weg), je mit
Region, Bildschirm, Freiraum oben und einer oder zwei Zeilen. Alle 16 setzen
in 128x48, geprüft mit `tools/lcd-text.py`.

Region 4 braucht oben sechs freie Zeilen, weil das Gerät dort selbst zeichnet
— dafür hat `lcd-text.py` jetzt `--reserve-top`. Region 2 braucht nichts frei
zu lassen.

```bash
tools/lcd-text.py --reserve-top 6 /tmp/stau.bin "Papierstau"
```

### Offen **[?]**

Ob ein gespeicherter Bildschirm ein `i2600_device_close()` oder einen
Netzschalter übersteht, ist nicht geprüft — im Dreifachtest wurde zwischen den
Durchläufen neu geschrieben. Und woher die Texte stammen, die das Gerät von
sich aus kennt, ist offen: Region 2 haben wir nie hochgeladen, `Scanning...`
erscheint trotzdem. Naheliegend ist die Firmware, die wir beim Kaltstart aus
den Herstellerimages selbst einspielen — dann wäre `Screen Missing` genau die
Nachfrage nach dem, was dort fehlt.

## 15. Zustände und Operationen **[C]**

### Die Ereignisse auf EP 0x88 **[C][M]**

`devicemanager.so` führt die Namen der Ereignistypen als Tabelle. In der
Reihenfolge der Tabelle, mit den Nummern, die wir am Draht gemessen haben,
als Anker:

```
0x01 End of Operation     0x10 Power State       0x20 Button Press
0x02 Start of Operation    0x11 Buffer State      0x21 Paper Jam
0x03 Ding                  0x12 Transport State   0x22 Multifeed
                           0x13 Tray State        0x23 Buffer Overflow
                           0x14 Udds State        0x24 Print Rollover
                           0x15 Lamp State        0x25 Other Error
                           0x16 Interlock State   0x26 Notification Offset
                           0x17 Carriage State    0x27 Page Exit
                           0x18 Scanner State     0x28 Imaging Complete
                           0x19 Elevator State    0x29 Mini Cal Paused
                           0x1a S0 Sensor State   0x2a Setup Pause
                                                  0x2b Patch Detected
                           0x60 Screen Missing    0x2c Patch Pause
                                                  0x2d DSP Reply
                                                  0x2e Trace Log
```

**Gemessen** sind 0x01, 0x02, 0x03, 0x10, 0x12, 0x15, 0x20 und 0x60; die
übrigen Nummern sind aus der Reihenfolge der Namenstabelle zwischen diesen
Ankern erschlossen und nicht einzeln belegt **[?]**. Es gibt also nicht einen
Gerätezustand, sondern mehrere nebenläufige — Power, Transport, Lampe,
Wagen, Elevator und weitere, jeder mit eigenen Meldungen.

### ScannerOperation: 27 Operationen **[C]**

`DeviceSettings::ScannerOperation(_ScannerOp)` ist eine Sprungtabelle über die
Operationen 0x00–0x1a. Jeder Pfad setzt nur bRequest und wValue und ruft dann
`DeviceSettings::Set(bRequest, wValue, wIndex=0, len, data)`:

| op | bRequest | wValue | op | bRequest | wValue |
|----|----------|--------|----|----------|--------|
| 0x00 | 0x10 | 1 | 0x0e | 0x1c | 1 |
| 0x01 | 0x10 | 0 | 0x0f | 0x1c | 2 |
| 0x02 | 0x10 | 0 | 0x10 | 0x1c | 3 |
| 0x03 | 0x10 | 0 | 0x11 | 0x20 | 1 |
| 0x04 | 0x11 | 0 | 0x12 | 0x20 | 0 |
| 0x05 | 0x11 | 1 | 0x13 | 0x21 | 0 |
| 0x06 | 0x11 | 1 | 0x14 | ?    | ?    |
| 0x07 | 0x12 | 0 | 0x15 | 0x39 | 0 |
| 0x08 | 0x12 | 1 | 0x16 | 0x39 | 1 |
| 0x09 | 0x14 | 0 | 0x17 | 0x39 | 2 |
| 0x0a | 0x15 | 0 | 0x18 | 0xe3 | 256 |
| 0x0b | 0x15 | 1 | 0x19 | 0x19 | 0 |
| 0x0c | 0x1a | 0 | 0x1a | 0x19 | 1 |
| 0x0d | 0x1b | 0 |      |      |     |

Die Ableseregel ist gegengeprüft: Operation 0x0d ergibt bRequest 0x1b,
wValue 0 — genau der Transfer, den `gen-duplex.py` als Landmarke
`OP_SCANNER_OPERATION_0D` benutzt.

### Energiesparmodus **[C]**

`FCT::EnterEnergyStar()` setzt die Einstellung `Set(0x31, 0, 0, 1 Byte = 1)`
und ruft dann Operation 0x16, also **bRequest 0x39, wValue 1**.
`ExitEnergyStar()` ruft Operation 0x17, **bRequest 0x39, wValue 2**.
`GetEnergyStar()` liest die Einstellung mit `Get(0x31, ...)` zurück.

Das ist der einzige gefundene Zustand, der **dauerhaft** anliegt und eine
eigene Panel-Meldung hat (Region 2 Bildschirm 3, `Power saver mode`). Damit
ist eine stehende Anzeige möglich, anders als beim flüchtigen `Scanning...`.

**Am Gerät erprobt [E]:** Die Anfrage wird angenommen. Die Einstellung 0x31
stand dabei schon auf 1, es musste also nichts Dauerhaftes geschrieben
werden. Danach meldet der Statusblock Byte 0x0c den Wert **1**, wo nach einem
Scan **3** und während eines Scans **4** gemeldet wird, und die Sensoren
liefern Vorgabewerte (Abdeckung „offen", obwohl sie zu ist) — das Gerät ist
also tatsächlich herunter. Ein Power-State-Ereignis auf EP 0x88 kommt dabei
**nicht**.

`Scanner::GetPowerState()` liest genau dieses Byte 0x0c aus dem Statusblock.
Abfragen und Schalten geht ohne die Öffnungssequenz des Kerns, die mit 271
Transfers ein schlafendes Gerät wecken würde:

```bash
native/i2600-power              # Zustand lesen
native/i2600-power saver        # 0x39 wValue 1
native/i2600-power on           # 0x39 wValue 2
```

Wichtig für solche Versuche: `i2600_device_close()` fährt die
Teardown-Sequenz, die das Gerät sehr wahrscheinlich wieder aufweckt. Wer es
in einem Zustand hinterlassen will, verlässt den Prozess ohne Teardown.

Daneben gibt es Zeitschalter: `SetLowPowerTimeout(unsigned char, unsigned
char)`, `GetLowPowerTimeout()`, `GetPowerOffTimeout()` sowie die
Konfigurationsschlüssel `lowpowermodetimeout` und `powerofftimeout`.

---

## 16. Knopfdruck zu paperless: `i2600-scand` **[C][M][E]**

Der grosse grüne Knopf löst einen Scan aus, das Gerät wartet dazwischen im
Energiesparmodus mit eigenem Text im Display, und die Seiten landen als ein
PDF je Stapel in einem Ordner, den paperless-ngx beobachtet. Am Gerät
erprobt, vollständiger Kreis.

### Warum ein eigener Dienst und nicht `scanbd` **[C]**

Die naheliegende Lösung wäre `scanbd` mit unserem SANE-Backend — der Sensor
`--scan` trägt `SANE_CAP_HARD_SELECT`, genau das Merkmal, auf das `scanbd`
achtet. Der Quelltext von `scanbd` 1.5.1 (`src/scanbd/sane.c`) entscheidet es
gegen diesen Weg:

* **Das Offenhalten leistet `scanbd` durchaus.** Der Pollthread ruft einmal
  `sane_open()` (Zeile 635) und fragt die Optionen dann in einer Schleife mit
  `usleep(timeout)` ab (Zeile 1180). Die Ereignismeldung wäre also scharf.
* **Aber um das Aktionsskript herum schliesst und öffnet es.** Beim Auslösen
  ruft es `sane_close()` (Zeile 1024), startet das Skript, und danach
  bedingungslos wieder `sane_open()` (Zeile 1158). Dieses erneute Öffnen
  fährt unsere 271 Transfer lange Öffnungssequenz, die das Gerät weckt — und
  **danach gibt es keinen Haken mehr**, an dem man es wieder schlafen legen
  könnte. Das Gerät stünde zwischen den Scans wach.
* Für den Paneltext hat `scanbd` ohnehin keinen Platz.

Dazu kommt der Aufwand: `scanbd` ist in den Manjaro-Paketquellen nicht
enthalten (AUR, braucht `confuse`), will eine dbus-Richtlinie, eigene
systemd-Units und zwei getrennte `sane.d`-Bäume. Für ein Gerät mit einer
Taste steht das in keinem Verhältnis.

Das SANE-Backend behält den Sensor trotzdem — für Oberflächen, die ihn
abfragen wollen. Nur der unbeaufsichtigte Weg geht an `scanbd` vorbei.

### Der Dienst

[native/i2600-scand.c](i2600-scand.c), gebaut auf `i2600-core`:

```
öffnen (einmal, und offen halten -- sonst meldet keine Taste)
  Paneltext hinterlegen (Region 2 Bildschirm 3 und 1)
  Lampe aus, Sparmodus ein
  warten auf Ereignis 0x20 mit id=1
    aufwecken (0x39 wValue 2)
    prepare-Phase nachziehen        <- ohne das kommt nur Schwärze
    scannen bis der Einzug leer ist
    PDF schreiben, umbenennen
    Paneltext auffrischen, Lampe aus, Sparmodus ein
```

```bash
native/i2600-scand -o ~/paperless-consume      # Dauerbetrieb
native/i2600-scand --once -v                   # ein Stapel, mit Protokoll
```

Ablage ist `--out`, sonst `$I2600_CONSUME_DIR`, sonst
`$HOME/paperless-consume`. Geschrieben wird nach `NAME.pdf.part` und erst am
Schluss auf `NAME.pdf` umbenannt — paperless beobachtet den Ordner und darf
keine halbe Datei zu fassen bekommen; `.part` konsumiert es nicht, und das
Umbenennen innerhalb eines Verzeichnisses ist atomar.

Die Seiten gehen als JPEG mit `/DCTDecode` ins PDF
([native/i2600-pdf.c](i2600-pdf.c)): kein zlib-Gerüst nötig, und die Dateien
sind etwa ein Zehntel so gross wie roh. 300 dpi werden zur Seitengrösse in
Punkt, A4 kommt als 586 x 848 pt heraus. Das kostet eine Baubedingung mehr,
`libjpeg`, und nur `i2600-scand` braucht sie.

`native/i2600-scand.service` ist eine systemd-User-Unit; der Gerätezugriff
braucht weder root noch eigene udev-Regel (Abschnitt 3).

### Der Sparmodus kostet die Scankonfiguration **[M][E]**

**Das war die teuerste Falle.** Nach 30 Minuten im Sparmodus startete der
Scandurchgang anstandslos, der Transport zog alle fünf Blätter durch, für
jedes kam ein `Page Exit` und ein formal einwandfreier Block mit gültigem
Footer — mit **76 Zeilen reiner Schwärze** statt rund 3500 Zeilen Bild:

```
[18:31:56]   ep 82: block of 196604 bytes in 12 transfers
[18:31:56]   page 2580 x 76, 1 byte/pixel
```

Das Aufwachen stellt die Kamera- und FPGA-Einstellungen der `prepare`-Phase
**nicht** wieder her, und die Lampe ist aus. `i2600_device_prepare()` fährt
die Phase ohne Schliessen noch einmal (171 Transfer, rund eine Sekunde);
danach kamen volle Seiten:

```
[18:34:59]   page 2444 x 3507 pixels, 1 byte/pixel
[18:35:00]   page 2456 x 3522 pixels, 1 byte/pixel
```

Das reiht sich in Abschnitt 5 ein: *ohne Lampe ein formal einwandfreier Block
reiner Schwärze*. Wer eine Sequenz kürzt oder einen Zustandswechsel einbaut,
muss den **Bildinhalt** prüfen, nicht die Blockgrösse.

### Die Lampe: nur wach, und nicht während des Aufwärmens **[C][M][E]**

Im Sparmodus brennt die Lampe weiter. Das ist kein Fehler unserer Seite:
`FCT::EnterEnergyStar()` ruft ausweislich des Disassemblats nur
`GetEnergyStar()`, `SetEnergyStar(1)` und Operation 0x16 — **die Lampe rührt
der Herstellertreiber dabei nicht an**. `DeviceSettings::TurnLampOff()` ist
Operation 0x04, also `bRequest 0x11 wValue 0`, derselbe Transfer, den die
Teardown-Sequenz fährt.

Zwei Bedingungen, beide am Gerät gemessen:

1. **Nur im Wachzustand.** An das schlafende Gerät geschickt wird `0x11
   wValue=0` zwar angenommen (kein Stall, Byte 0x0c bleibt 1), bleibt aber
   wirkungslos. Dasselbe Gerät geweckt und derselbe Transfer: Lampe aus.
2. **Nicht während der Aufwärmphase.** Unmittelbar nach `prepare`, das die
   Lampe einschaltet, verpufft das Ausschalten ebenfalls. Die Firmware fährt
   eine eigene Aufwärmzeit — daher `GetLampWarmupTime(ELAMP_WARMUP_TYPE,
   unsigned short)` und `Scanner::IsOkToStartScan(ELAMP_WARMUP_TYPE, bool)`.
   Der Dienst wartet 1,5 s und fasst nach.

Dass der Sparmodus die Lampe wieder anschaltet, war die naheliegende Vermutung
und ist **falsch**: mit vorher gelöschter Lampe eingeschläfert, blieb sie aus.

### Der Paneltext hängt am Zustand, nicht am Dienst **[E]**

`Bereit` liegt auf **Region 2 Bildschirm 3**, beim Hersteller `Power saver
mode`. Gezeichnet wird dieser Bildschirm nur, solange das Gerät schläft —
der Text und der Sparmodus sind dasselbe. Wach zeigt das Gerät seinen
eigenen Bildschirm, und daran ändert ein Upload nichts (Abschnitt 14).

Am Gerät bestätigter Ablauf eines Drucks: `Bereit` → `Scannt` (unser Text auf
Region 2 Bildschirm 1, der `Scanning...` ersetzt) → `Bereit`, und wenige
Sekunden später geht die Lampe aus.

Die Bitmaps sind einkompiliert: [native/gen-panel.py](gen-panel.py) rendert
sie mit demselben Code wie `tools/lcd-text.py` nach `panel-bitmaps.h`, damit
der Dienst ohne Datei neben sich auskommt. `--panel DATEI` ersetzt den
Wartetext, `--no-panel` lässt das Bedienfeld in Ruhe.

Auf ein `Screen Missing` (Typ 0x60) hin lädt der Dienst die Bildschirme neu —
das ist der einzige Moment, in dem ein Upload angefordert ist.

### Die Tasten im Kern

Der Kern führt Tastendrücke jetzt als **Warteschlange** statt als Zähler:
`i2600_next_button(&id, &value1)` nimmt den ältesten Druck ab. Nötig, weil
`id` die Taste benennt und ein Dienst, der nur den grünen Knopf will, die `id`
eines Drucks nicht an den nächsten verlieren darf.

Damit liefert das SANE-Backend bei `--function` endlich **`id`** statt der
Displayzahl — der offene Punkt aus Abschnitt 13 ist erledigt:

```
--scan[=(yes|no)] [no] [hardware]   wahr einmal nach einem Tastendruck
--function <int> [0] [read-only]    1 = grüner Knopf, 0 und 3 die Pfeile
```

### Der Netzschalter: der Dienst muss sich beenden **[C][E]**

Für den unbeaufsichtigten Betrieb ist das der wichtigste Punkt, und er wurde
beinahe übersehen. Wird der Scanner am Netzschalter ausgeschaltet — die
übliche Kur gegen einen Papierstau —, verschwindet er vom Bus. Der libusb-
Handle bleibt dabei formal gültig, jeder Transfer antwortet mit
`LIBUSB_ERROR_NO_DEVICE`, und der Interrupt-Leser lief in der ersten Fassung
stumm weiter. Ein Dienst, der nur pollt, **dreht dann ewig auf einem toten
Gerät**, und `Restart=always` greift nicht, weil der Prozess gar nicht endet.

Dazu kommt: Einschalten enumeriert ein **neues** USB-Gerät. Es gibt also
nichts, womit sich der alte Handle wiederbeleben liesse — der einzige Weg ist
von vorn.

Deshalb setzt der Kern jetzt eine Marke, sobald ein Transfer das Gerät
vermisst (`i2600_device_gone()`, gesetzt in `ctrl_in`, `ctrl_out` und im
Interrupt-Leser, der es am schnellsten merkt und seine Schleife verlässt).
Der Dienst bricht daraufhin ab — **ohne Teardown**, der nur weitere
Fehltransfers aufhäufen würde — und überlässt den Rest systemd. Der neue
Prozess findet ein kaltes Gerät vor und spielt die Firmware ein wie bei jedem
Kaltstart (Abschnitt 2). Genau das macht den Netzschalter unkritisch.

In der Unit gehört dazu `StartLimitIntervalSec=0`: solange der Scanner aus
ist, endet der Dienst sofort, und systemd würde ihn sonst nach wenigen
Versuchen endgültig aufgeben.

Ein halb geschriebenes PDF, dessen Gerät mitten im Stapel verschwindet, wird
verworfen — die `.part`-Datei bleibt nicht liegen.

### Wiedererkennen ohne udev-Regel: libusb-Hotplug **[M][E]**

Mit `Restart=always` allein vergeht bis zu ein `RestartSec`, ehe der Dienst
den wieder eingeschalteten Scanner bemerkt. Eine udev-Regel wäre der
naheliegende Weg, ist aber für eine **User**-Unit umständlich — udev läuft im
System-Manager, und die Brücke dorthin (`TAG+="systemd"` plus
`ENV{SYSTEMD_USER_WANTS}`) hängt an einer angemeldeten Sitzung.

Nötig ist sie nicht: libusb bringt Hotplug selbst mit, und wir binden libusb
ohnehin. `i2600_wait_for_device(timeout_s)` meldet sich für
`LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED` auf unsere VID/PID an und schläft in
`libusb_handle_events_timeout_completed()`. Gemessen: **10 ms CPU über 10 s
Wartezeit** — es blockiert wirklich, es pollt nicht.

Ein Detail entscheidet darüber, ob das trägt: **das Ereignis allein genügt
nicht.** Hotplug feuert aus libusbs eigenem udev-Monitor und kann der
`uaccess`-Regel zuvorkommen, die die ACL auf den Geräteknoten setzt
(Abschnitt 3) — dann ist das Gerät da und trotzdem nicht zu öffnen. Der
Beweis ist deshalb nicht das Ereignis, sondern ein **geglückter Open**, und
genau den wiederholt die Schleife; das Ereignis weckt sie nur. Ohne
Hotplug-Fähigkeit fällt sie auf 300-ms-Polling zurück.

Damit wartet der Dienst, statt zu enden, solange der Scanner aus ist, und
`RestartSec` deckt nur noch den Absturzfall ab (jetzt 2 s).

Den Netzschalter quittiert der Dienst mit Rückgabewert **75** (`EX_TEMPFAIL`)
statt 1, und die Unit zählt ihn über `SuccessExitStatus=75` als normales
Ende. Sonst stünde nach jedem gelösten Papierstau ein `Failed with result
'exit-code'` im Journal — headless liest sich das wie ein Defekt. Ein
echter Fehler bleibt dabei ein Fehler.

### Leerseiten: der Rand ist das ganze Problem **[M][E]**

Im Farb-Duplex ist jede zweite Seite die leere Rückseite eines einseitig
bedruckten Blattes. **paperless-ngx kann das nicht** — dort steht seit Jahren
ein Feature-Wunsch, und die Umgehung sind Vorschaltskripte, die mit
Ghostscript die Schwärzung messen und mit qpdf Seiten herausschneiden. Im
Dienst ist es besser aufgehoben: dort liegen die Rohpixel vor der
JPEG-Kompression, und eine verworfene Seite kostet weiter unten gar nichts.

Gemessen wird der Anteil Pixel, die deutlich dunkler sind als das Weiss des
Blattes selbst — Weisspunkt ist der **Modus** der hellen Histogrammhälfte,
damit ein vergilbtes Blatt an sich selbst gemessen wird, und eine Seite mit
dunklem Mittelwert gilt nie als leer, damit kein Foto wegfliegt.

**Der erste Anlauf hat nichts verworfen, und das war lehrreich.** Die leere
Rückseite kam auf **0,344 %** — weit über jeder brauchbaren Schwelle. Der
Verdacht fiel auf Sensorrauschen, war aber falsch: tiefer schneiden änderte
nichts (0,373 % bei −48, 0,344 % bei −80). Es waren also **wirklich dunkle
Pixel**. 26 000 Pixel bei 2449 × 3540 ist ziemlich genau ein zwei Pixel
breiter Rahmen — der Rest des Transportrands, den der Autobeschnitt
stehenlässt.

Mit **1 % Rand ausgespart** sieht dieselbe Messreihe so aus:

| Seite | ohne Rand | mit 1 % Rand |
|-------|-----------|--------------|
| 1 (Foto, dunkel) | 71,7 % | Mittelwert-Sperre |
| 2 | 29,0 % | 30,2 % |
| 3 | 21,4 % | 22,4 % |
| 4 | 6,7 % | 8,6 % |
| 5 (hell) | 4,0 % | 4,8 % |
| 6 (leere Rückseite) | **0,344 %** | **0,0006 %** |

Aus Faktor 12 wird Faktor 10 000. Am Gerät bestätigt: derselbe Stapel noch
einmal, `blank page dropped (0.001 % ink, limit 0.050 %)`, fünf Seiten statt
sechs, und die helle Seite 5 ist drin.

Die Schwelle bleibt trotzdem zaghaft bei **0,05 %**: eine Zeile 12-pt-Text
auf A4 sind schon rund 0,17 %, vier gelochte Löcher ähnlich viel. Eine echte
Seite zu verlieren ist deutlich schlimmer als eine leere zu behalten.
`--keep-blank` schaltet es ab, `--blank-ink` verstellt die Schwelle, und
jede verworfene Seite steht mit ihrem Messwert im Protokoll.

### Farbe und Duplex

Beides sind nur Schalter (`--color --duplex`), weil die Sequenzen dafür schon
stehen (Abschnitt 11). Die Unit setzt sie über `I2600_SCAND_OPTS`, damit ein
`systemctl --user edit`-Override sie ändern kann, ohne die Unit anzufassen.
Ein Farb-Duplex-Stapel kostet etwa 1,4 MB je Seite im PDF.

**Falle in der Unit:** `Environment=` trennt den Wert an Leerzeichen und liest
jedes Stück als eigene Zuweisung. `Environment=I2600_SCAND_OPTS=--color
--duplex` setzt also nur `--color`, und `--duplex` fällt **stillschweigend**
weg — am Gerät daran aufgefallen, dass farbig gescannt wurde, aber einseitig.
Der Wert muss in Anführungszeichen:

```
Environment="I2600_SCAND_OPTS=--color --duplex"
```

Ebenso frisst ein `%` in `Documentation=` den Rest: der Pfad mit `%20` darin
ist für systemd ein Spezifizierer und wird abgelehnt.

### Der Beschnitt schnitt im Schattensaum **[M][E]**

`i2600_autocrop` nimmt die erste und letzte Zeile beziehungsweise Spalte,
deren Mittelwert ein **Viertel** des Papierweiss übersteigt. Am Übergang vom
Transportrahmen zum Blatt liegt dort aber noch der Schattensaum. Gemessen am
Rand einer fertigen A4-Seite:

```
oben:    87, 206, 251, 253, ...        -- ein bis zwei Zeilen
links:   81, 121, 151, 172, 186, 192, 199, 218, 245, 254
rechts:  71,  99, 113, 121, 137, 164, 189, 210, 225, 246
```

Senkrecht reichen zwei Zeilen, waagerecht zieht sich der Verlauf über **rund
zehn Spalten** — und die Schwelle von 60 ist schon in der allerersten Spalte
überschritten, der Beschnitt trimmt dort also gar nicht. Das ist derselbe
Rand, der die Leerseitenmessung verdorben hat.

Der Beschnitt läuft jetzt nach: von jeder Kante nach innen, solange diese
Zeile oder Spalte dunkler als **90 % des Papierweiss** ist. Das passt sich
an, wie breit der Saum gerade ist, und ist auf 1 % der Kantenlänge begrenzt,
damit eine Seite mit randlos dunklem Inhalt höchstens 2 mm verliert statt
von der Seite aufgefressen zu werden. Danach:

```
oben:    241, 253, 254, ...
links:   232, 253, 255, ...
rechts:  221, 234, 242, 246, 250, ...
```

Rechts bleibt ein sehr heller Verlauf über etwa fünf Spalten stehen, weil die
Schwelle dort gerade greift — 0,4 mm blasser Grauton, unterhalb dessen, was
sich zu holen lohnt. Die Änderung sitzt im Kern, kommt also `i2600-grab` und
dem SANE-Backend genauso zugute.

### Offen **[?]**

* Nach dem letzten Blatt meldet `status[0x0f]` häufig **2** („Papier da"),
  obwohl der Einzug leer ist; der Kern deutet das als `I2600_STOP_STALLED`.
  In einem Lauf mit fünf Blättern kam dagegen sauber die 1. Der Stapel ist
  dabei vollständig gescannt — die Meldung ist also irreführend, nicht der
  Scan. Woran der Unterschied hängt, ist nicht gemessen.
* Ob die 76 Zeilen eine feste Vorgabe der Firmware sind oder von der
  Schlafdauer abhängen, ist nicht untersucht; `i2600_device_prepare()` räumt
  es ohnehin aus.
* Ob ein kürzerer Weg als die ganze `prepare`-Phase genügt (nur die
  Kameraseiten und die Lampe), ist nicht ausgemessen — eine Sekunde ist
  billig genug, dass es sich nicht lohnte.

---

## 17. Senkrechte Streifen: die Kalibriertabellen lagen brach **[C][M][E]**

Nach dem Reinigen des Scanners traten in den Bildern haarfeine senkrechte
Linien über die volle Seitenhöhe auf — die klassische Signatur fehlender
Korrektur je Sensorpixel. Naheliegender Verdacht war unser Treiber, denn wir
laden nur `gains`: **3 Byte analoge Gesamtverstärkung je Kamera**
(Abschnitt 5), keine Tabelle über die 2580 Spalten.

Die erste Gegenmessung schien den Treiber zu entlasten. Dasselbe Blatt durch
den Herstellertreiber (`scanimage -d kds_i2000:i2000 --mode Gray --resolution
300 --source "ADF Duplex"`), weisse Rückseite, Innenbereich vermessen:

```
Mittel 252.5   Spanne 98.7..255.0
mittlere Abweichung 4.44   Nachbarsprung 2.67
Spalten mehr als 12 unter dem Mittel: 103
```

103 schmale dunkle Spalten, die dunkelste bei 98.7 von 255 — der
Herstellertreiber zeigte die Streifen genauso.

**Dieser Schluss war falsch, und zwar auf lehrreiche Weise.** Es ist doch der
Treiber — nur nicht beim Scannen, sondern bei einem Schritt davor, den wir
nie gefahren haben. Und schlimmer: unser Treiber bringt auch den
Herstellertreiber dazu, diesen Schritt zu überspringen. Die Gegenmessung hat
also nicht den Hersteller gemessen, sondern unsere eigene Spur.

### Wo die Korrekturtabellen liegen **[C][M]**

Sie liegen **im Gerät**, in dessen SPI-Flash, und zwar in 256 KiB bei
**0x00000..0x3FFFF**. Belegt ist nicht alles; das Blockbild aus dem
ausgelesenen Bereich (2-KiB-Blöcke, `ff`-Blöcke sind unbeschrieben) **[M]**:

```
  0x00000..0x02fff   6 Blöcke        0x20000..0x217ff   3 Blöcke
  0x04000..0x06fff   6 Blöcke        0x24000..0x257ff   3 Blöcke
  0x08000..0x0afff   6 Blöcke        0x28000..0x297ff   3 Blöcke
  0x10000..0x12fff   6 Blöcke        0x2c000..0x2d7ff   3 Blöcke
  0x14000..0x16fff   6 Blöcke        0x30000..0x317ff   3 Blöcke
  0x18000..0x1afff   6 Blöcke        0x34000..0x357ff   3 Blöcke
                                     0x38000..0x397ff   3 Blöcke
                                     0x3c000..0x3d7ff   3 Blöcke
  0x3f800..0x3ffff   Kopfblock
```

Die 6-Block-Bereiche enthalten Paare aus zwei u16 je Pixel, die über die
Spalten langsam wandern — ein Beispiel von 0x00000:

```
73 7a 00 01   70 7a 00 01   6d 7a 00 01   6a 7a 00 01
```

also u16 0x7a73, 0x7a70, 0x7a6d … und dazu konstant 0x0100. Gelesen als
Verstärkung und Offset je Sensorpixel: 2580 Pixel × 4 Byte = 10320 Byte,
aufgerundet genau 6 Blöcke. Die 3-Block-Bereiche fassen entsprechend 2 Byte
je Pixel. Diese Zuordnung ist **erschlossen, nicht belegt** **[?]** — gemessen
sind nur Blockbild und Byteinhalt.

Im letzten Block liegt bei **0x3FC00** ein 230-Byte-Ergebniskopf
(`DmPiranhaImageCalResults`). Sein **erstes Byte 0xAA** ist die Auskunft
„dieses Gerät hat Bildkalibrierung" — `DeviceSettings::ScannerHasImageCal()`
prüft genau dieses Byte **[C]**. An unserem Gerät gemessen **[M]**:

```
  +000  aa 03 b5 00 85 00 c6 00 00 00 00 00 00 00 bc 00
  +010  8d 00 c9 00 00 00 00 00 00 00 a7 00 a7 00 00 00
```

Die Werkskalibrierung ist also vorhanden und war es die ganze Zeit.

### Angewendet wird aus dem SDRAM, nicht aus dem Flash **[C][M][E]**

Die FPGA normalisiert aus dem Arbeitsspeicher des Geräts. Das Kopieren
Flash → SDRAM ist ein **eigener Schritt, den der Host anstossen muss**:
`DeviceSettings::MoveFlashToSRAM()`. Transfer für Transfer, aus dem Code
gelesen und am Gerät nachgefahren:

| # | Richtung | Anfrage | wValue | wIndex | Daten | Bedeutung |
|---|----------|---------|--------|--------|-------|-----------|
| 1 | OUT | 0xa3 | 0xdc4d | 0 | `01` | SDRamDriverEnable |
| 2 | OUT | 0xa3 | 0xd800 | 0 | `08` | TargetCode = 8 |
| 3 | OUT | 0xa3 | 0xd908 | 0 | `2a` | NormalizationDataStart |
| 4 | IN  | 0xa3 | 0xd901 | 0 | → `24` | CommonGeneralControl lesen |
| 5 | OUT | 0xa3 | 0xd901 | 0 | `34` | dasselbe mit Bit 4 gesetzt |
| – | – | – | – | – | – | 30 ms warten |
| 6 | IN  | 0xa3 | 0xde37 | 0 | → `00` | FlashStatus, bis Bit 0 frei (≤100×, je 30 ms) |
| 7 | OUT | 0xa3 | 0xd901 | 0 | `24` | CommonGeneralControl zurück |
| 8 | OUT | 0xa3 | 0xd800 | 0 | `00` | TargetCode = 0 |

Bit 4 von 0xd901 ist der Auslöser; das Gerät kopiert selbst, der Host wartet
nur auf den Flash-Status. **Gemessen: 22 Durchläufe der Warteschleife, also
etwa 0,7 s** **[M]**. `0x2A` kommt aus `normalization_data_start` in der
Ressource `DMDATABASE` von `devicemanager.so`; dieselbe Tabelle nennt
`sdram_memory_address` = `0x01FF9F00` **[C]**.

Danach prüft der Hersteller mit `VerifySramCalData()`, ob es angekommen ist:
den Ergebniskopf aus dem Flash gegen 0x100 Byte ab `sdram_memory_address`,
**118 Byte verglichen, jedes 32-Bit-Wort byteverdreht** **[C]**. Am Gerät
bestätigt: vorher 118 von 118 Byte verschieden, danach 0 von 118 **[M]**.

Drei Speicher hängen an eigenen Anfragen, alle mit
wValue = Adresse >> 16 und wIndex = Adresse & 0xffff **[C]**:

| Anfrage | Richtung | Speicher | am i2600 |
|---------|----------|----------|----------|
| 0xe1 | IN/OUT | SRAM  | **stallt** (Pipe-Fehler) **[M]** |
| 0xe2 | IN     | SDRAM | funktioniert **[M]** |
| 0xe3 | IN/OUT | Flash | funktioniert **[M]** |

`ReadSramMemory` oder `ReadSdramMemory` wählt der Hersteller über ein Flag in
`Data` (Offset 0x40b8); am i2600 ist es offenbar aus, denn nur 0xe2
antwortet. Dasselbe Flag entscheidet zwischen `ImageCalibration` und
`ImageCalibrationA4` **[C]**.

Die Lage im SDRAM ist nur punktuell belegt: Flash 0x18000 entspricht
SDRAM **0x01FF0000** (byteverdreht je Wort, Suchtreffer im ausgelesenen
Flash-Abbild), der Ergebniskopf Flash 0x3FC00 entspricht 0x01FF9F00 **[M]**.
Beides mit einem einzigen Versatz lässt sich nicht erklären — die Kopie ist
also verdichtet oder stückweise. Wie genau, ist offen **[?]**.

### Warum es nie lief: das 0xAA im Session-Info-Block **[C][M]**

`DeviceSettings::InitImageCalValues(bool force)` ist der Einstieg, und sein
erster Schritt ist ein Abbruchkriterium **[C]**:

```
sessionInfo = GetSessionInfo()            # Anfrage 0x37 IN, 64 Byte
if (sessionInfo[1] == 0xAA && !force) return 0      # schon geladen, fertig
SetAutoWhite(false)                       # 0x18 OUT, 4 Byte
SetNormalizationDataStart(0x2A)           # 0xa3 OUT 0xd908
if (ReadImageCalResults() == 0)           # 0xe3 IN, Flash 0x3FC00, 0x100
    if (ScannerHasImageCal())             # Kopfbyte 0 == 0xAA
        if (MoveFlashToSRAM() == 0)
            SetFrontWhitePatchLeftLimit() # 0xa3 OUT 0xda0f
            SetRearWhitePatchLeftLimit()  # 0xa3 OUT 0xdb0f
            SetFront/RearWhitePatchTargets()
            SetAutoWhite(true)
            sessionInfo[1] = 0xAA
            SetSessionInfo()              # 0x37 OUT, 64 Byte
```

Das Byte 1 des Session-Info-Blocks ist also eine **Notiz des Hosts an sich
selbst**, die im Gerät liegt. Und hier schliesst sich der Kreis: **in allen
unseren Mitschnitten stand es bereits auf 0xAA**, der Herstellertreiber nahm
daher den Abbruchzweig, und deshalb kommt in keinem Mitschnitt ein
MoveFlashToSRAM vor — kein 0xd908, kein 0xdc4d, kein 0xd800, und auch kein
0xda0f/0xdb0f **[M]**:

```
$ tools/vendor-payloads.py captures/pcapng/b-simplex-lineart-200.pcapng --request 0x37
  [71]  IN  01 aa 00 00 06 06 a3 8d 80 46 ...
  [752] OUT 01 aa 00 00 06 06 a3 8d 80 46 ...     # unverändert zurück
```

Unsere Sequenzen sind positionsgetreu aus diesen Mitschnitten erzeugt. Also
schreibt **unser** Treiber in `GRP_SESSION_W` genau diese 64 Byte samt 0xAA
ins Gerät, ohne je geladen zu haben.

Die Kette ist am Gerät vollständig durchgemessen **[M]**, Netzschalter aus und
wieder an zwischen den Schritten:

| Schritt | Session-Byte 1 | SDRAM gegen Flash |
|---------|----------------|-------------------|
| direkt nach dem Kaltstart, `i2600-grab -s session_w` (also ohne unseren Schreibzugriff) | **0x00** | 118 von 118 verschieden |
| danach `scanimage -d kds_i2000:i2000 --help`, nur Gerät öffnen, kein Papier | **0xAA** | **0 von 118** |
| anderer Kaltstart, voller Lauf von `i2600-grab` | **0xAA** | 118 von 118 verschieden |

Die erste Zeile beantwortet die entscheidende Frage: **das Gerät setzt das
0xAA nicht selbst.** Nach dem Kaltstart ist der ganze Block null. Das 0xAA in
den Mitschnitten stammt also aus einer früheren Sitzung des
Herstellertreibers, und in unseren Läufen stammt es von uns.

Die zweite Zeile ist die Gegenprobe: es genügte, das Gerät mit dem
Herstellerrücken zu **öffnen** — kein Scan, kein Blatt —, und er hat seine
Kalibrierung selbst geladen. Er kann es also, wenn man ihm die Notiz nicht
vorweg fälscht.

Die dritte Zeile ist unser Treiber: Notiz gesetzt, nichts geladen. Ein danach
gestarteter Herstellertreiber glaubt sie und lädt ebenfalls nicht. **Damit ist
die Gegenmessung vom Anfang dieses Abschnitts erklärt**, und der Satz „es ist
kein Softwareproblem" ist widerlegt.

Nebenbei fiel auf, dass wir nicht nur das Markierungsbyte fälschen, sondern
die ganzen 64 Byte. Was der Herstellertreiber an diesem Gerät nach einem
frischen Laden hinterlässt, und was unsere Sequenz stattdessen schreibt
**[M]**:

```
Hersteller:  01 aa 00 00 ff ff 00 00 00 00 00 00 00 00 ff ff 00 00 ...
unsere:      01 aa 00 00 06 06 a3 8d 80 46 a3 8d 80 46 06 06 12 c4 ...
```

Die 60 Byte hinter der Markierung sind in unserem Fall der Zustand **eines
fremden Geräts aus dem Mitschnitt**. Was dort steht, ist nicht aufgeklärt
**[?]** — die Lage und Reihenfolge legt Weissabgleichsmittelwerte nahe, die
der Hersteller mit `GetSessionInfo()` zurückliest.

### Der Effekt im Bild **[E]**

Leeres weisses Blatt, 300 dpi grau, Innenbereich (15 % Rand weg):

```
                               ohne Laden        nach --load
Mittel                         255.0             187.5
Spanne                         255.0..255.0      180.9..192.2
mittlere Abweichung            0.00              1.06
Nachbarsprung                  0.00              0.67
Spalten > 12 unter dem Mittel  0                 0
Anteil der Pixel bei 255       100.00 %          0.00 %
```

Ohne geladene Tabellen ist das Weiss **vollständig in die Begrenzung
gefahren** — jedes Pixel 255. Die Streifen aus der Eingangsmessung sind genau
die Spalten, deren Empfindlichkeit so weit darunter liegt, dass sie als
einzige nicht anschlagen; deshalb Mittel 252.5 bei 103 Einbrüchen. Mit
geladenen Tabellen liegt dasselbe Blatt bei 187.5 mit einer Spanne von 11
Stufen, mittlerer Abweichung 1.06 statt 4.44 und Nachbarsprung 0.67 statt
2.67. Die Streifen sind weg.

Zwei Einschränkungen, ehrlich gesagt: die beiden Zeilen sind **kein saubereres
A/B**, weil die unkalibrierte Seite anschlägt und damit jede Spaltenstruktur
wegschneidet — vergleichbar sind nur die kalibrierte Messung und die
Eingangsmessung mit dem Herstellertreiber. Und `--load` setzt neben dem
Kopiervorgang auch 0xd908 und 0xdc4d; nach dem Kaltstart standen die aber
schon auf 0x2A und 0x01, der Firmware-Vorgabe, so dass nur der Inhalt im
SDRAM den Unterschied macht **[M]**.

Das Laden **überlebt die ganze Öffnungs- und Teardown-Sequenz** und bleibt
bis zum nächsten Netzschalter stehen **[E]**. Ein Kaltstart löscht es: direkt
nach dem Einschalten stallt jede FPGA- und Flash-Anfrage, weil der Booter
läuft; erst nach dem Firmware-Download ist der Weg offen.

### Das Werkzeug `i2600-calibrate` **[E]**

```bash
systemctl --user stop i2600-scand       # der Dienst belegt den Scanner
native/i2600-calibrate                  # --check: Kopf, SDRAM, Vergleich
native/i2600-calibrate --load           # Flash -> SDRAM, dann erneut prüfen
native/i2600-calibrate --session        # das 0xAA im Session-Info-Block
native/i2600-calibrate --dump FILE      # die 256 KiB Flash sichern (12 s)
native/i2600-calibrate --peek sdram ADDR LEN
```

Es **schreibt nie Flash und nie NvRam**; `--load` berührt ausschliesslich die
acht FPGA-Register oben. Eine Sicherung des Flash-Bereichs dieses Geräts
liegt als `captures/analysis/caldata-flash-0x00000-0x3ffff.bin` im Projekt
(262144 Byte, sha256 `e6f71494…40d46d85`).

Das Werkzeug ist jetzt nur noch zum Nachsehen da: der Treiber lädt selbst
(nächster Abschnitt). Für Einzelversuche bleibt es nützlich, weil es ohne die
271 Transfers der Öffnungssequenz arbeitet.

Die Zahlen in diesem Abschnitt kommen aus zwei Messhilfen in `tools/`:

```bash
tools/column-uniformity.py scan.pgm 0.15   # Spaltengleichmaessigkeit
tools/blank-page-check.py   scan.pgm 150   # was i2600-scand entscheiden wuerde
```

Die zweite baut `i2600_autocrop()` und `page_ink_percent()` in Python nach und
läuft dem C-Code hinterher, wenn er sich ändert — eine Messhilfe, keine
zweite Umsetzung.

### Der Treiber lädt jetzt selbst **[C][M][E]**

`i2600-core.c` hat den Ladeweg bekommen, als Nachbau von
`InitImageCalValues()`. Weil die Sequenzheader erzeugt werden und nicht von
Hand bearbeitet werden dürfen, sitzt die Änderung im Code: `run_steps()`
erkennt den einen Schritt, dessen Mitschnitt-Nutzlast nicht so hinaus darf —
Anfrage 0x37 OUT mit 64 Byte — und ruft statt dessen `session_info_write()`:

```
info = ctrl_in(0x37, 0, 0, 64)        # den lebenden Block lesen
info[1] = (cal_ensure() == 1) ? 0xAA : 0
ctrl_out(0x37, 0, 0, info, 64)        # nur dieses Byte geändert
```

Das ist genau, was der Hersteller tut: auch er ändert in
`InitImageCalValues()` nichts als Byte 1 und schreibt den Rest unverändert
zurück **[C]**. Erkannt wird der Schritt an der Anfrage, nicht an der Gruppe,
weil nur die Simplex-Tabelle ihre Gruppen auseinanderhält — die Duplex- und
Farbtabellen führen alles unter `GRP_DUPLEX`.

`cal_ensure()` liest den Ergebniskopf aus dem Flash, prüft bei Markierung 0xAA
den SDRAM gegen und fährt bei Abweichung die acht FPGA-Transfers. Danach
setzt es noch das White-Patch-Limit (unten). Es wird zusätzlich am Ende von
`i2600_device_open()` gerufen, damit auch `--skip session_w` ein korrigiertes
Bild liefert, und merkt sich den Erfolg nur bis zum nächsten
`i2600_device_open()`, weil ein Netzschalter oder ein Firmware-Download das
SDRAM wieder leert.

**Am Gerät erprobt [E]**, Netzschalter aus und wieder an, dann ein einziger
Lauf `i2600-grab -p 1 -R -r` auf ein leeres weisses Blatt, ohne jeden
Handgriff dazwischen:

```
[  7.881] firmware: scanner firmware running, version 0001020F
[  8.196] phase open         100 transfers, 0 skipped
[  8.525] calibration: data in SDRAM is not the same as flash, copying it over
[  9.241] calibration: shading tables are in SDRAM
[  9.485] phase prepare      171 transfers, 0 skipped
```

Innenbereich gemessen, 15 % Rand weg:

```
                               vorher (Kaltstart)   jetzt (Kaltstart)
Mittel                         255.0                183.6
Spanne                         255.0..255.0         180.1..188.5
mittlere Abweichung            0.00                 1.15
Nachbarsprung                  0.00                 0.62
Spalten > 12 unter dem Mittel  0                    0
Anteil der Pixel bei 255       100.00 %             0.00 %
```

Danach meldet `i2600-calibrate --check` 0 von 118 und `--session` das Byte 1
= 0xAA — diesmal zu Recht. Der Block sieht so aus:

```
00 aa 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

Byte 0 bleibt also 0, wo der Hersteller 1 hinterlässt
(`InitializeSessionInfo()` setzt Byte 0 = 1 und vier weitere Bytes auf 0xff
**[C]**). Wir lassen es stehen, weil wir wie der Hersteller nur Byte 1
anfassen, und das Bild ist damit in Ordnung — Byte 0 gehört offenbar nicht
zum Scanweg **[M]**.

### Das White-Patch-Limit: gesetzt, aber ohne Bildwirkung **[M][E]**

`InitImageCalValues()` setzt nach einem erfolgreichen Laden auch das linke
Limit des White Patch, des Referenzstreifens neben der Seite:
`FpgaBase::SetFrontWhitePatchLeftLimit()` schreibt über `SetHiLoPair()` zwei
Einzelbytes, **0xda0f = Hi, 0xda10 = Lo** **[C]**. Der Wert ist
`white_patch_left_limit_front` = **1326** aus `DMDATABASE`.

Gemessen, jeweils die Mittelwerte des White Patch aus 0xda19..0xda1e:

| Limit 0xda0f/0xda10 | Rot/Grau | Grün | Blau |
|---------------------|----------|------|------|
| 0x0000 (unser Kaltstart, vorher) | **0** | 131 | 471 |
| 0x052e = 1326 (Herstellerlauf) | 59 | 166 | 295 |
| 0x052e = 1326 (jetzt, unser Lauf) | 53 | 299 | 639 |

Mit dem Limit auf 0 mittelt die FPGA über den dunklen Transportrand und
meldet für Rot/Grau eine 0; mit 1326 kommt ein plausibler Wert. **Auf das
Bild wirkt es nicht** — derselbe Scan mit und ohne gesetztes Limit:

```
ohne Limit   Mittel 183.6   Spanne 180.1..188.5   Abweichung 1.15   Sprung 0.62
mit  Limit   Mittel 185.9   Spanne 182.7..190.7   Abweichung 1.09   Sprung 0.65
```

Der Unterschied im Mittel ist Lampenwärme, nicht das Register. Es steht
trotzdem drin, damit wir das Gerät so hinterlassen wie der Hersteller. Die
Mittelwerte selbst speisen bei uns nichts zurück: `wbal` liest sie, aber die
folgenden Schreibzugriffe (0xdd0b, 0xdd0a) sind feste Werte aus dem
Mitschnitt.

Das **hintere** Limit 0xdb0f/0xdb10 bleibt unangetastet: der Lauf des
Herstellertreibers an diesem Gerät hat es auf 0 gelassen **[M]**, und
`InitImageCalValues()` setzt es nur, wenn das Flag für die hintere Kamera
steht **[C]**.

`SetAutoWhite` (Anfrage 0x18, 4 Byte) senden wir weiter **nicht**. Es gibt
keinen Rücklesepfad dafür, und das Bild braucht es nicht — mehr liess sich
nicht feststellen **[?]**.

### Was die Korrektur nebenan kaputt gemacht hat **[M][E]**

Kalibrierte Bilder sind **dunkler**, weil sie nicht mehr anschlagen: Papier-
weiss liegt bei etwa 185 statt bei 255. Das trifft jeden, der einen absoluten
Pegel annimmt. Durchgesehen wurden die drei Stellen, die Pegel anfassen:

**`i2600_autocrop()` ist nicht betroffen.** Es rechnet durchweg relativ: `page`
ist das hellste Dezil der Zeilenmittel, die Schwelle `page/4`, der Saumabbau
`page*9/10`. Der einzige absolute Wert ist die Abbruchbedingung `page < 40`
(„zu dunkel zum Trennen"), und 185 ist weit darüber. Am Blatt nachgerechnet:
`autocrop` findet x 74..2532, y 250..3782 bei `page` = 175 **[M]**.

**`page_ink_percent()` in `i2600-scand` war betroffen, und zwar vollständig.**
Die Wache am Anfang

```c
if (sum / total < 200)      /* so dunkel -> hat Inhalt */
    return 100.0;
```

war auf angeschlagene Bilder geeicht. Ein beschnittenes leeres Blatt hat jetzt
einen Mittelwert von **183** — unter 200, also meldete die Funktion 100 %
Tinte, und **keine Leerseite wäre mehr verworfen worden**. Bei Duplex ist das
jede zweite Seite. Gemessen, beschnittene Seite, Grenze 0.05 %:

| Wache | leeres Blatt | dünnste Seite mit Inhalt im Bestand |
|-------|--------------|--------------------------------------|
| 200 | 100.0000 % → **bleibt** | 7.2958 % → bleibt |
| 150 | **0.0012 % → verworfen** | 7.2958 % → bleibt |

Die Wache steht jetzt auf **150**: vierzig mal unter der Grenze für das leere
Blatt, und die Seiten mit Inhalt im Bestand liegen mit 7.3 %, 20.3 % und
27.4 % unverändert weit darüber **[M]**. Ein älterer, unkalibrierter Scan
wird mit 150 genauso behandelt wie vorher.

**Das SANE-Backend ist nicht betroffen**, weil es Grau und Farbe durchreicht
und die Bitonal-Wandlung dem Frontend lässt (Abschnitt 9). Scans sehen dort
jetzt grauer aus; das ist richtig so, und die Schwellenautomatik des
Frontends fängt es.

### Die Kalibrierung neu rechnen: Teilbefund **[C]**

Davon zu trennen ist die Frage, wie man die Tabellen **neu erzeugt**, falls
die Werkswerte einmal nicht mehr passen. Der Weg ist aufgeklärt, aber nicht
nachgefahren.

`Calibration::Begin()` legt die Teilkalibrierungen an (`ImageCalibration`
bzw. `ImageCalibrationA4`, `MechanicalCalibration`, `MagCalibration`,
`LCDContrastCalibration`), `Calibration::Run()` fährt sie, `End()` räumt ab.
Die Bildkalibrierung selbst ist
`ImageCalibration::PerformImageCalibration(xml::CALIBRATIONSAVEDATA,
FwEventLogEntry*)` und läuft so **[C]**:

1. `InitImageCalValues(true)` — mit `force`, also ohne Abbruch am 0xAA
2. `SetAFEGainsToUnity()`, `StageCalTarget()` — **ein Kalibrierblatt wird
   eingezogen und mitten im Papierweg angehalten**: `SetupScannerConfig(0x0a)`
   plus Start der Transportoperation, dann warten auf das Ereignis; die
   Protokollzeile dazu lautet „Waiting for cal target to be fed & transport
   to halt"
3. `GetRiplImages()` holt Weiss- und Dunkelbild, `CheckBackgroundColor()`,
   `DetermineRGBBrightSpotLocations()` / `DetermineGrayBrightSpotLocations()`,
   `DetermineWhitePatchLocation()`, `GetWhitePatchTargets()`
4. `CalcAFEGains()` bzw. `CalcAFEGainsOffsets()` stellen die analoge
   Verstärkung je Kanal (0xe0 Kameraregister, 0x200er wValue-Bereich);
   Zielwert `desired_image_cal_pixel_average` = 34500 aus `DMDATABASE`
5. `RiplNormalize()` → `CNormalize::CalculateNormalizationTables()` →
   `ripl::CalTablesCalculate(weissbild, dunkelbild, pfad, …)`. **Gerechnet
   wird auf dem Host**, das Ergebnis landet als Datei `CalData.dat` (und
   `AFEGains.dat`) im Zwischenspeicherverzeichnis
6. `EjectCalTarget()` — `SetupScannerConfig(0x0b)`, Blatt auswerfen
7. `SaveImageCalResults()` schreibt `CalData.dat` zurück ins Gerät:
   **128 Durchläufe, je 2048 Byte, Anfrage 0xe3 OUT, Adresse = n × 0x800**,
   also genau die 256 KiB bei 0x00000..0x3FFFF. `CalData.dat` ist demnach
   262144 Byte gross.

Damit ist die Antwort auf „welcher Request bringt die Tabellen ins Gerät"
**0xe3**, in 2-KiB-Stücken — nicht 0x30, und nicht ein Sonderweg. Das ist
auch die Stelle, an der man ein Gerät unbrauchbar machen kann: wer dort
geratene Inhalte hineinschreibt, überschreibt die Werkskalibrierung. Deshalb
hat `i2600-calibrate` kein Schreibkommando.

**Auslösbar ist es über XML.** `devicemanager.so` kennt die Kommandos
`calibratebegin`, `calibraterun`, `calibrateend`, `calibratereset`
(`CXmlTaskHandler::OnCalibratebegin` → `ProcessCommands::Calibratebegin`),
`device.so` führt sie in seiner Kommandotabelle:

```
<calibratebegin exit='devprocesscommands.ExitCalibratebegin'></calibratebegin>
<calibraterun   exit='devprocesscommands.ExitCalibraterun'></calibraterun>
<calibrateend   exit='devprocesscommands.ExitCalibrateend'></calibrateend>
<calibratereset exit='devprocesscommands.ExitCalibratereset'></calibratereset>
```

`calibratebegin` nimmt die Argumente `calibrationtype`, `calibrationsavedata`
und `calibrationdirectory`. Die Werte von `calibrationtype` stehen als
Zeichenkettentabelle im Modul: `image`, `udds`, `ddr`, `ddrbist`, `klut`,
`auto_180_degrees`, `lcdcontrast`, `magnification`, `magnificationtest`,
`mechanical`, `atpithresholding`, `ithresholding`.

Den Knopf dazu hat die **Herstelleroberfläche**: `twaingui.exe` enthält
`m_driverbuttonCalibrationImage`, `…Mechanical`, `…Udds`, `…Magnification`,
`…LcdContrast` und ein `m_calibrationform`. Sie ist eine .NET-Anwendung und
braucht `mono`, das auf diesem Rechner **nicht installiert** ist — ein
usbmon-Mitschnitt einer echten Bildkalibrierung ist damit möglich, aber noch
nicht gemacht **[?]**. Dazu käme, dass ein Kalibrierblatt vorhanden sein muss.

Offen bleibt also: die **Transferfolge einer vollständigen Bildkalibrierung**
ist nicht gemessen **[?]**, und ohne Mitschnitt wird sie es auch nicht —
geraten wird hier nichts, weil der letzte Schritt in den Flash geht.

### Was als Nächstes dran wäre

1. ~~`i2600_device_open()` um den Kalibrier-Ladeweg ergänzen~~ — **erledigt**,
   siehe oben. Kaltstart liefert ohne Handgriff ein korrigiertes Bild.
2. ~~Das linke White-Patch-Limit nachziehen~~ — **erledigt** für vorne
   (0xda0f/0xda10 = 1326); es ändert die Mittelwerte der FPGA, nicht das Bild.
   Offen bleiben die **White-Patch-Ziele**, die `SetFrontWhitePatchTargets()`
   schreibt: welche Register das sind und ob sie etwas ändern, ist nicht
   untersucht **[?]**.
3. `SetAutoWhite` (0x18, 4 Byte) bleibt offen **[?]** — ohne Rücklesepfad und
   ohne sichtbare Bildwirkung liess sich nichts entscheiden.
4. ~~Die absoluten Pegelannahmen durchsehen~~ — **erledigt**, siehe oben:
   `i2600-scand` hat eine neue Wache, `autocrop` und das SANE-Backend
   brauchten keine. Offen bleibt die Frage, ob die PDFs für die
   Texterkennung von paperless einen Weisspunktabgleich brauchen — sie sind
   jetzt grauer, und ob das der Erkennung schadet, ist nicht gemessen **[?]**.
5. `mono` installieren und eine Bildkalibrierung über `twaingui` mitschneiden
   — erst dann lässt sich die Neuberechnung nachbauen.
