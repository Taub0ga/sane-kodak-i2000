# Kodak i2600 — USB-Protokoll

Ergänzung zu [README.md](README.md), Teil B. Erstellt 2026-10-19 aus den
sechs pcapng-Mitschnitten in `captures/pcapng/` und aus statischer Analyse
der Treiberbibliotheken (Ghidra 12.1.2 über REA).

**Jede Aussage ist markiert:**

| Marke | Bedeutung |
|---|---|
| **[M]** | am Mitschnitt verifiziert |
| **[C]** | im Code verifiziert (Symbol, Funktionssignatur, Datentabelle, Log-String) |
| **[MC]** | beides, unabhängig |
| **[?]** | Vermutung — nicht belegt |

> **Fortsetzung:** Die Initialisierungs- und Scan-Startsequenz steht in
> [INITIALISIERUNG.md](INITIALISIERUNG.md). Dort werden auch drei Punkte
> dieses Dokuments korrigiert bzw. geschlossen: `0x11` ist die
> Lampensteuerung und `0x1b` ist `ScannerOperation(0x0d)` (Abschnitt 4 hier
> ließ beide offen); bei `0x3a` ist **`wIndex`**, nicht `wValue`, der
> Ein/Aus-Schalter (Abschnitt 6 hier sagt das falsch); `FrontImageWidth`
> zählt in **Pixelpaaren**, die Bildzeile ist 2580 Byte, und der
> 8-Byte-„Trailer" aus Abschnitt 5 ist der letzte Datensatz eines
> rückwärts gelesenen Bildfooters, der die Geometrie mitführt
> (Abschnitt 4a und 5 hier sind entsprechend zu lesen).

Die Mitschnitte wurden mit einem eigenen usbmon-Parser neu ausgewertet
(`pcapng`-Blöcke → `struct usbmon_packet`), unabhängig von `tshark`s
Dissektoren und von den Skripten in `tools/`.

> **Zwei Fallen bei der Auswertung dieser Mitschnitte** — beide haben in
> früheren Auswertungen zu falschen Zahlen geführt:
>
> 1. **Nach Gerät filtern.** Die Mitschnitte umfassen den ganzen Bus 1.
>    Der Scanner ist Gerät **6**; `a-open-close.pcapng` enthält zusätzlich
>    456 Transfers von Gerät 3 (einem Bluetooth-Adapter: 18-Byte-Bulk-IN auf
>    *dessen* EP 0x82). Ohne Filter sieht es aus, als kämen auf 0x82
>    Kommandoantworten. **[M]**
> 2. **`urb_len`, nicht die mitgeschnittene Länge.** `tshark` lief mit
>    `-s 4096`; jeder 16384-Byte-Bildtransfer ist im pcapng auf ~4032 Byte
>    Nutzdaten gekappt. Die Datenmengen stimmen nur, wenn man das
>    `length`-Feld des URB nimmt (`len_cap` ist die gekappte Länge). Die
>    Control-Transfers (≤ 2048 Byte) sind vollständig erfasst. **[M]**

---

## 1. Architektur: wo das Protokoll liegt

Das ist das wichtigste Ergebnis für ein natives SANE-Backend, und es
korrigiert die Arbeitsannahme der Vorgängersession.

**`device.so` enthält das Gerätekommandoformat nicht.** Die Kette ist **[C]**:

```
DeviceControl (exportiertes C-API von device.so)
  -> CDevDevice::Control            (device.so 0x138500)
     -> CDevIO::Control             (device.so 0x145160)
        -> COsUsb::Ioctl            (device.so 0x1cee90)
           -> COsUsbImpl::Ioctl     (device.so 0x1c5080)
              -> openusb
```

`CDevIO::Control` hat genau zwei Aufrufer: `CDevDevice::Control` (also das
Export) und `CDevProcessCommands::ExitScanbegin`, letzteres nur mit
`Control(this, 0x22, 0,0,0,0,0)` — Ioctl 0x22 ohne Nutzlast. `COsUsb::Ioctl`
hat genau einen Aufrufer. **Damit erzeugt `device.so` keinen einzigen
Vendor-Request selbst**; alle kommen von außen durch `DeviceControl` herein.
`device.so` ist für Control-Transfers ein durchsichtiges Rohr. **[C]**

Die Signatur sagt es direkt:

```c
CDevIO::Control(EOSUSBIOCTL, unsigned char* pCmd, unsigned int cbCmd,
                unsigned char* pData, unsigned int cbData, unsigned int* pcbRead)
```

und in `COsUsbImpl::Ioctl`, Fall 3 (`OS_USBPIPE_CONTROL`), wird `pCmd`
byteweise in das openusb-Setup übernommen: `pCmd[0]`→bmRequestType,
`pCmd[1]`→bRequest, `pCmd[2..3]`→wValue, `pCmd[4..5]`→wIndex,
`pCmd[6..7]`→wLength. **Der Aufrufer übergibt also das fertige
8-Byte-USB-Setup-Paket.** **[C]**

`driver.so` hat eine eigene, statisch gelinkte Kopie derselben
OS-Schicht, aber deren `COsUsb::Ioctl` wird nirgends aufgerufen — toter
Code. **[C]**

Wer baut die Setup-Pakete? **`devicemanager.so`** (5,1 MB). Es ist die
einzige Bibliothek, die `DeviceControl` namentlich auflöst, und es enthält
keinen openusb-Code (0 openusb-Symbole, gegen 48 in device.so und
driver.so). **[C]**

Und `devicemanager.so` ist nicht etwa ein Verwaltungswerkzeug, sondern die
**eigentliche Scanner-Maschinensteuerung auf dem Host**. Quelldateinamen und
Klassen **[C]**:

```
dm_FpgaBase.cpp  dm_FpgaE2.cpp  dm_FpgaWalleye.cpp  dm_FpgaEndurance.cpp
dm_initializescanner.cpp  dm_setupscanner.cpp  dm_scanner.cpp
dm_Calibration.cpp  dm_imagecalibration.cpp  dm_MechanicalCalibration.cpp
dm_magcalibration.cpp  dm_lcdcontrastcalibration.cpp  dm_ocp.cpp
dm_statemachine.cpp  dm_NicaPrinter.cpp  dm_DeviceSettings.cpp

Klassen: FpgaBase, FpgaRegister, E2FpgaRegister, FpgaE2, DeviceSettings,
         Scanner, SetupScanner, ImageCalibration, FCT, DeviceManager
Tabellen: DeviceManager::c_BaseRegisterMap, c_E2RegisterMap,
          c_EnduranceRegisterMap
```

Der i2600 hat also **keine autonome Firmware-Kommandoschicht**. Der Host
spricht die FPGA-Register des Geräts direkt an. Das erklärt sowohl die große
Zahl an Einzelregister-Zugriffen im Mitschnitt als auch, warum Auflösung und
Farbmodus das Gerät nichts angehen (Abschnitt 3).

**Folgerung für ein SANE-Backend [?]:** Was nachzubauen ist, ist nicht ein
Satz von „Scanner-Kommandos", sondern die Initialisierungs- und
Kalibriersequenz von `devicemanager.so`. Der Umfang ist damit deutlich
größer als bei einem Gerät mit eigener Firmware-API — aber die
Referenzimplementierung liegt vollständig und mit originalen Symbolnamen vor.

---

## 2. Endpunkte und Pipes

### Was benutzt wird **[M]**

Aus allen sechs Mitschnitten, nur Gerät 6, abgeschlossene URBs:

| Endpunkt | Typ | tatsächliche Nutzung |
|---|---|---|
| EP0 | Control | **der komplette Kommandokanal**, 94–405 Vendor-Transfers je Szenario |
| 0x02 | Bulk OUT | **null Transfers in allen sechs Mitschnitten** |
| 0x81 | Interrupt IN | **null Transfers in allen sechs Mitschnitten** |
| 0x82 | Bulk IN | nur Bilddaten (Vorderseite) |
| 0x86 | Bulk IN | nur Bilddaten (Rückseite) |
| 0x88 | Interrupt IN | 8-Byte-Ereignismeldungen, nur während eines Scans |

Antworten auf Kommandos kommen **ausschließlich auf EP0 IN** (bmRequestType
`0xC0`). Auf 0x82/0x86 liegt nichts als Bilddaten. **[M]**

### Warum — die eingebettete Pipe-Konfiguration **[C]**

`device.so` trägt die Gerätekonfiguration als XML im `.rodata` (Modell
`i2600`, gemeinsamer `<common>`-Block der i2000-Familie):

```xml
<devosusbopen>
  <osusbopen>
    <configuration>1</configuration>
    <interface>0</interface>
    <alternate>0</alternate>
    <pipe><eosusbpipe>OS_USBPIPE_BULKINTERRUPT</eosusbpipe><endpoint>1</endpoint><flag>20</flag></pipe>
    <pipe><eosusbpipe>OS_USBPIPE_BULKINTERRUPT</eosusbpipe><endpoint>8</endpoint><flag>24</flag></pipe>
    <pipe><eosusbpipe>OS_USBPIPE_BULKOUT</eosusbpipe><endpoint>2</endpoint></pipe>
    <pipe><eosusbpipe>OS_USBPIPE_IMAGEFRONT</eosusbpipe><endpoint>2</endpoint></pipe>
    <pipe><eosusbpipe>OS_USBPIPE_IMAGEREAR</eosusbpipe><endpoint>6</endpoint></pipe>
  </osusbopen>
</devosusbopen>
<devretrytimeout>302000</devretrytimeout>
<devsingleimageendpoint>false</devsingleimageendpoint>
<devinterruptendpoint>true</devinterruptendpoint>
```

Damit ist die offene Frage der Vorgängersession **entschieden**:

* **`OS_USBPIPE_BULKIN` ist für dieses Gerät überhaupt nicht
  konfiguriert.** Es fehlt nicht nur in der Liste — es gibt die Pipe nicht.
  Die Vermutung, BULKIN teile sich 0x82 mit IMAGEFRONT, ist damit
  **widerlegt**: es gibt keinen BULKIN-Verkehr, weil es keinen BULKIN gibt.
  Kommandoantworten laufen über die CONTROL-Pipe. **[MC]**
* **Endpunktnummer 2 wird doppelt vergeben** — einmal als `BULKOUT`, einmal
  als `IMAGEFRONT`. Das ist kein Widerspruch, sondern genau die
  Richtungslogik aus `COsUsbImpl::BulkIOControl`
  (`if (pipe != 2 && pipe != 9 && pipe != 0xb) endpoint |= 0x80`):
  BULKOUT(2) bleibt `0x02`, IMAGEFRONT(4) wird `0x82`. **[C]**
* **Zwei `BULKINTERRUPT`-Pipes** mit Endpunkt 1 und 8 und den Flags 20 und
  24. Die Bedeutung der Flagwerte ist **nicht geklärt [?]**; die
  Flag-Namensliste in `device.so` enthält unter anderem
  `OS_USBPIPEFLAGS_MONITORBUTTONEVENTS` und
  `OS_USBPIPEFLAGS_NOTMONITORBUTTONEVENTS`, was dazu passen würde, dass
  EP 0x81 der Tastenkanal ist und im SANE-Pfad deshalb ungenutzt bleibt
  **[?]**.

### Wozu EP 0x02 (Bulk OUT)

Offene Frage 4 des README ist damit beantwortet: **Firmware- und
Anwendungsdownload.** In `devicemanager.so` **[C]**:

```
USBCMD: SendBulkOut(chunk size=0x%x bytes)
USBCMD: Load Application(Bank:%d  Block:%d  Address:0x%04X  Bytes:%d)
USBCMD: LoadInternalFirmware(SrcIdx:%d  Bytes:%d  TargetAddr:0x%04X)
DeviceManager::InitializeScanner::LoadFirmwareBank(unsigned char, unsigned char const*, unsigned int)
DeviceManager::InitializeScanner::LoadInternalFirmware(...)
DeviceManager::InitializeScanner::ProgramBooter()
```

Im normalen Scanbetrieb mit passender Firmware wird der Pfad nicht
betreten — daher null Transfers. **[C]**

### Das Ioctl-Enum `EOSUSBIOCTL`

`s_aosusblookupusbpipe` ist eine Ioctl→Pipe-Tabelle, gültig für Codes
1..0x33, aufgebaut in Sechsergruppen (je eine Operationsfamilie über
BULKIN/BULKOUT/CONTROL/IMAGEFRONT/IMAGEREAR/INTERRUPT). Der `switch` in
`COsUsbImpl::Ioctl` bestätigt die Gruppierung, und zwei unabhängige
Fundstellen belegen die Benennung **[C]**:

Aus `devicemanager.so`-Log-Strings: `OS_USBIOCTL_TIMEOUT_BULKIN`,
`…_BULKOUT`, `…_CONTROL`, `…_IMAGEFRONT`, `…_IMAGEREAR` — das ist genau die
Gruppe `case 0xd..0x12` (SetTimeout). Und
`DeviceManager::DeviceSettings::UsbIoctl` benennt fünf Codes im Klartext:

| Ioctl | Name laut Log-String | Gegenprobe in device.so |
|---|---|---|
| 1 | — (BULKIN read) | `case 1`: BulkIOControl + `OsUsbIoctlBulkInCallback` |
| 2 | — (BULKOUT write) | `case 2`: `OsUsbIoctlBulkOutCallback` |
| 3 | — (CONTROL) | `case 3`: baut das Setup-Paket |
| 4 | — (IMAGEFRONT read) | `case 4`: `OsUsbIoctlImageFrontCallback` |
| 5 | — (IMAGEREAR read) | `case 5`: `OsUsbIoctlImageRearCallback` |
| 6 | — (INTERRUPT) | `case 6`: Interrupt-Thread |
| 0x0a | `CancelIO Image Front` | `case 10`: cancel io |
| 0x0b | `CancelIO Image Rear` | `case 0xb`: cancel io |
| 0x0c | `CancelIO Interrupt` | `case 0xc`: cancel + EventWait 5 s |
| 0x0d–0x12 | `TIMEOUT_{BULKIN,BULKOUT,CONTROL,IMAGEFRONT,IMAGEREAR,INTERRUPT}` | `case 0xd..0x12`: SetTimeout |
| 0x13 | `Reset all Endpoints` | `case 0x13`: ClearHalt, abschaltbar über Cfg 1/0x9f |
| 0x16–0x1b | — (Retry-Parameter, 8 Byte) | `case 0x16..0x1b` |
| 0x1c–0x21 | — (Chunk-Size abfragen) | `case 0x1c..0x21` |
| 0x22 | *scanbegin* | `CDevIO::Control`: Busy-Prüfung; `COsUsbImpl`: `CalculateMaxChunkSize` |
| 0x23 | *resource lock* | `CDevIO::Control` → `LockDevice`, verlangt 4 Byte |
| 0x24 | `Resource Unlock` | `CDevIO::Control` → `UnlockDevice` |
| 0x27 | — | `GetDeviceInfo` |

---

## 3. Aufgabe A: Auflösung und Farbmodus — bestätigt

Die Kernbehauptung der Vorgängersession **stimmt**, und zwar deutlich
stärker als dort formuliert.

Verglichen wurden die vollständigen Vendor-Request-Ströme (bmRequestType,
bRequest, wValue, wIndex, wLength **und alle Nutzdatenbytes**) von

* `b-simplex-lineart-200` (Lineart, 200 dpi)
* `e-simplex-lineart-300` (Lineart, 300 dpi)
* `f-simplex-gray-200` (Graustufen, 200 dpi)

**Lineart 200 dpi gegen Graustufen 200 dpi: 228 gesendete Requests, davon
228 bitgleich bis auf ein einziges Feld** — `wIndex` von Request `0x1f`
(`0x8879` ↔ `0x8b4c`). Dieses Feld ist kein Modusparameter: `0x1f` ist
`DeviceSettings::SetTime(OsTimeRecord&)` **[C]**, der Wert ist die
Uhrzeit. **[M]**

**Lineart 200 dpi gegen Lineart 300 dpi:** dieselbe Lage, dazu vier
zusätzliche Requests in `e` — `0x11`, `0x10 wValue=0`, `0x10 wValue=0
wIndex=1` und eine Wiederholung des 78-Byte-Schreibzugriffs
`0xa3 wValue=0xdc00`, dessen 78 Nutzdatenbytes **byteidentisch** zur ersten
Ausführung sind. Alle vier liegen in der Abbausequenz nach dem letzten
Bildblock; es ist eine Wiederholung derselben idempotenten Operation, kein
auflösungsabhängiger Parameter. **[M]**

Der 120-Byte-Block von Request `0x30` ist in allen drei Mitschnitten
**bitgleich**. **[M]**

Die Rohdatenmengen auf EP 0x82 (über `urb_len`):

| Scan | Ergebnisbild | Rohdaten EP 0x82 |
|---|---|---|
| Lineart 200 dpi | 1657 × 2382, 1 bit | 20 989 568 B |
| Lineart 300 dpi | 2486 × 3571, 1 bit | 20 949 944 B |
| Graustufen 200 dpi | 1657 × 2387, 8 bit | 21 027 668 B |

Spannweite 0,4 %. **[M]**

### Die Bestätigung im Code

Request `0x30` ist **[C]**:

```
DeviceManager::CScannerConfiguration::SetScannerConfiguration(SDmScanConfig const&)
                    -> Set(bRequest=0x30, ..., cbData=0x78)
```

`0x78` = 120 Byte — genau die beobachtete Länge. Die Struktur heißt
`SDmScanConfig`; sie beschreibt also die *Scanner*-Konfiguration, nicht den
Auftrag.

Entscheidend ist, was in `devicemanager.so` **nicht** vorkommt: in der
vollständigen Aufrufstellen-Tabelle der Vendor-Requests (Abschnitt 4) gibt
es keinen Request für Auflösung, Bittiefe oder Farbmodus. Es gibt
`SetColorLookUpTable` und `SetColorCorrectionTable` (Kalibrierkurven,
`0xe4`/`0xe5`), aber keinen Modus- oder dpi-Parameter. Auflösung und
Farbmodus sind Begriffe des Optionsmodells in `driver.so` (235
Capabilities) und der Bildverarbeitung in `hippo.so`, nicht des Geräts. **[C]**

**Beide Belege sind unabhängig und stimmen überein. Der Scanner scannt
immer gleich.** Für ein natives SANE-Backend heißt das: ein einziger
Gerätemodus, und Skalierung, Bittiefe und Bitonal-Wandlung rechnet der Host.
**[MC]**

---

## 4. Aufgabe B: das Kommandoformat

### Das Format auf dem Draht **[MC]**

Es gibt **kein eigenes Kommando-Framing**. Jedes Kommando *ist* ein
USB-Vendor-Control-Transfer auf EP0:

```
bmRequestType = 0x40  (Host → Gerät)   oder   0xC0  (Gerät → Host)
bRequest      = Opcode          (ein Byte)
wValue        = erster Parameter  (16 bit, Bedeutung je Opcode)
wIndex        = zweiter Parameter (16 bit, bei Kameraoperationen 0 = Front, 1 = Rear)
wLength       = Nutzdatenlänge  (0 .. 2048 beobachtet)
```

Kein Längenfeld, keine Sequenznummer, keine Prüfsumme, kein Statuswort in
der Nutzlast: Länge und Richtung stehen im Setup-Paket, der Erfolg ist der
USB-Status. In allen sechs Mitschnitten hatte **kein einziger
Control-Transfer einen Status ≠ 0** — auch nicht `d1-no-paper`, wo der Scan
mit „Document feeder out of documents" scheitert. Fehler werden über den
Statusblock und die Interrupt-Ereignisse gemeldet, nicht über
USB-Stalls. **[M]**

Die Code-Entsprechung ist exakt **[C]**:

```c
DeviceManager::DeviceSettings::Get(u8 bRequest, u16 wValue, u16 wIndex,
                                   u32 cbData, u8* pData)
    -> SendCommand(bmRequestType=0xC0, bRequest, wValue, wIndex, cbData, pData)

DeviceManager::DeviceSettings::Set(u8 bRequest, u16 wValue, u16 wIndex,
                                   u32 cbData, u8* pData)
    -> SendCommand(bmRequestType=0x40, bRequest, wValue, wIndex, cbData, pData)
```

`Get`/`Set` sind die beiden Engpässe, durch die jeder Vendor-Request läuft
(35 bzw. 57 direkte Aufrufstellen). Genau die zwei Werte `0xC0` und `0x40`
kommen im Mitschnitt vor, und `SendCommand` protokolliert sie auch so:

```
USBCMD: %s(reqtype=%s:0x%X, req=0x%X, val=0x%X, indx=0x%X, datasize=0x%X)
```

Mit `KDS_DEBUGUSB` (offene Frage 6 des README) ist also tatsächlich ein
benanntes Request-Protokoll zu erwarten. **[C]**

### Die Opcode-Tabelle

Aus allen 92 direkten Aufrufstellen von `DeviceSettings::Get`/`::Set` in
`devicemanager.so`, mit den Konstanten aus dem Disassembly und dem Namen der
aufrufenden Funktion. **Das sind keine geratenen Bedeutungen — es sind die
Originalfunktionsnamen des Herstellers.** **[C]**
Die Spalte „Mitschnitt" sagt, ob und wie oft der Request in den sechs
Mitschnitten vorkommt. **[M]**

| bReq | Richtung | Code-Fundstelle (`devicemanager.so`) | wLength | Mitschnitt |
|---|---|---|---|---|
| `0x00` | IN | — *(einzige Get-Stelle ohne erkennbare Konstante:* `DeviceSettings::GetStatus()`, `cbData=0x20`*)* | 32 | ja, gepollt **[MC]** |
| `0x02` | IN | `DeviceSettings::GetFirmwareVersions()` | 56 | 1× je Sitzung |
| `0x03` | IN | `CMachineConstants::GetMachineConstants()` | 32 | 1× je Sitzung |
| `0x09` | IN | `CLogs::GetEventLog()` | 0x650 | nein |
| `0x0a` | IN | `CTraceLog::GetUnreadByteCount()` / `CLogs::GetTraceLog()` | 2 / 0xff0 | nein |
| `0x10` | OUT | `DeviceSettings::ScannerOperation(_ScannerOp)` | 0 | **ja, Scanstart/-stop** |
| `0x11` | OUT | *(keine Konstanten-Fundstelle)* | 0 | ja, immer |
| `0x13` | OUT | `CLogs::AddEventToScannerLog(FwEventLogEntry const&)` | 5 | nein |
| `0x16` | OUT | `DeviceSettings::SetButtonSequenceNumber(u16, u16)` | 0 | 1× je Sitzung |
| `0x17` | OUT | `DeviceSettings::StartCapture(unsigned short)` | 0 | **ja, nur Scan** |
| `0x18` | OUT | `DeviceSettings::SetAutoWhite(bool)` | 4 | nein |
| `0x1b` | OUT | *(keine Konstanten-Fundstelle)* | 0 | ja, nur Scan |
| `0x1e` | OUT | `DeviceSettingsP2::SetBatchState(u8,u8,u8)` | — | nein |
| `0x1f` | OUT | `DeviceSettings::SetTime(OsTimeRecord&)` | 0 | 1× je Sitzung |
| `0x20` | OUT | `DeviceSettingsA4::ScannerOperation(_ScannerOp)` | 4 | nein (Flachbett) |
| `0x30` | OUT | `CScannerConfiguration::SetScannerConfiguration(SDmScanConfig const&)` | **0x78 = 120** | **ja, 2× je Scan** |
| `0x31` | IN/OUT | `DeviceSettings::GetEnergyStar()` / `SetEnergyStar(u8)` | 1 | ja, nur Scan (OUT) |
| `0x32` | IN/OUT | `DeviceSettings::Get/SetLowPowerTimeout()` | 1–2 | ja, immer |
| `0x33` | IN/OUT | `DeviceSettings::GetMeters()` / `SetMeters(FwUsbMeters const&)` | 20 | ja, immer (IN) |
| `0x34` | IN/OUT | `DeviceSettings::GetEndOfLine()` / `SetEndOfLine(FwEndOfLine const&)` | 0x37 = 55 | 1× je Sitzung |
| `0x35` | IN/OUT | `GetNvRamData()` / `SetNvRamData(NvRam const&)` (wValue 0, 128 B) · `Get/SetConsumableMetersData()` (wValue 1, 256 B) | 128 / 256 | ja, immer |
| `0x36` | IN/OUT | `GetBoardSerialNumber(EUSBBOARDID, char*)` (16 B) · `DeviceSettingsA4::Get/SetMechanicalCalData()` (wValue 2) · `SetUddsCalibrationData()` (wValue 1, 4 B) | 16 | 1× je Sitzung (IN) |
| `0x37` | IN/OUT | `DeviceSettings::GetSessionInfo()` / `SetSessionInfo(SDmSessionInfo&)` | 0x40 = 64 | ja, immer |
| `0x3a` | OUT | `Scanner::StartInterruptEvents()` (wValue=1) / `Scanner::StopInterruptEvents()` | 0 | **ja, 2× je Scan** |
| `0x3c` | OUT | `DeviceSettingsP2::SetUddsCalibrationData()` | 4 | nein |
| `0x3f` | OUT | `Diagnostics::SetTransportMotor(bool)` (wValue=0x0a) / `SetFeederClutch(bool)` | — | nein |
| `0x41` | IN/OUT | `DeviceSettings::Get/SetCameraData(u16,u16,u32,u8*)` | var. | nein |
| `0x44` | OUT | `Diagnostics::SetLamps(bool,bool,bool,bool)` | 1 | nein |
| `0x45` | IN/OUT | `DeviceSettingsP2::Get/SetBatchData(...)` | 3 | ja, immer |
| `0x5f` | OUT | `DeviceSettingsP2::SetOCPLED(u8, u16)` | — | nein |
| `0x62` | OUT | *(keine Konstanten-Fundstelle; passt zu* `OCP::PopulateAllMessages` */* `dm_ocp.cpp`*)* **[?]** | 768 | 1–2× je Sitzung |
| `0x63` | OUT | `LCDContrastCalibration::Begin()` (wValue=1) / `Run()` (2) / `End()` (3) | 1 | nein |
| `0x64` | OUT | `OCP::PopulateAllMessages(bool)` | — | nein |
| `0xa0` | OUT | `InitializeScanner::LoadInternalFirmware(...)` | var. | nein |
| `0xa2` | IN/OUT | `FCT::Get/SetESN()` (wValue 0x1fe0, 24 B) · `FCT::Get/SetDateStamp()` (wValue 0x1ff8, 8 B) · `InitializeScanner::ProgramBooter()` | 8–24 | nein |
| `0xa3` | OUT | `InitializeScanner::LoadFirmwareBank(u8, const u8*, u32)`, dort mit `wValue=0xd80e` | 1,4,78 | **ja, sehr viel** |
| `0xa5` | IN/OUT | `FCT::ReadLocation(FCTADDRESSTYPE, u32)` / `WriteLocation(...)` | 1 | nein |
| `0xe0` | IN/OUT | `ReadAfeDevice(u8, u16, u8*)` / `WriteAfeDevice(u8, u16, u8)` · `Read/WriteCameraDevice(u16,u8,u16,u8*,u16)` · `SetCameraPageValue(u16, u8)` mit `wValue=0x001f` | 1,3,32 | **ja, sehr viel** |
| `0xe1` | IN/OUT | `Read/WriteSramMemory(u32, u8*, u16)` | var. | nein |
| `0xe2` | IN | `ReadSdramMemory(u32, u8*, u16)` | var. | nein |
| `0xe3` | IN/OUT | `Read/WriteFlashMemory(u32, u8*, u16)` | 256 | ja, 1× je Sitzung (IN) |
| `0xe4` | OUT | `DeviceSettings::SetColorLookUpTable(u16, u8*, CAMERATYPE)`, `wValue=0x0100` | 0x400 = 1024 | nur Textmitschnitte |
| `0xe5` | OUT | `SetColorCorrectionTable(u8*, CAMERATYPE)` | 0x800 = 2048 | nur Textmitschnitte |
| `0xf0` | OUT | `DeviceSettings::PerformDiagnosticAction(u16, u16)` | — | nein |
| `0xf1` | OUT | `DeviceSettings::SendDiagnosticRequest(u16, u16, u8*, u16)` | — | nein |
| `0xf2` | IN | `DeviceSettings::GetDiagnosticResults(u8*)` | 0x200 | nein |

Die wLength-Werte aus dem Code stimmen überall mit dem Mitschnitt überein,
wo beide vorliegen — `0x30`→120, `0x37`→64, `0x34`→55, `0x35`→128/256,
`0x36`→16, `0x33`→20, `0x45`→3, `0xe4`→1024, `0xe5`→2048, `0xe3`→256.
**Das ist eine unabhängige Gegenprobe über zehn Requests und bestätigt die
Zuordnung. [MC]**

### Was damit umbenannt werden muss

Gegenüber der Deutungstabelle in README Teil B.1 ergeben sich
**Korrekturen**, alle **[C]**:

| bReq | README-Deutung | tatsächlich |
|---|---|---|
| `0x03` | „Zähler?" | **Machine Constants** (`CMachineConstants::GetMachineConstants`) |
| `0x16` | „`wValue=0x0009 wIndex=0x01ff`" | **SetButtonSequenceNumber** |
| `0x17` | „Transfer freigeben" | **StartCapture** |
| `0x1f` | „`wValue=0x3086`" | **SetTime** — `wIndex` ist die Uhrzeit, kein Gerätezustand |
| `0x32` | „16-Bit-Parameter" | **Low-Power-Timeout** |
| `0x33` | „—" | **Meters** (Betriebszähler) |
| `0x34` | „Identität" | **End-Of-Line-Datensatz** (Werksprüfdaten; enthält u. a. VID/PID und Modellstring) |
| `0x35` | „Firmware-Stand" | **NvRam** (wValue 0) bzw. **Consumable Meters** (wValue 1) |
| `0x36` | „Seriennummer" | **Board-Seriennummer** (`EUSBBOARDID` in `wValue`) |
| `0x37` | „Konfigurationsblock" | **Session Info** (`SDmSessionInfo`) |
| `0x3a` | „vermutlich pro Kamera" | **Start/Stop Interrupt Events** — `wValue=1` startet, 0 stoppt; `wIndex` ist nicht die Kamera |
| `0x45` | „—" | **Batch Data** |
| `0xa3` | „indirekter Registerzugriff" | Registerzugriff **und** `LoadFirmwareBank` — derselbe Opcode in zwei Rollen |
| `0xe0` | „direkter Registerzugriff" | **AFE- und Kamera-Register** (`ReadAfeDevice`/`WriteAfeDevice`, `Read/WriteCameraDevice`) |
| `0xe3` | „Speicherlesen" | **Flash** |
| `0xe4` | „Tabellen-Upload (Gamma/LUT)" | **Color Look-Up Table**, je Kamera |
| `0xe5` | „Tabellen-Upload" | **Color Correction Table**, je Kamera |

Das wiederkehrende Muster um Register `0x1f` („schreiben, schreiben, lesen,
dann Zielregister") ist **`SetCameraPageValue(u16, u8)` mit `wValue=0x001f`**
— also ein **Page-/Bank-Select der Kamera**, kein allgemeiner
Adress-Handshake. `wIndex` 0/1 ist dabei tatsächlich die Kamera
(Front/Rear), bestätigt durch `Read/WriteCameraDevice` und
`xml::CAMERATYPE`. **[C]**

Nicht geklärt bleiben **[?]**: `0x11` und `0x1b` (keine Aufrufstelle mit
Konstante gefunden — vermutlich über einen Wrapper oder inlined) sowie
`0x62` (768 Byte, Inhalt sieht nach 5×8-Zeichenmustern aus; die Nähe zu
`dm_ocp.cpp`/`OCP::PopulateAllMessages` und `dm_lcdcontrastcalibration.cpp`
macht „Zeichengenerator für das Bedienfeld-Display" plausibel, belegt ist
es nicht).

### Ablauf eines Scans **[M]**

Aus `b-simplex-lineart-200.pcapng`, Zeiten relativ zum ersten URB, mit den
Code-Namen aus der Tabelle:

```
 5.216   Standard: GET_DESCRIPTOR mehrfach, SET_CONFIGURATION
 5.231   C0 00  GetStatus (3×)
 5.234   C0 02  GetFirmwareVersions
 5.235   C0 34  GetEndOfLine        -> 04 0a 60 1d .. "KODAK" .. "i2600 SCANNER"
 5.245   C0 36  GetBoardSerialNumber-> "0000000048785942"
 5.249   C0 37  GetSessionInfo
 5.250   40 1f  SetTime
 5.252   C0 03  GetMachineConstants
 5.253   C0 35  GetNvRamData        -> "2014/02/13 10:32:04:056" ...
 5.271   40 a3  Register da27/db27 = 00 00 00 01        (Front/Rear)
 5.273   C0 e3  ReadFlashMemory  wIndex=fc00, 256 B
 5.289   a3 d901: lesen 0x24, schreiben 0x20
 5.311   lange e0-Sequenz: SetCameraPageValue(0x1f) + Zielregister,
         erst wIndex=0 (Front), dann wIndex=1 (Rear)
 5.402   C0 45  GetBatchData -> 02 00 00
 5.439   40 62  768-Byte-Block (Bedienfeld? [?])
 5.448   40 35  SetNvRamData
 5.592   C0 33  GetMeters -> f5 c4 06 00 5a 1c 0b 00 ...
 5.741   40 62  768-Byte-Block (zweites Mal)
 5.772   40 3a  StartInterruptEvents (wValue=1, wIndex=1)
 5.773   40 1b  [?]
 5.776   EP88   01 End of Operation
 5.840   EP88   15 Lamp State
 5.904   EP88   03 Ding (EventId 0, Value1=1)
 5.968   EP88   03 Ding (Value1=2)
 5.977   40 31  SetEnergyStar = 01
 6.012   40 11  [?]  (wValue=1)
 6.043   40 37  SetSessionInfo
--- Referenzdurchgang ---
 6.624   40 30  SetScannerConfiguration, 120 B, Byte 0x30 = 02
 6.627   40 10  ScannerOperation (wValue=1)      -> starten
 6.736   EP88   10 Power State, Value1=4
 6.800   EP88   02 Start of Operation
 6.801   40 17  StartCapture (wValue=1)
 6.802   EP82   4 Transfers, 51 824 B                 <- Dunkelreferenz
 6.805   40 10  ScannerOperation (wValue=0)      -> stoppen
 6.864   EP88   42 Imaging Complete, Value2 = 1
 6.928   EP88   01 End of Operation
--- Scandurchgang ---
 6.931   40 30  SetScannerConfiguration, 120 B, Byte 0x30 = 01
 6.933   40 10  ScannerOperation (wValue=1)
 6.992   EP88   10 Power State, Value1=3
 7.056   EP88   60 Screen Missing, Value1=0x0109
 7.120   EP88   10 Power State, Value1=4
 7.184   EP88   12 Transport State, Value1=1      <- Transport läuft
 8.400   EP88   12 Transport State, Value1=2
 8.464   EP88   02 Start of Operation
 8.641   EP82   637 Transfers, 10 421 120 B           <- Blatt 1
 9.489   EP88   42 Imaging Complete, Value2 = 2
 9.552   EP88   41 Page Exit, EventId 1               <- Blatt 1 ausgeworfen
 9.826   EP82   642 Transfers, 10 516 624 B           <- Blatt 2
10.320   EP88   13 Tray State, Value1=1
10.641   EP88   42 Imaging Complete, Value2 = 3
10.769   EP88   41 Page Exit, EventId 2, Value1=1     <- Blatt 2 ausgeworfen
10.823   40 10  ScannerOperation (wValue=0), 3×
11.153   EP88   12 Transport State, Value1=0          <- Transport steht
11.210   EP82   URB abgebrochen (Status -2)
11.317   40 11  [?]
11.521   40 3a  StopInterruptEvents
```

Bemerkenswert: ein Durchgang ist `0x30`(Setup) → `0x10 wValue=1`(Start) →
optional `0x17`(StartCapture) → Bildblöcke → `0x10 wValue=0`(Stop). Ein
einzelner `0x30`-Block deckt dabei einen **mehrblättrigen** Lauf ab — es gibt
nur zwei `0x30`-Requests je Scan, aber drei Bildblöcke. **[M]**

---

## 4a. Die FPGA-Register (`0xa3`) — vollständig benannt

Nach dem ersten Entwurf dieses Dokuments ließ sich die Registerkarte
(offene Frage 5) auflösen. `devicemanager.so` enthält die Registeradressen
als **2-Byte-Konstanten mit Originalnamen** im `.rodata`, erreichbar als
Symbole `DeviceManager::{FpgaRegister,E2FpgaRegister,EnduranceFpgaRegister}::<Name>`.
170 Stück, dekodiert nach `captures/analysis/fpga-registermap.txt`
(`tools/protocol/regmap.py`). **[C]**

### Format von `0xa3` **[MC]**

```
bmRequestType 0x40 / 0xC0
bRequest      0xa3
wValue        Startadresse des Registerblocks
wIndex        0
wLength       Zahl der Register — je Register genau ein Byte
```

Zwei unabhängige Belege für „wLength = Registeranzahl":

* Der 78-Byte-Schreibzugriff mit `wValue=0xdc00` deckt genau die Register
  `0xdc00` (`JpegControl`) bis `0xdc4d` (`SDRamDriverEnable`) ab — das sind
  **genau 78** aufeinanderfolgende Adressen. **[MC]**
* Der 4-Byte-Zugriff auf `0xda27` trifft `FrontDocIdByte3` und die drei
  Folgeadressen; die Karte benennt nur `Byte3`, die Byte-2/1/0-Register
  liegen erwartungsgemäß direkt dahinter. Nutzlast `00 00 00 01` =
  DocId 1. **[MC]**

Die Adressräume gliedern sich sauber: `0xd7xx` Revision/Testbarkeit,
`0xd8xx` Speicherzugriff und Kamera-Bytes, `0xd9xx` gemeinsame
Bildverarbeitung, `0xdaxx` **Frontkamera**, `0xdbxx` **Rückkamera** (byte-
für-byte spiegelbildlich zu `0xdaxx`), `0xdcxx` JPEG und
User-Preference-Operanden, `0xddxx` Udds (Ultraschall-Doppelblatterkennung),
`0xdexx` SPI-Flash, Einzugskupplung, Drucker. **[C]**

Die vollständig benannten Zugriffe je Mitschnitt liegen in
`captures/analysis/*.fpga-registers.txt` (`tools/protocol/regs.py`).

### Was der Treiber beim Scan tatsächlich setzt **[M]**

Aus `b-simplex-lineart-200`, Frontkamera, in Reihenfolge:

| Register | Wert | |
|---|---|---|
| `X_Scaling` | `0x00` | |
| `Y_Scaling` | `0x00` | |
| `RedRephaseLineStorageAmount` | `0x07` | |
| `GreenRephaseLineStorateAmount` | `0x04` | *(Schreibfehler im Original)* |
| `BlueLineBuffer` | `0x28` | |
| `X_Y_ResolutionFooterOnly` | `0x22` | |
| `JpegControl`…`SDRamDriverEnable` | 78 Byte | |
| `FrontFixedPageLength` | `0x0014` = **20** | Referenzdurchgang |
| `FrontGrayScaleSelection` | `0x00` | |
| `FrontImageWidth` | `0x050a` = **1290** | |
| `FrontPageDetectLeftLimit` | `0x0004` = 4 | |
| `FrontIP_Control` | `0x00` | |
| `FrontImageFormatFooterOnly` | `0x01` | |
| `FrontPageStartThreshold` | `0x0014` = 20 | |
| `FrontPageDetectWidth` | `0x050a` = **1290** | |
| `FrontHorizontalCropLeft` | `0x0004` = 4 | |
| `FrontWhiteBalanceTargetRedGs` | `0x0141` = 321 | |
| `CommonGeneralControl` | `0x28` | löst die Messung aus |
| *(lesen)* `FrontWhiteBalanceTarget{RedGs,Green,Blue}` | 321, 320, 331 | |
| *(lesen)* `FrontWhitePatchAverageRedGs` | 318, 323, 321 | dreimal — Mittelung |
| `UddsTwoPageSampleInterval` | `0x46` | |
| `UddsMultifeedSetup` | `0x40` | |
| `FrontValidPixelLevelThreshold` | `0x12` | vor dem Scandurchgang |
| `FrontFixedPageLength` | `0x122a` = **4650** | Scandurchgang |

Im Duplexmitschnitt wird derselbe Satz zusätzlich auf `0xdbxx`
(Rückkamera) geschrieben, mit identischer `RearImageWidth` = 1290, aber
`RearHorizontalCropLeft` = `RearPageDetectLeftLimit` = `0x16` = **22**
gegen 4 vorn — ein kamerabezogener Versatz. **[M]**

### Damit ist Aufgabe A auch auf Registerebene geschlossen **[MC]**

In **allen drei** Vergleichsmitschnitten (Lineart 200 dpi, Lineart 300 dpi,
Graustufen 200 dpi) werden diese Register mit **denselben Werten**
geschrieben:

```
X_Scaling                 = 0x00     <- keine Skalierung im Gerät
Y_Scaling                 = 0x00
X_Y_ResolutionFooterOnly  = 0x22
FrontImageWidth           = 0x050a
FrontGrayScaleSelection   = 0x00     <- gleich bei Lineart und Graustufen
FrontHorizontalCropLeft   = 0x0004
FrontFixedPageLength      = 0x0014 / 0x122a
```

Zwei Namen sind dabei für sich schon Beweis:

* **`X_Scaling = 0` und `Y_Scaling = 0`** — die Skalierungseinheit des FPGA
  ist abgeschaltet.
* **`X_Y_ResolutionFooterOnly`** — der Registername sagt, dass die
  Auflösungsangabe *nur in den Bild-Footer* geschrieben wird, also
  mitgeführt, aber nicht umgesetzt. Passend dazu
  `FrontImageFormatFooterOnly = 1`.

Dass zusätzlich `FrontGrayScaleSelection` im Graustufen- und im
Lineart-Mitschnitt gleich ist, schließt auch einen geräteseitigen
Farbmoduswechsel aus. **Auflösung, Bittiefe und Farbmodus erreichen das
Gerät nie.** **[MC]**

### Zur Geometriefrage — näher, aber noch nicht geschlossen **[?]**

Die Werte sind jetzt bekannt, ihre **Einheiten nicht**:

* `FrontImageWidth = 1290`. Als Pixel gelesen wäre das für eine
  A4-Breite zu wenig; die Registernamen der E2-Gruppe zählen ausdrücklich in
  **Words** (`FrontImageBorderWordsLeftHi/Lo`, `FrontJobSpecWordsHi/Lo`),
  ein „Word" ist aber nicht definiert. Bei 2 Byte/Word wären es 2580
  Byte/Zeile, bei 4 Byte/Word 5160.
* `FrontFixedPageLength = 4650` im Scandurchgang, `20` im
  Referenzdurchgang. Für den Referenzblock passt das grob: 20 × 2580 =
  51 600 gegen gemessene 51 816 Byte Nutzlast (0,4 % daneben) — **nicht
  exakt**, also ist mindestens ein Zusatzanteil je Block oder je Zeile im
  Spiel.
* Für die Bildblöcke trägt es nicht: die beiden Blätter desselben Scans
  liefern 10 421 120 und 10 516 624 Byte, obwohl `FixedPageLength`
  dazwischen nicht neu geschrieben wird. **Die Blocklänge folgt also dem
  tatsächlichen Blattdurchlauf, nicht dem Register.** `FixedPageLength` ist
  eine Vorgabe für die Seitenerkennung, keine Übertragungslänge. **[M]**
* Die 508-Byte-Granularität (= 4 × 127) bleibt unerklärt. Der Faktor 127
  taucht in jeder Blockgröße auf.

Der entscheidende Versuch bleibt derselbe: **eine physisch kürzere Vorlage
scannen** und sehen, wie sich Blocklänge und die Register verhalten. Zweiter
Weg: `FrontImageWidth` künstlich verändern und die Blockgröße beobachten —
das bestimmt die Einheit direkt.

---

## 5. Aufgabe D: der Bilddatenstrom

### Aufteilung **[MC]**

0x82 trägt die Vorderseite (`OS_USBPIPE_IMAGEFRONT`), 0x86 die Rückseite
(`OS_USBPIPE_IMAGEREAR`); `devsingleimageendpoint=false`, daher
`CDevImageManagerTwoImageEndpoint`. Im Duplexmitschnitt laufen beide
Endpunkte gleichzeitig und liefern praktisch gleich viel (31,29 MB vorn,
31,22 MB hinten). **[M]**

### Kein Framing **[M]**

Es gibt **keine Rahmen-, Block- oder Zeilenheader**. Der Host liest in
16 384-Byte-Blöcken (`min(rest, 0x4000)` bzw. `CalculateMaxChunkSize`), und
mitten in einem Block fängt ein Transfer mit gewöhnlichen Pixelwerten an:

```
b0 t0    len=16384 | 02 00 00 01 00 00 00 00 00 00 05 00 08 03 01 02
b1 t100  len=16384 | 97 9c a5 9b 9d a6 93 ad 8e ad a3 ab 96 af 8f aa
b1 t300  len=16384 | c0 b7 a5 9b a7 a6 b2 a7 a4 ad b5 bd d1 cf c7 c5
```

Die 16 384 ist eine **Leseblockgröße des Hosts, keine Struktur des
Stroms**. **[M]**

### Die Daten sind 8-bit-Graustufen **[M]**

Papier liegt bei 0xd0–0xe3, Leerlauf vor und nach dem Blatt bei 0x00–0x0e.
Ein Helligkeitsprofil über einen Block (Mittelwert je Transfer) zeigt
durchgehend: dunkler Vorlauf, Seite, dunkler Nachlauf. Das gilt in
**Lineart- wie Graustufenmitschnitten identisch** — ein weiterer Belegfür
Abschnitt 3.

### Blockende: Kurztransfer plus 8-Byte-Trailer **[M]**

Ein logischer Block endet mit einem Transfer < 16 384 Byte (USB-Short-Packet).
Die letzten 8 Byte jedes **vollständig erfassten** Blocks sind strukturiert —
in allen 7 vollständig erfassten Blöcken ohne Ausnahme:

```
... 05 03 05 04 08 05 | 05 04 08 05 | 00 5e 01 ff     (51 824 B)
... 04 03 05 06 05 0b 0d 09 09 07 08 05 03 06 04 06 | 03 06 04 06 | 00 e8 01 ff
... 2c a5 a8 00       | f8 02 18 82 | f8 02 18 82 | 00 64 01 ff
```

Also: **die letzten vier Nutzdatenbytes noch einmal, dann
`00 <u16 LE> ff`.** Die Verdopplung gilt in 7 von 7 Fällen (`d[-12:-8] ==
d[-8:-4]`). Der 0xff-Wert ist strukturell eindeutig: innerhalb des Stroms
kommt **kein einziges 0xff vor** (Maximum in einem Dunkelblock: 0x25), und
in dunkler Umgebung sticht `01 ff` heraus. Der u16 ist 0x015e bei jedem
Referenzblock (immer derselbe Wert) und variiert bei Bildblöcken
(0x01e8, 0x0164). Seine Bedeutung ist **nicht geklärt [?]**.

### Blockgröße: Vielfaches von 508 Byte **[M]**

Nimmt man diesen 8-Byte-Trailer als Trailer an, dann sind **alle 11
beobachteten Blöcke exakte Vielfache von 508 Byte**:

| Trailerlänge angenommen | ggT der Nutzlastgrößen |
|---|---|
| 0 Byte | 4 |
| 4 Byte | 4 |
| **8 Byte** | **508** |

Der ggT springt genau bei 8 auf 508. Dass 11 unabhängige Blöcke zufällig
den Faktor 127 teilen, ist praktisch ausgeschlossen. Nutzlast/508 ergibt
102 für den Referenzblock und 20 295…20 702 für die Bildblöcke. **508 ist
die Ausgabegranularität des Geräts.** **[M]**

**Ob 508 Byte eine Sensorzeile sind, ist offen [?]**, und die Hinweise
sprechen dagegen:

* Eine Autokorrelation über 4032 zusammenhängende Byte aus der Seitenmitte
  zeigt **keine Periodizität unter 2000 Byte** (monoton fallend ab lag 8).
  Eine Zeile ist also breiter als 2000 Byte — 508 wäre dann ein Bruchteil
  einer Zeile. **[M]**
* **Keine einzige Zeilenlänge teilt alle Blöcke.** Weder 1016, 1524, 2032,
  2540, 3048 noch 5080 gehen in allen Nutzlasten auf. Die Blöcke sind also
  keine ganzzahligen Zeilenmengen, sondern werden auf 508-Byte-Grenzen
  geschnitten. **[M]**

Der Code nennt die naheliegenden Kandidaten, aber ohne auslesbare Werte: die
NvRam-Dumpstrings in `devicemanager.so` enthalten
`u16FrontLineWidth`, `u16RearLineWidth`, `u16FrontLeftPixel`,
`u16RearLeftPixel`, `u16FrontCameraDistance`, `u16RearCameraDistance`,
`u16MagnificationTargetFixedLength` **[C]**. Im 120-Byte-`0x30`-Block stehen
bei Offset 0x28 die Paare `36 0c` = 3126 und `32 0c` = 3122 — zwei
dicht benachbarte Werte, wie man sie für Front- und Rückkamera erwartet. Die
Zuordnung „3126 = `u16FrontLineWidth`" ist **plausibel, aber unbelegt [?]**,
und 3126 teilt die Blockgrößen nicht.

**Die offene Frage 1 des README bleibt damit offen.** Der entscheidende
Versuch steht weiter aus: dieselbe Konfiguration mit einer **physisch
kürzeren Vorlage** scannen. Werden die Blöcke proportional kleiner, ist die
Blockgröße die Seitenlänge; bleiben sie gleich, ist sie fest. Dieser Versuch
braucht jemanden am Gerät, der das Papier einlegt.

### Seitenende **[MC]**

Das Seitenende steht **nicht im Bildstrom**, sondern auf EP 0x88. Nach jedem
abgeschlossenen Block kommt `42 Imaging Complete` mit laufendem Zähler in
`u32Value2`, nach jedem ausgeworfenen Blatt `41 Page Exit` mit der
Blattnummer in `u8EventId`. Im Bildstrom selbst markiert nur der
Kurztransfer das Blockende. **[M]**

### Korrektur: ein Simplexscan hat **nicht** zwei Durchgänge je Seite

README Teil B.2 schließt aus „zwei Bildblöcke bei einem Simplexscan einer
Seite" auf zwei Durchgänge je Seite. **Das ist falsch — es waren zwei
Blätter.** Drei unabhängige Belege:

1. **Helligkeitsprofile.** Jeder Bildblock zeigt genau ein vollständiges
   Blatt: dunkler Vorlauf (~40 Transfers), Seite, dunkler Nachlauf
   (~35 Transfers). Zwei Durchgänge *derselben* Seite müssten ähnlich
   aussehen; die Profile der beiden Blöcke sind **verschieden**. **[M]**
2. **Kreuzkorrelation der Profile über alle Mitschnitte.** Es gibt genau
   zwei wiederkehrende Blattidentitäten. `b/0x82/Block2` und
   `c/0x82/Block3` korrelieren mit **r = 1,00** — dasselbe physische Blatt
   in zwei verschiedenen Mitschnitten. Entsprechend
   r = 0,96…0,99 innerhalb jeder Gruppe und r ≈ 0,5 zwischen ihnen. **[M]**
3. **Die Interrupt-Ereignisse.** Im Simplexmitschnitt kommt
   `41 Page Exit` **zweimal**, mit `u8EventId` = 1 und 2. **[MC]**

Daraus folgt für die Datenmengen: **~10,4 MB sind die Rohdaten eines
A4-Blattes**, nicht eines halben. Die 21 MB aus Abschnitt 3 sind zwei
Blätter. Die Relation „10 MB Rohdaten → 0,5 MB Bitonalbild" bleibt
bestehen, der Faktor ist aber auf ein Blatt zu beziehen.

Im Duplexmitschnitt liefen 3 Blätter durch (je 3 Bildblöcke auf 0x82 und
0x86). `c/0x86/Block3` korreliert mit keinem anderen Block (r ≤ 0,49) —
eine Rückseite, die sonst nirgends vorkommt. **[M]**

### Der 51 824-Byte-Block **[M]**

Immer der erste Block jedes aktiven Endpunkts, immer genau 51 824 Byte
(= 102 × 508 + 8), immer mit Trailer `00 5e 01 ff`, Inhalt jedes Mal anders
und durchweg nahe null (Maximum 0x25). Er entsteht aus einem eigenen
`0x30`/`0x10`-Durchgang, bei dem der Transport nicht anläuft (keine
`12 Transport State`-Ereignisse). Das passt zu einer frisch gemessenen
**Dunkelreferenz**; bewiesen ist es nicht **[?]**. Die Klassen
`ImageCalibration`, `dm_imagecalibration.cpp` und `CDevImpl::CalibrateRun`
in `devicemanager.so`/`driver.so` stützen die Deutung **[?]**.

---

## 6. Der Interrupt-Kanal EP 0x88 — vollständig entschlüsselt

Das ist das am klarsten belegte Ergebnis dieser Analyse: Struktur **und**
Ereignisnamen stehen wörtlich in `devicemanager.so`.

### Nachrichtenstruktur **[C]**

Aus den Dumpstrings in `dm_DeviceSettings.cpp`:

```
u8EventType   = %d
u8EventId     = %d
u16Value1     = %d
u32Value2     = %d
```

1 + 1 + 2 + 4 = **8 Byte**, genau die beobachtete Nachrichtenlänge:

| Offset | Feld |
|---|---|
| 0 | `u8EventType` — der Ereigniscode (Tabelle unten) |
| 1 | `u8EventId` — Instanz/Index (bei `Page Exit` die Blattnummer) |
| 2–3 | `u16Value1` (little endian) |
| 4–7 | `u32Value2` (little endian) |

### Ereignistabelle **[C]**

Dekodiert aus `DeviceManager::s_eventtidtable` (32 Einträge à 16 Byte,
`{u8 id, …, const char* name}`), erreichbar über
`DeviceManager::GetInterruptEventName(unsigned char)`:

| Code | Name | im Mitschnitt gesehen |
|---|---|---|
| `0x00` | Dummy | — |
| `0x01` | **End of Operation** | ja |
| `0x02` | **Start of Operation** | ja |
| `0x03` | **Ding** | ja (Value1 = 1, 2) |
| `0x10` | **Power State** | ja (Value1 = 3, 4) |
| `0x11` | Buffer State | — |
| `0x12` | **Transport State** | ja (Value1 = 0, 1, 2) |
| `0x13` | **Tray State** | ja (Value1 = 1) |
| `0x14` | Udds State | — |
| `0x15` | **Lamp State** | ja |
| `0x16` | Interlock State | — |
| `0x17` | Carriage State | — |
| `0x18` | Scanner State | — |
| `0x19` | Elevator State | — |
| `0x1a` | S0 Sensor State | — |
| `0x20` | Button Press | — (siehe README: im SANE-Pfad nicht aktiv) |
| `0x30` | Paper Jam | — |
| `0x31` | Multifeed | — |
| `0x32` | Buffer Overflow | — |
| `0x33` | Print Rollover | — |
| `0x34` | Other Error | — |
| `0x40` | Notification Offset | — |
| `0x41` | **Page Exit** | ja (EventId = Blattnummer) |
| `0x42` | **Imaging Complete** | ja (Value2 = Blockzähler) |
| `0x43` | Mini Cal Paused | — |
| `0x44` | Setup Pause | — |
| `0x50` | Patch Detected | — |
| `0x51` | Patch Pause | — |
| `0x52` | DSP Reply | — |
| `0x60` | **Screen Missing** | ja (Value1 = 0x0109) |
| `0xf0` | Debug | — |
| `0xf1` | **Trace Log** | ja (Value2 = 0x0820) |

Damit sind alle im README unter „Interrupt-Meldungen auf EP 0x88"
aufgelisteten Bytefolgen benannt. Beispiele **[MC]**:

```
01 00 00 00 00 00 00 00   End of Operation
02 00 00 00 00 00 00 00   Start of Operation
03 00 01 00 00 00 00 00   Ding, Value1=1
10 00 04 00 00 00 00 00   Power State, Value1=4
12 00 01 00 00 00 00 00   Transport State, Value1=1
41 01 00 00 00 00 00 00   Page Exit, Blatt 1
41 02 01 00 00 00 00 00   Page Exit, Blatt 2, Value1=1
42 00 00 00 01 00 00 00   Imaging Complete, Block 1
42 00 00 00 02 00 00 00   Imaging Complete, Block 2
f1 00 00 00 20 08 00 00   Trace Log, Value2=2080
60 00 09 01 00 00 00 00   Screen Missing, Value1=265
01 05 02 00 02 00 00 00   End of Operation, EventId=5, Value1=2, Value2=2
```

Der Kanal wird mit `0x3a wValue=1` (`Scanner::StartInterruptEvents`)
eingeschaltet und mit `0x3a wValue=0` (`StopInterruptEvents`)
abgeschaltet — deshalb ist er nur während eines Scans aktiv. **[MC]**

Die Bedeutung von `u8EventId` jenseits von `Page Exit` und von
`u16Value1`/`u32Value2` bei den einzelnen Ereignistypen ist **nicht
geklärt [?]**.

---

## 7. Weiteres aus dem Code **[C]**

* **Der Kommando-Wortschatz zwischen `driver.so` und `device.so` ist XML,
  nicht binär.** `CDevCommandFSM::GetDevCommandFromXmlString`,
  `EDEVID_COMMAND`, `DevCommandState`. `DeviceManager::s_commandidtable`
  (29 Einträge) listet sie als Tags: `<calibratebegin>`, `<calibraterun>`,
  `<diagnosticbegin>`, `<fctbegin>`, `<getconfig>`, `<getlog>`,
  `<getoutput>`, `<getserialnumberall>`, `<imageend>`, `<interfaceclose>`,
  `<querysupport>`, `<resourcebegin>`, `<resourceend>`, `<scanbegin>`,
  `<scanend>`, `<scanrequeststop>`, `<sessionbegin>`, `<sessionend>`,
  `<setconfig>`, `<setupbegin>`, `<setupend>`, `<populatemessages>` u. a.
  Für ein SANE-Backend ist diese Schicht irrelevant — sie ist
  Treiberinterna, nicht Gerät.
* **`hippo.so` macht kein USB** (0 openusb-Symbole). Es enthält aber
  Klassen, die wie das Auftragsmodell aussehen: `Jobspec`, `Scanbegin`,
  `Createcamera`, `Windowadd`, `Setinput`, `Setconfig`, `Setbaffle`,
  `Server`, `Region`, `ImageInfo`, `Sharedsetupinfo`, dazu `Botan` (Krypto)
  und die Perfect-Page-Bildverarbeitung. Die Strings
  `ForceUsbPipeBulkIn`/`BulkOut`/`Interrupt` sind dort Konfigurationsnamen,
  keine I/O.
* **`0xe4`/`0xe5` fehlen in den pcapng-Mitschnitten, sind aber in den
  Textmitschnitten von `b` und `c` vorhanden** (4× 1024 B bzw. 8× 2048 B).
  Es sind `SetColorLookUpTable` und `SetColorCorrectionTable`. Der
  Unterschied ist reproduzierbar belegt, die Ursache nicht: plausibel ist
  ein Zwischenspeicher, da der Treiber Gerätedaten unter
  `/var/kodak/kds_i2000/data/48785942/` ablegt (`device.xml`,
  `accumulators.xml`, `i2600_48785942.hst`, 105 KB) **[?]**. Wer die
  Kalibriertabellen braucht, sollte dieses Verzeichnis vor dem Mitschnitt
  leeren.

---

## 8. Stand der offenen Fragen aus README

| # | Frage | Stand |
|---|---|---|
| 1 | Rohbildformat (Bittiefe, Zeilenlänge) | **teilweise**: 8 bit belegt, Granularität 508 Byte belegt. `FrontImageWidth = 1290` und `FrontFixedPageLength = 4650` sind jetzt bekannt (Abschnitt 4a), ihre Einheiten nicht. Entscheidender Versuch: kürzere Vorlage scannen |
| 2 | Zwei Durchgänge je Simplexseite? | **beantwortet und korrigiert**: es waren zwei *Blätter*, nicht zwei Durchgänge |
| 3 | Der 51 824-Byte-Block | **teilweise**: eigener `0x30`/`0x10`-Durchgang ohne Transport, 102 × 508 + 8 Byte, Dunkelreferenz plausibel aber unbelegt |
| 4 | Wozu EP 0x02 (Bulk OUT)? | **beantwortet**: Firmware-/Anwendungsdownload (`SendBulkOut`, `LoadFirmwareBank`, `ProgramBooter`) |
| 5 | Registerkarte `0xe0`/`0xa3` | **beantwortet für `0xa3`**: alle 170 FPGA-Register sind mit Originalnamen dekodiert (Abschnitt 4a, `captures/analysis/fpga-registermap.txt`), die Zugriffe je Mitschnitt benannt. `0xe0` = AFE- und Kameraregister, `wValue=0x1f` = `SetCameraPageValue`, `wIndex` = Kamera — die AFE-/Kamera-Innenregister sind noch nicht benannt |
| 6 | `KDS_DEBUGUSB` | **noch nicht ausprobiert**, aber der Formatstring `USBCMD: %s(reqtype=…, req=…, val=…, indx=…, datasize=…)` belegt, dass es benannte Requests liefern würde |

### Nächste Schritte, nach Nutzen sortiert

1. **Einheit von `FrontImageWidth` bestimmen.** Entweder über eine kürzere
   Vorlage (siehe unten) oder indem man das Register gezielt verändert und
   die Blockgröße beobachtet. Das schließt die letzte offene Frage zum
   Rohformat.
2. **`KDS_DEBUGUSB` einschalten** und einen Scan mit Treiberprotokoll
   mitschneiden. Das verbindet Requests, Registernamen und Zeitachse ohne
   weiteres Reverse Engineering.
3. **Kurze Vorlage scannen** (Quittung o. ä.) und Blockgrößen vergleichen.
   Klärt Frage 1. Braucht jemanden am Gerät.
4. **`/var/kodak/kds_i2000/data/48785942/` leeren** und neu mitschneiden, um
   `0xe4`/`0xe5` mit vollen Nutzdaten zu bekommen.
5. **`SetupScanner::` und `InitializeScanner::` durcharbeiten** — das ist
   die Initialisierungssequenz, die ein natives Backend nachbauen müsste.

---

## 9. Werkzeuge

Die Auswertungsskripte dieser Analyse liegen in `tools/protocol/`:

| Datei | Zweck |
|---|---|
| `usbmon.py` | pcapng-Leser für `LINKTYPE_USB_LINUX_MMAPPED`, ohne tshark |
| `ctrl.py` | paart Control-Submit/Complete zu logischen Transfers |
| `dump.py` | Vendor-Request-Strom je Mitschnitt, diff-bar |
| `inv.py` | Request-Inventar über alle Mitschnitte |
| `bulk.py` | Bulk-Transfers und -Mengen über `urb_len` |
| `trailer.py` | prüft Trailerstruktur und 508-Byte-Granularität |
| `profile.py` | Helligkeitsprofil je Bildblock |
| `corr.py` | Kreuzkorrelation der Blattprofile |
| `tl.py` | annotierte Zeitachse |
| `elftab.py` | dekodiert `{id, const char* name}`-Tabellen aus einer ELF |
| `cmds.py` | extrahiert die Opcode-Tabelle aus den `Get`/`Set`-Aufrufstellen |
| `regmap.py` | dekodiert die 170 FPGA-Registernamen und -adressen |
| `regs.py` | benennt die `0xa3`-Registerzugriffe eines Mitschnitts |

`cmds.py` braucht vorab
`objdump -d -C --no-show-raw-insn devicemanager.so > dm.dis`.
`regmap.py` und `regs.py` erwarten `devicemanager.so` unter
`driver/opt/kodak/kds_i2000/lib/` relativ zum Arbeitsverzeichnis (also das
entpackte `.deb`).

Abgelegte Ergebnisse in `captures/analysis/`:
`fpga-registermap.txt` (die 170 Register),
`*.fpga-registers.txt` (benannte Registerzugriffe je Mitschnitt),
`b-simplex.protocol-timeline.txt` (annotierte Zeitachse).
