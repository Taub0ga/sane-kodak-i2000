# Kodak i2600 — Initialisierung und Scanstart

Ergänzung zu [PROTOKOLL.md](PROTOKOLL.md). Dort stehen Kommandoformat,
Opcode-Tabelle, die 170 FPGA-Register und der Interrupt-Kanal. Hier steht die
**Reihenfolge**: welche Requests das Gerät von der Inbetriebnahme bis zum
fertigen Bild bekommt, welche Antworten ausgewertet werden und woran der
Treiber jeweils wartet.

Erstellt 2026-10-19 aus `devicemanager.so` (Treiber v04.14.00.00) und den sechs
pcapng-Mitschnitten in `captures/pcapng/`.

**Jede Aussage ist markiert** — dieselbe Systematik wie in PROTOKOLL.md:

| Marke | Bedeutung |
|---|---|
| **[M]** | am Mitschnitt verifiziert |
| **[C]** | im Code verifiziert (Symbol, Signatur, Datentabelle, Log-String) |
| **[MC]** | beides, unabhängig |
| **[?]** | Vermutung — nicht belegt |

> **Methodischer Hinweis.** `devicemanager.so` ruft `COsLog::Message(const char*
> file, long line, EOSLOGFLAG, const char* fmt, …)` auf. Der Quelldateiname
> steht in `%rsi`, die **Zeilennummer in `%edx`**. Damit lässt sich die
> *Quellreihenfolge* einer Funktion rekonstruieren — die Adressreihenfolge im
> optimierten Binary ist sie nicht, GCC verschiebt Fehlerpfade ans Ende. Alle
> Ablaufangaben unten stammen aus dieser Rekonstruktion
> (`tools/protocol/dmtrace.py … --src`). **[C]**

---

## 1. Wo die Sequenz liegt

`devicemanager.so` ist eine **Zustandsmaschine mit zwei Eingängen** **[C]**:

```
Scanner::ProcessCommand(COMMAND)     <- die XML-Kommandos von driver.so
Scanner::ProcessEvent(EVENT)         <- interne Ereignisse + Interrupt-Ereignisse
Scanner::ProcessInterruptEvent(SInterruptEvent*)  -> ProcessEvent
```

`ProcessEvent` ist der einzige Aufrufer von `Scanner::Initialize()`,
`Scanner::Setup()` und `Scanner::CleanupScan()`. **[C]** Die eigentliche
Inbetriebnahme läuft in einem eigenen Thread (`OsThreadProcInitializeScanner`),
der `InitializeScanner::InitializeDevice()` aufruft. **[C]**

Die Zustände (`DeviceManager::s_stateidtable`, 14 Einträge) **[C]**:

| Wert | Zustand | | Wert | Zustand |
|---|---|---|---|---|
| 0 | Error | | 7 | ResetForBaffleCapture |
| 1 | Disconnected | | 8 | Calibrating |
| 2 | NotReady | | 9 | Scanning |
| 3 | Initializing | | 10 | PausePending |
| 4 | Idle | | 11 | Paused |
| 5 | SettingUpForScan | | 12 | WaitingOnQEmpty |
| 6 | ScanningBaffles | | 13 | WaitingOnGetOutput |

Die Ereignisse (`DeviceManager::s_dmeventidtable`, 29 Einträge) **[C]**:

| Wert | Ereignis | | Wert | Ereignis |
|---|---|---|---|---|
| 0 | RequestInit → `Scanner::Initialize()` | | 0x12 | MultifeedEndOfJobLeavePaper |
| 1 | Disconnect | | 0x13 | MultifeedEndOfJobGenerateImage |
| 2 | Reconnect | | 0x14 | TransportTimeout |
| 3 | LowPower | | 0x15 | OtherError |
| 4 | InitSucceeded | | 0x16 | Stop |
| 5 | InitFailed | | 0x17 | StartResume |
| 6 | CriticalError | | 0x18 | PauseStop |
| 7 | SetupScanner → `Scanner::Setup()` | | 0x19 | PauseSucceeded |
| 8 | SetupSucceeded | | 0x1a | PauseFailed |
| 9 | SetupFailed | | 0x1b | ImagingComplete |
| 0x0a | End | | 0x1c | LampsDim |
| 0x0b | QEmpty | | 0x1d | LampsOff |
| 0x0c | EndOfOperation | | | |
| 0x0d | PaperJam | | | |
| 0x0f–0x11 | MultifeedContinue / …StopFeeder / …EndOfJob | | | |

Die Rückgabewerte aller DeviceManager-Funktionen sind `EDEVMGRSTS`
(`DeviceManager::s_dmstatustable`, 23 Einträge) **[C]** — relevant, weil
`IsOkToStartScan` sie als Fehlergrund zurückgibt (Abschnitt 9):

```
0 Success          6 CoverOpen             0x0c LampsWarming      0x12 NotComplete
1 Fail             7 CriticalError         0x0d LastCameraImage   0x13 NoWhitePatchFound
2 AdfEmpty         8 Disconnected          0x0e Multifeed         0x14 PaperJam
3 Busy             9 EcdoCustomLearnFail   0x0f NoFeederFound     0x15 Timeout
4 CalibrationRequired  0x0a EndOfOutput    0x10 NoImageReady      0x16 PoweredOff
5 CleanGuides      0x0b InvalidState       0x11 NoPrintHeadFound
```

### Das Gerätemodell steht als XML im Binary

`devicemanager.so` trägt die Familienkonstanten als `<devicemanagerdata>`-Block
im `.rodata`; `Data::GetInt("…")`/`GetString`/`GetBoolean` lesen daraus. Für die
Sequenz wichtige Einträge **[C]**:

```xml
<string name='devicesettingstype'>walleye</string>   <!-- -> FpgaWalleye / dm_FpgaWalleye.cpp -->
<string name='family'>i2000</string>
<string name='fullpowerstate'>operational</string>
<int name='numberofinterruptendpoints'>2</int>
<bool name='useimagingcomplete'>true</bool>
<int name='documentcountsize'>4</int>
<int name='max_scan_width'>10320</int>               <!-- 1200-dpi-Pixel -->
<int name='max_scan_pixels'>5336</int>
<int name='max_sheet_width'>10200</int>
<int name='max_sheet_height'>192000</int>
<int name='overscan_height_1200dpi'>1800</int>
<int name='image_footer_bytes'>2400</int>
<int name='extra_image_padding_bytes'>65535</int>
<int name='white_patch_left_limit_front'>1326</int>
<int name='fpga_red_rephase_300_dpi'>0x0D</int>      <!-- u. a.; siehe Abschnitt 7 -->
<int name='fpga_green_rephase_300_dpi'>0x07</int>
<int name='fpga_red_rephase_300_dpi_DT'>0x07</int>
<int name='fpga_green_rephase_300_dpi_DT'>0x04</int>
<int name='fpga_blue_line_buffer'>0x28</int>
<int name='optical_black_aim_value'>20</int>
<int name='end_of_operation_timeout'>5000</int>
```

Der komplette Block steht in `tools/protocol/` nicht als Datei, sondern
lässt sich mit `strings -a devicemanager.so | grep -A60 '<devicemanagerdata>'`
ausgeben.

**Wichtig für ein Backend:** `devicesettingstype = walleye`. Der i2600 benutzt
also den `FpgaWalleye`/`DeviceSettings`-Basispfad, **nicht** `DeviceSettingsA4`
(Flachbett) und **nicht** `DeviceSettingsP2`. Alles, was unten mit `A4::` oder
`P2::` markiert ist, gehört **nicht** zum i2600. **[C]**

---

## 2. Die zwei Adressräume hinter `0xa3` und `0xe0`

PROTOKOLL.md Abschnitt 4a hat `0xa3` als FPGA-Registerzugriff identifiziert.
Der Herstellername dazu ist **`USB_ACCESS_EXTERNAL_MEM`** (Abschnitt 3) — es
ist also kein Registerkommando, sondern ein **generischer Zugriff auf den
externen Speicherbus** des Scanner-Controllers, in dem die FPGA-Register bei
`0xd7xx`–`0xdexx` liegen. **[C]** Das erklärt, warum derselbe Opcode auch
`LoadFirmwareBank` bedient.

`0xe0` ist dagegen ein **serieller Seitenzugriff auf zwei andere Bausteine**.

### Format von `0xe0` **[MC]**

```
bmRequestType 0x40 / 0xC0
bRequest      0xe0
wValue        Registeradresse innerhalb der aktiven Seite
wIndex        Geräteindex (0 = Front/Master, 1 = Rear/Slave)
wLength       1, 3 oder 32
```

Belegt durch die Argumentzuordnung der vier Zugriffsfunktionen **[C]**
(`DeviceSettings::Get/Set(bRequest, wValue, wIndex, cbData, pData)`):

```c
ReadAfeDevice (u8 afe,   u16 addr, u8* data)
    -> Get(0xe0, wValue=addr, wIndex=afe, cbData=1, data)

WriteAfeDevice(u8 afe,   u16 addr, u8 data)
    -> Set(0xe0, wValue=addr, wIndex=afe, cbData=1, &data)
       Log: "USBCMD: WriteAfeDevice(afe=%x,addr=%x,data=%x)"

SetCameraPageValue(u16 camera, u8 page)
    -> Set(0xe0, wValue=0x1f, wIndex=camera, cbData=1, &page)

ReadCameraDevice (u16 camera, u8 page, u16 addr, u8* data, u16 len)
    -> SetCameraPageValue(camera, page)
    -> Get(0xe0, wValue=addr, wIndex=camera, cbData=len, data)

WriteCameraDevice(u16 camera, u8 page, u16 addr, u8* data, u16 len)
    -> SetCameraPageValue(camera, page)                        // Seite schreiben
    -> ReadCameraDevice(camera, page, 0x1f, &tmp, 1)           // -> schreibt die Seite nochmals
    -> Get(0xe0, wValue=0x1f, wIndex=camera, cbData=1, &tmp)   //    und liest sie zurück
    -> Set(0xe0, wValue=addr, wIndex=camera, cbData=len, data)
```

Damit ist das Muster aus PROTOKOLL.md („schreiben, schreiben, lesen, dann
Zielregister") **vollständig erklärt**: `WriteCameraDevice` setzt die Seite,
ruft dann `ReadCameraDevice` zum Zurücklesen auf, und *das* setzt die Seite ein
zweites Mal, bevor es Register `0x1f` liest. Erst danach kommt der Zugriff auf
das Zielregister. **[MC]**

### 2a. Das Kameragerät — Seiten und Register

**Aufgabe 1 dieser Session.** Ergebnis in Kurzform:

* Das Gerät hinter `0xe0`/`wIndex` ist der **Kameracontroller des jeweiligen
  Kamerakopfs** (Front = „master", Rear = „slave"). Es hat **7 Seiten (0–6) mit
  je 32 Ein-Byte-Registern**, Register `0x1f` ist jeder Seite gemeinsam und
  dient als **Seitenwahl**. **[MC]**
* **`devicemanager.so` enthält keine Namen und keine Namenskonstanten für diese
  Register** — anders als bei den FPGA-Registern gibt es keine
  `DeviceManager::…Register::<Name>`-Symbole im `.rodata`. Die Methode aus
  PROTOKOLL.md Abschnitt 4a (2-Byte-Konstanten über `nm -SC` plus
  Sektionsabbildung) liefert für `0xe0` **nichts**; alle 220 `r`/`R`-Symbole
  sind geprüft. **[C]**
* **Eine Typbezeichnung des Bausteins steht nicht im Binary.** Gesucht wurde in
  allen 48 084 Strings nach den üblichen AFE-/CCD-Familien (AD9xxx, LM98xx,
  AFExxxx, VSPxxxx, WM8xxx, ADSxxxx, TCD/ILX/ICX/CXD) — **kein Treffer**. Was
  man über den Baustein sagen kann, ist nur, was die Zugriffe zeigen. **[C]**

Was der Treiber tatsächlich benutzt — aus allen Aufrufstellen von
`Read/WriteCameraDevice` in `devicemanager.so` **[C]**, Spalte „Mitschnitt" aus
`captures/analysis/camera-afe-registers.txt` **[M]**:

| Seite | Register | Länge | Aufrufstelle | Wert(e) | Mitschnitt |
|---|---|---|---|---|---|
| 0 | `0x00`–`0x1f` | 32 | `SetupScanner::SetCameraResolution()` | Tabellenblock | ja |
| 0 | `0x02` | 1 | `SetAfeStandby()` | `0x20` | ja |
| 0 | `0x02` | 1 | `SetCameraResolution()` | `0x20` (am Anfang), dann `0x28`, `0x08`, `0x19` | ja |
| 1 | `0x00`–`0x1f` | 32 | `SetCameraResolution()` | Tabellenblock | ja |
| 1 | `0x00` | 3 | `WriteAllAFEGains(u8 cam, u8* gains, u8 n)` | `00 a7 a7` / `00 aa aa` | ja |
| 1 | `0x00` | 3 | `ReadAllAFEGains(…)` (Gegenprobe) | dito | ja |
| 1 | var. | 1 | `SetupScanner::WriteAFEGain(u8,u8,u8*)` | — | nein |
| 2 | `0x00`–`0x1f` | 32 | `SetCameraResolution()` | Tabellenblock | ja |
| 2 | `0x05` | 1 | `SetAfeStandby()` | `0x00` | ja |
| 3 | `0x00`–`0x1f` | 32 | `SetCameraResolution()` | Tabellenblock | ja |
| 3 | `0x00`–`0x04` | 1 | `SetAfeStandby()` | je `0x88` | ja |
| 4 | `0x00`–`0x1f` | 32 | `SetCameraResolution()` | Tabellenblock | ja |
| 5 | `0x00`–`0x1f` | 32 | `SetCameraResolution()` | Tabellenblock | ja |
| 6 | `0x01` | 1 | `SetCameraResolution()` | `0x22` | ja |
| — | `0x1f` | 1 | `SetCameraPageValue()` | Seitennummer | ja, vor jedem Zugriff |

**Die Verstärkung sitzt in Seite 1.** `WriteAllAFEGains(camera, gains, count)`
und `ReadAllAFEGains` schreiben bzw. lesen `count` Bytes ab Seite 1
Register 0, im Mitschnitt `count = 3` (drei Kanäle). Die statischen Tabellen
belegen Seite 1 Register 0–4 mit `54 54 54 54 54`, es gibt also **fünf**
Verstärkungsregister, von denen der Treiber drei benutzt. Der Scan schreibt
`00 a7 a7` (Front) bzw. `00 aa aa` (Rear) und liest dasselbe zurück. **[MC]**
Der erste Kanal auf 0 passt dazu, dass der i2600 im Graustufenpfad nur den
RedGs-Kanal braucht — belegt ist das nicht **[?]**.

### Die sechs Seiten kommen aus statischen Tabellen — byte-exakt bestätigt

`devicemanager.so` enthält **zwölf 192-Byte-Tabellen** (= 6 Seiten × 32
Register) mit Originalnamen **[C]**:

```
s_dmcamera{color,gs}{300,600}{master,slave}      s_dmcamera…masterDT / …slaveDT
```

`SetupScanner::SetCameraResolution()` wählt eine davon und schreibt Seite 0..5
in sechs 32-Byte-Transfers, zuerst für Kamera 1 (slave), dann Kamera 0
(master). Die Auswahl **[C]**:

| `*(u32*)(Data+0x40c4) == 3` | `m_ImageFormat == 2` (Farbe) | Tabelle |
|---|---|---|
| ja | ja | `s_dmcameracolor300{master,slave}` |
| ja | nein | `s_dmcamerags300{master,slave}` |
| nein | ja | `s_dmcameracolor300{master,slave}DT` |
| nein | nein | `s_dmcamerags300{master,slave}DT` |

bei Auflösung 400 oder 600 dpi entsprechend die `…600…`-Tabellen.

**Die Gegenprobe ist exakt**: die 192 Byte, die der Treiber im Mitschnitt an
Kamera 0 schreibt, sind **byteidentisch** mit `s_dmcamerags300masterDT`, die an
Kamera 1 mit `s_dmcamerags300slaveDT` — **0 abweichende Bytes von 192**, in
allen vier Scanmitschnitten, beide Kameras
(`captures/analysis/camera-pages.txt`, `tools/protocol/camtab.py`). **[MC]**

Master- und Slave-Tabelle unterscheiden sich in **genau einem Byte**: Seite 2
Register 0 (`0x70` master, `0x30` slave). **[C]**

Was `Data+0x40c4` ist, bleibt offen **[?]**: geschrieben wird es nur von
`DeviceSettingsA4::` und `DeviceSettingsP2::PrepareQuerySupportResponse` (mit 3
bzw. 4), für den walleye-Pfad also nie — der Wert ist damit ≠ 3, was zur
gemessenen `…DT`-Tabelle passt. **[MC]** Wofür „DT" steht, ist **nicht
belegt [?]**; die ini-Schlüssel `fpga_red_rephase_300_dpi_DT` zeigen, dass es
eine durchgehende Variantenachse der i2000-Familie ist.

Die Tabellen selbst liegen dekodiert in
`captures/analysis/camera-page-tables.txt`.

### 2b. Die „AFE"-Register `0x20`–`0x2a` gehören **nicht** zum i2600

`ReadAfeDevice`/`WriteAfeDevice` sprechen einen **zweiten, getrennten
Adressraum** auf demselben Opcode an. Dort liegen je Kanal drei Register **[C]**:

| Kanal | Offset | Gain MSB | Gain LSB |
|---|---|---|---|
| 0 | `0x20` | `0x28` | `0x24` |
| 1 | `0x21` | `0x29` | `0x25` |
| 2 | `0x22` | `0x2a` | `0x26` |

`SetAfeGainOffset(u8 afe, EDMCAMERA_CHANNEL ch, u8 offset, u16 gain)` zerlegt
`gain` in `gain >> 1` (MSB-Register) und `gain & 1` (LSB-Register) — die
Verstärkung ist also **9 bit**, verteilt auf zwei Register — schreibt beide plus
das Offsetregister und **liest alle drei zur Kontrolle zurück**. **[C]**

Die Kanalzuordnung erfolgt über Bitmasken auf `1 << ch` **[C]**:
`0x560` → Registertripel 0, `0xa8c` → Tripel 1, `0x12` → Tripel 2;
`ch == 0` ist ungültig (Ausnahme „ScannerOperation invalid operation").

`EDMCAMERA_CHANNEL` ist aus der Namensmap im `DeviceSettings`-Konstruktor
dekodiert und über `ImageCalibration::CalcAFEGains` unabhängig bestätigt
(dort stehen die Konstanten 5/3/1/2 direkt neben den Log-Strings
„red"/„green"/„blue"/„even gray") **[C]**:

| Wert | Kanal | Tripel | | Wert | Kanal | Tripel |
|---|---|---|---|---|---|---|
| 1 | blue | 2 | | 7 | odd red | 1 |
| 2 | even gray | 1 | | 8 | even green | 0 |
| 3 | green | 1 | | 9 | odd green | 1 |
| 4 | odd gray | 2 | | 10 | even blue | 0 |
| 5 | red | 0 | | 11 | odd blue | 1 |
| 6 | even red | 0 | | | | |

**Für den i2600 ist dieser Pfad toter Code.** Beides unabhängig:

* Alle Aufrufer von `SetAfeGainOffset`, `GetAfeGainOffset`, `AdjustAfeGain` und
  `AdjustAfeOffset` liegen in `DeviceSettingsA4::SetChannelMinOBValue` und
  `ImageCalibrationA4::CalcAFEChannelValues`, also im **Flachbettpfad**. Die
  einzige weitere Schreibstelle der Register `0x01`–`0x08` und `0x20`–`0x2a` ist
  `DeviceSettingsA4::SetupAFE`. **[C]**
* In keinem der sechs Mitschnitte kommt ein `0xe0` mit `wValue` zwischen `0x20`
  und `0x2a` vor; beobachtet werden ausschließlich `wValue` ∈ {`0x00`, `0x01`,
  `0x02`, `0x05`, `0x1f`}. **[M]**

Der i2600 erreicht seine AFE also **über Seite 1 des Kameragerätes**, nicht über
diesen Adressraum.

---

## 3. Die Opcode-Namen des Herstellers

`DeviceSettingsP2::GetSimulatedData` enthält eine 256-Einträge-Sprungtabelle
über `bRequest` mit Log-Strings `"Simulating USBCMD: USB_…"`. Das sind die
**Originalnamen der Opcodes**. **[C]** Dekodiert aus der Sprungtabelle bei
`0x289c8c`:

| bReq | Name | | bReq | Name |
|---|---|---|---|---|
| `0x00` | `USB_GET_STATUS` | | `0x37` | `USB_VRAM_DATA` |
| `0x02` | `USB_GET_FW_VERSIONS` | | `0x38` | `USB_ELEVATOR_LOAD_POSITION` |
| `0x03` | `USB_GET_MACHINE_CONSTANTS` | | `0x3a` | `USB_INTERRUPT_EVENT_CONTROL` |
| `0x09` | `USB_GET_EVENT_LOG` | | `0x3b` | `USB_BACKGROUND` |
| `0x0a` | `USB_TRACE_LOG` | | `0x3f` | `USB_TRANSPORT_COMMAND` |
| `0x10` | `USB_START_STOP` | | `0x40` | `USB_SUBSYSTEM_COMMAND` |
| `0x11` | **`USB_SET_LAMP`** | | `0x41` | `USB_CAMERA_COMMAND` |
| `0x16` | `USB_SET_SEQUENCE_NUMBER` | | `0x43` | `USB_DSP_COMMAND` |
| `0x18` | `USB_SET_AUTOWHITE` | | `0x44` | `USB_PDMD_LED_COMMAND` |
| `0x1f` | `USB_SET_TIME` | | `0xa3` | **`USB_ACCESS_EXTERNAL_MEM`** |
| `0x31` | `USB_ENERGY_STAR` | | `0xe3` | `USB_ACCESS_FLASH` |
| `0x32` | `USB_LAMP_TIMEOUT` | | `0xe4` | `USB_ACCESS_COLOR_LUT` |
| `0x33` | `USB_METERS` | | `0xe5` | `USB_ACCESS_TETRA_SRAM` |
| `0x34` | `USB_EOL_CONFIGURATION` | | `0xe7` | `USB_READ_WRITE_DSP0` |
| `0x35` | `USB_NVRAM_DATA` | | `0xe8` | `USB_READ_WRITE_DSP1` |
| `0x36` | `USB_SERIAL_NUMBER` | | | |

Die Namen stimmen über 20 Opcodes mit der aus den walleye-Aufrufstellen
hergeleiteten Tabelle in PROTOKOLL.md überein — eine unabhängige Gegenprobe.
**Zwei Einträge weichen ab**, weil die Tabelle aus dem **P2**-Pfad stammt und
Opcodes familienweise mehrfach belegt sind: `0x37` ist im walleye-Pfad
`Get/SetSessionInfo` (nicht `VRAM_DATA`), `0xe5` ist dort
`SetColorCorrectionTable` (nicht `TETRA_SRAM`). Beide bleiben also wie in
PROTOKOLL.md benannt. **[C]**

### Korrekturen zu PROTOKOLL.md

Drei Punkte sind damit entschieden:

1. **`0x11` ist die Lampensteuerung.** Zwei unabhängige Belege: der Name
   `USB_SET_LAMP`, und `DeviceSettings::TurnLampOn()`/`TurnLampOff()` rufen
   `ScannerOperation(5)` bzw. `ScannerOperation(4)` auf, was laut
   `_ScannerOp`-Tabelle (Abschnitt 4) genau `0x11 wValue=1` bzw. `0x11
   wValue=0` erzeugt. Im Mitschnitt: `40 11 val=0000` am Ende der
   Initialisierung, `40 11 val=0001` zweimal vor dem Scan, `40 11 val=0000`
   beim Abbau. **[MC]**
2. **`0x1b` ist `ScannerOperation(0x0d)`** und wird von
   `Scanner::StartInterruptEvents()` ausgelöst. Im Mitschnitt steht `40 1b`
   unmittelbar nach `40 3a`. **[MC]** *Was* die Operation bewirkt, ist weiter
   **nicht belegt [?]** — sie hat keinen eigenen Log-String.
3. **Bei `0x3a` ist `wIndex` der Ein/Aus-Schalter, nicht `wValue`.** Der Code
   ist eindeutig **[C]**:
   ```
   StartInterruptEvents: Set(0x3a, wValue=<Flag>, wIndex=1, 0, NULL)
   StopInterruptEvents:  Set(0x3a, wValue=<Flag>, wIndex=0, 0, NULL)
   ```
   und der Mitschnitt bestätigt es **[M]**: Start `40 3a val=0001 idx=0001`,
   Stop `40 3a val=0001 idx=0000` — `wValue` ist in beiden Fällen 1.
   PROTOKOLL.md Abschnitt 6 („mit `0x3a wValue=1` ein- und `wValue=0`
   ausgeschaltet") ist damit **zu korrigieren**. Die Bedeutung von `wValue`
   bleibt offen **[?]**; in `StopInterruptEvents` wird dort
   `(Data+0x2aa3 == 0)` eingesetzt.

---

## 4. `ScannerOperation` — ein Opcode-Multiplexer

`DeviceSettings::ScannerOperation(_ScannerOp)` ist die Funktion, über die der
größte Teil der Maschinensteuerung läuft. Sie ist ein `switch` über 27
Operationen, der jeweils `bRequest`, `wValue` und `wIndex` setzt und mit
`wLength = 0` sendet. Vollständig dekodiert aus der Sprungtabelle bei
`0x288e74` **[C]**:

| Op | bReq | wValue | wIndex | Benutzt von | Mitschnitt |
|---|---|---|---|---|---|
| `0x00` | `0x10` | 1 | 0 | `Scanner::StartScanner()` | **ja, Scanstart** |
| `0x01` | `0x10` | 0 | 0 | `CleanupScan()` („Halt scanner") | **ja, Scanstop** |
| `0x02` | `0x10` | 0 | 1 | `CleanupScan()` („Cancel IO") | **ja, am Ende** |
| `0x03` | `0x10` | 0 | 2 | — | nein |
| `0x04` | `0x11` | 0 | 0 | `TurnLampOff()` | **ja** |
| `0x05` | `0x11` | 1 | 0 | `TurnLampOn()` | **ja** |
| `0x06` | `0x11` | 1 | 1 | — | nein |
| `0x07`/`0x08` | `0x12` | 0 / 1 | 0 | — | nein |
| `0x09` | `0x14` | 0 | 0 | — | nein |
| `0x0a`/`0x0b` | `0x15` | 0 / 1 | 0 | — | nein |
| `0x0c` | `0x1a` | 0 | 0 | — | nein |
| `0x0d` | `0x1b` | 0 | 0 | `StartInterruptEvents()` | **ja** |
| `0x0e`–`0x10` | `0x1c` | 1 / 2 / 3 | 0 | — | nein |
| `0x11`/`0x12` | `0x20` | 1 / 0 | 0 | Flachbett | nein |
| `0x13` | `0x21` | 0 | 0 | — | nein |
| `0x14` | `0x22` | 0 | 2 | — | nein |
| `0x15`–`0x17` | `0x39` | 0 / 1 / 2 | 0 | — | nein |
| `0x18` | `0xe3` | `0x0100` | 0 | — | nein |
| `0x19`/`0x1a` | `0x19` | 0 / 1 | 0 | — | nein |

`StartCapture(u16)` ist dagegen eigenständig: `Set(0x17, wValue=arg, 0, 0,
NULL)`. **[C]**

---

## 5. Inbetriebnahme: `InitializeScanner::InitializeDevice()`

Läuft einmal je Sitzung im Thread `OsThreadProcInitializeScanner`.
Rekonstruiert aus den Quellzeilennummern von `dm_initializescanner.cpp`
(Zeile 133 bis 403) plus den Aufrufstellen. **[C]**

```
L133  >>> InitializeDevice()
      Scanner::Lock()                                   (Ressourcensperre, Ioctl 0x23)
      InitializeScanner::IsScannerHealthy()
         -> Scanner::ProcessEvent(...)
      if (!healthy):
L165     StopInterruptEvents()              "Failed to stop the interrupt events"
L174     Scanner::Unlock()                  "Failed to unlock the device"
L177     <<< return
L189  "Scanner health passed...check if booter update is needed"
      InitializeScanner::IsBooterUpdateNeeded()
L196  "Getting the batch data."             -> vtable +0xa0  (DeviceSettingsP2::GetBatchData, 0x45)
L200  "Setting batch state to END."         -> vtable +0x98
L212  DeviceSettings::GetStatus()            (0x00 IN, 32 B)   "Failed to get scanner status"
L217  "Raising Power level to normal"       -> vtable +0x70
L221                                        "Unable to raise power level"
L228  WaitForPowerState(...)                "Failed to bring the device out of energy star"
L241  "Scanner is in an active power state, send a Stop to get to a known state"
                                            -> vtable +0x60 = ScannerOperation(Stop)
L244  "Wait for scanner to return back to fullPowerState in 5 seconds"
      WaitForPowerState(state, 5000, &out)
L249  "… did not succeed, try Halt"         -> ScannerOperation(Halt)
      Scanner::PurgeScannerBuffers()
L259  "Getting the batch data."
L263  "Setting batch state to END."
L290  GetStatus()                           "Failed to get scanner status"
L296  InitializeScanner::LoadFirmware(bool) "Failed to bring the device into an operational state"
      Scanner::GetPowerState()
      InitializeScanner::CheckAllFirmwareVersions(u8)
L315  Scanner::StartInterruptEvents()       "Failed to start the interrupt events"
L328  DeviceSettings::TurnLampOn()          "Failed to turn on the lamps"       -> 0x11 val=1
L341  "Had to skip homing the camera because we don't have interrupt events started"
L345  Scanner::HomeCamera(true)             "Failed to home the camera"
      InitializeScanner::SaveDeviceInfo()
L353  DeviceSettings::TurnLampOff()         "Failed to turn off the lamps"      -> 0x11 val=0
L363  "DisableLowPower alteration found...DisableLowPowerSettings"
      DeviceSettings::DisableLowPowerSettings()
L376  "Overriding default lowpowermodetimeout of 15 to 14"
L387                                        "Failed to verify lowpowermode"
L399  Scanner::Unlock()                     "Failed to unlock the device"
L403  <<< InitializeDevice()
```

Vorbedingung `fullpowerstate`: `Data::GetString("fullpowerstate")` wird mit
`strcasecmp` gegen `"normal"` geprüft; für die i2000-Familie steht dort
`operational`. **[C]**

**Das passt auf den Mitschnitt** (`b-simplex-lineart-200`, Zeiten relativ)
**[M]**:

```
5.231  C0 00  GetStatus (3x)                       <- L212/L290
5.234  C0 02  GetFirmwareVersions                  <- CheckAllFirmwareVersions
5.235  C0 34  GetEndOfLine
5.245  C0 36  GetBoardSerialNumber
5.249  C0 37  GetSessionInfo
5.250  40 1f  SetTime
5.252  C0 03  GetMachineConstants
5.253  C0 35  GetNvRamData
5.271  40 a3  da27/db27 = 00 00 00 01              <- SetDocumentIds
5.273  C0 e3  ReadFlashMemory
5.289  40 a3  d901 lesen/schreiben
5.311  40 e0  SetAfeStandby, Kamera 0 dann 1       <- s. u.
5.402  C0 45  GetBatchData                         <- L196
5.402  40 11  val=0  Lampe aus                     <- L353
5.439  40 62  768-Byte-Block
5.448  40 35  SetNvRamData                         <- SaveDeviceInfo
5.592  C0 33  GetMeters
5.741  40 62  768-Byte-Block (2. Mal)
```

Das lange `0xe0`-Bündel bei 5.311 ist **`DeviceSettings::SetAfeStandby()`** —
die Übereinstimmung ist exakt **[MC]**: Seite 0 Register `0x02` ← `0x20`,
Seite 2 Register `0x05` ← `0x00`, Seite 3 Register `0x00`–`0x04` ← je `0x88`,
zuerst Kamera 0, dann Kamera 1. Genau diese neun Schreibzugriffe stehen als
`WriteCameraDevice`-Aufrufe in `SetAfeStandby`, dazu
`FpgaBase::Get/SetCommonGeneralControl` und `COsTime::Sleep(20)` zwischen den
Schritten. Die Log-Strings dort benennen auch die Bits von FPGA-Register
`0xd901`: *„D901 - Enabling the test pattern bit"*, *„D901 - Disabling the test
pattern bit"*, *„D901 - Disabling the camera enable bit"*. **[C]**

Dieselbe Sequenz läuft am Sitzungsende noch einmal (11.361 im Mitschnitt) —
`SetAfeStandby` ist also sowohl Teil der Inbetriebnahme als auch des Abbaus.
**[M]**

### `Scanner::StartInterruptEvents()`

```
L1421 >>> StartInterruptEvents
      Data::GetInt("numberofinterruptendpoints")        (= 2)
L1429/L1430  "USBCMD: Start Interrupts"
      DeviceSettings::Set(0x3a, wValue=<Flag>, wIndex=1, 0, NULL)
L1442 "USBCMD: Complete"
L1446                                   "Failed to start interrupts."
      DeviceSettings::UsbIoctl(6, …, 0x80)              Ioctl 6 = INTERRUPT lesen starten
      ScannerOperation(0x0d)                            -> 0x1b
      COsSync::EventWait(5000)          auf das erste Interrupt-Ereignis („ring")
L1481                                   "Failed: Unable to enable the interrupt events"
L1496                                   "Error waiting for a ring from the device: %d"
      -> bei Fehler StopInterruptEvents()
L1510 <<< StartInterruptEvents
```
**[C]**, und im Mitschnitt genau in dieser Folge: `40 3a idx=1` (5.772),
`40 1b` (5.773), erstes EP-0x88-Ereignis `01 End of Operation` (5.776). **[M]**

`StopInterruptEvents()` spiegelt das: Ioctl `0x0c` (*„USBCMD: CancelIO
Interrupt"*, mit 5-s-EventWait in der OS-Schicht), dann
`Set(0x3a, …, wIndex=0, …)`. **[C]**

---

## 6. Scanstart: `Scanner::Setup()`

Ausgelöst von `ProcessEvent(EVENT = 7 SetupScanner)`, also indirekt vom
XML-Kommando `<scanbegin>` (`s_commandidtable[0x14]`) über
`Scanner::ProcessCommand`. **[C]**

```
Scanner::Setup():
  GetCurrentState()                            Zustand muss passen
  new SetupScannerA4(Data&, DeviceSettings&)   (Objekt wird angelegt, Pfad ungenutzt)
  Scanner::SetDocumentIds()                    -> 0xa3 auf da27/db27 (DocId)
  new SetupScanner(Data&, DeviceSettings&)
  DeviceSettings::GetBackgroundColor()
  --- Referenzdurchgang ---------------------------------------------------
  SetupScanner::ConfigScanner(scanType, …)     Referenz-/Shutter-Konfiguration
  Scanner::ProcessEvent(...)
  Scanner::GetMaxChunkSize(camera 0) / (camera 1)
  delete/new ImageManager(Scanner&, Data&, …)  Bildpuffer anlegen
  Scanner::StartScanner()                      -> 0x10 val=1
  SetupScanner::ConfigScanner(...)
  Scanner::SetCurrentState(STATE)
  COsSync::EventReset(); Scanner::StartScanner(); EventWait(...); EventReset()
  Scanner::CaptureImage(camera, …)             Dunkelreferenz einlesen
  Image::GetHeightInPixels()
  Scanner::ResetBaffleCapture()
  DeviceSettings::StartCapture(1)              -> 0x17 val=1
  Scanner::CaptureImage(camera, …)
  Image::GetBufferChunk(...)
  Scanner::HomeCamera(bool)
  Scanner::WaitForState(STATE, timeout)
  Scanner::ProcessEvent(...)
  FpgaBase::SetFrontValidPixelLevelThreshold(u8)
  FpgaBase::SetRearValidPixelLevelThreshold(u8)
  --- Scandurchgang -------------------------------------------------------
  SetupScanner::ConfigScanner(...)             Scan-Konfiguration
  COsSync::EventWait(...); EventReset()
  Image::GetBufferChunk(...)
```
**[C]**

Das deckt sich mit der Zeitachse aus PROTOKOLL.md Abschnitt 4: ein
`0x30`(Setup) → `0x10 val=1`(Start) → optional `0x17`(StartCapture) →
Bildblöcke → `0x10 val=0`(Stop), und das zweimal — einmal im Shutter-, einmal
im Normalmodus. **[M]** Dass die beiden Durchgänge durch
`CaptureMode = shutter` bzw. `Normal` im `0x30`-Block unterschieden werden,
belegt Abschnitt 8.

---

## 7. `SetupScanner::ConfigScanner()` — die Registerprogrammierung

Quellreihenfolge aus `dm_setupscanner.cpp` Zeile 469–647. **[C]**

```
L469  >>> ConfigScanner(scanType, bool)
      DeviceSettings::GetSetupOverride()
      SetupScanner::DetermineRulesFormatResolution()
          Data::GetWindow(WINDOWTYPE, CAMERATYPE)  (4x)
          GetFrontWindow() / GetRearWindow()
          UserSelectedECDO() / UserSelectedAutoColorDetect()
          SetupScanner::GetWindowSpeed(window, format)
              -> m_Resolution (this+0x240)   Geräte-Scanauflösung
              -> m_ImageFormat (this+0x244)
      SetupScanner::CalculateMaxImageSize()     Hostpufferbedarf, s. u.
L486  "SetupColorTables() Start"
      SetupScanner::SetupColorTables(DOCUMENTTYPE)
          -> LoadColorTableData(), SetupColorTables(CAMERATYPE, DOCUMENTTYPE)
          -> 0xe4 SetColorLookUpTable / 0xe5 SetColorCorrectionTable
L491                                        "Failed to load the color tables"
L493  "SetupColorTables() End"
      SetupScanner::SetupCommonImageChain()   gemeinsame d9xx-Register, s. u.
L511                                         "Failed to setup Common Image Chain"
      SetupScanner::SetCameraResolution()     Kameraseiten 0..5 + Einzelregister
L529                                         "Failed to set the camera resolution"
      DeviceSettings::SetupAFE(int, IMAGEFORMAT)
          -> WriteAllAFEGains(camera, gains, 3)   Kameraseite 1
L544                                         "Failed to setup the AFEs"
      SetupScanner::SetupFPGA(CAMERATYPE=front, scanType)
      SetupScanner::SetupFPGA(CAMERATYPE=rear,  scanType)
      FpgaBase::SetCommonGeneralControl(u8); COsTime::Sleep(100)
      SetupScanner::VerifyWhitePatchTracking(front) / (rear)
      FpgaBase::SetupUdds(MULTIFEEDUDDSSENSITIVITY, bool)
      SetupScanner::SetupScannerConfig(scanType)
      CScannerConfiguration::Set()            -> 0x30, 120 Byte
L633                                         "Failed to write the scanner configuration to the scanner"
L647  <<< ConfigScanner
```

### `SetupCommonImageChain()` — die `0xd9xx`-Register

Jeder Schreibzugriff hat einen Log-String mit Registeradresse. **[C]**

| Register | Setter | Quelle des Werts | Mitschnitt **[M]** |
|---|---|---|---|
| `0xd900` `CommonIP_Control` | `SetCommonIpControl` | „Common Image Process Control (D900)" | — |
| `0xd901` `CommonGeneralControl` | `SetCommonGeneralControl` | mehrfach, mit `Sleep(20)`; „Bypass color correction", „Bypass 10-8 LUT. Use upper 8-bits" | `0x28` |
| `0xd903` `X_Scaling` | `SetX_Scaling` | — | `0x00` |
| `0xd904` `Y_Scaling` | `SetY_Scaling` | — | `0x00` |
| `0xd905` `RedRephaseLineStorageAmount` | `SetRedRephase…` | `Data::GetInt("fpga_red_rephase_{200,300,600}_dpi[_DT]")` | `0x07` |
| `0xd906` `GreenRephaseLineStorateAmount` | `SetGreenRephase…` | `…("fpga_green_rephase_…")` | `0x04` |
| `0xd907` `BlueLineBuffer` | `SetBlueLineBuffer` | `…("fpga_blue_line_buffer")` = `0x28` | `0x28` |
| `0xd920` `X_Y_ResolutionFooterOnly` | `SetX_Y_Resolution…` | „X Y Resolution (Image Footer Only) (D920)" | `0x22` |

Die gemessenen `0x07` / `0x04` sind genau
`fpga_red_rephase_300_dpi_DT = 0x07` und
`fpga_green_rephase_300_dpi_DT = 0x04` aus dem eingebetteten XML — **nicht** die
`…_200_dpi`- und **nicht** die Nicht-DT-Werte. **[MC]** Zwei unabhängige
Bestätigungen in einer Zeile: die **DT**-Variante (wie bei den Kameratabellen)
und die **300-dpi**-Zeile (wie bei `SpeedId` und `SetupImageWidth`).

`0xd907 = fpga_blue_line_buffer = 0x28` stimmt ebenfalls exakt. **[MC]**

### `SetupFPGA(camera, scanType)` — die `0xdaxx`/`0xdbxx`-Register

Ruft in dieser Reihenfolge auf (Quellzeilen 2591–2687) **[C]**:

```
SetupImageHeight          -> Front/RearFixedPageLength      "Image Height (lines)"
SetupGrayscaleSelection   -> Front/RearGrayScaleSelection   (DB2B für rear)
SetupImageWidth           -> Front/RearImageWidth           "Image Width (in pixel pairs)"
SetupPageDetectLeftLimit  -> Front/RearPageDetectLeftLimit
SetupIPControl            -> Front/RearIP_Control
SetupPageStartThreshold   -> Front/RearPageStartThreshold
SetupPageDetectWidth      -> Front/RearPageDetectWidth
SetupHorizontalCrop       -> Front/RearHorizontalCropLeft    (DB02/DB03 für rear)
```

Die Log-Strings nennen die Registeradressen mit: `DB02/DB03` für
`RearHorizontalCropLeft`, `DB04/DB05` für `RearImageWidth`, `DB06/DB07` für
`RearFixedPageLength`, `DB2B` für `RearGrayScaleSelection`. Das deckt sich
Byte für Byte mit der Registerkarte aus PROTOKOLL.md Abschnitt 4a — eine
unabhängige Gegenprobe der dort dekodierten Adressen. **[MC]**

---

## 8. Der 120-Byte-Block von `0x30`: `SDmScanConfig`

**Aufgabe 3 dieser Session — vollständig gelöst.**

`CScannerConfiguration` hat für jedes Feld einen eigenen Setter von 5 bis 17
Byte Länge, der genau eine Instruktion `mov %si,0xNN(%rdi)` enthält. Daraus
fällt die Struktur direkt heraus. Die `SDmScanConfig` beginnt bei `this+8` (bei
`this+0` liegt die `DeviceSettings&`-Referenz), die Strukturoffsets sind also
Setter-Offset minus 8. **[C]**

```c
struct SDmScanConfig {              /* 120 Byte, little endian, ungepackte u16 */
/* 0x00 */ u8  FeedType;                           /* 0 Normal, 1 StopIfEmpty      */
/* 0x01 */ u8  FeedStyle;                          /* 0 None, 1 Normal             */
/* 0x02 */ u16 PageCount;
/* 0x04 */ u16 TransportTimeout;
/* 0x06 */ u16 MaxPageLength;
/* 0x08 */ u16 MultifeedLength;
/* 0x0a */ u16 GapLength;
/* 0x0c */ u8  <unbenutzt>
/* 0x0d */ u16 FrontCaptureLEOffset;               /* unaligned!                   */
/* 0x0f */ u16 RearCaptureLEOffset;
/* 0x11 */ u8  TraySensorMode;                     /* 0 Disabled, 1 Enabled        */
/* 0x12 */ u16 FrontCaptureTEOffset;
/* 0x14 */ u16 RearCaptureTEOffset;
/* 0x16 */ u16 PrinterOffset;
/* 0x18 */ u8  PauseEnable;
/* 0x19 */ u32 MaxPrintCount;
/* 0x1d */ u16 HoleSize;
/* 0x1f */ u8  <unbenutzt>
/* 0x20 */ u8  SpeedId;                            /* 0 Stopped, 2 600dpi, 4 300dpi, 5 200dpi */
/* 0x21 */ u8  DynamicSpeedId;                     /* dieselbe Codierung           */
/* 0x22 */ u8  ThrottleTimeout;
/* 0x23 */ u8  RampId;                             /* 0 Normal, … Long             */
/* 0x24 */ u8  ImageDataId;                        /* 0 Color, 1 Gray              */
/* 0x25 */ u8  Stop;
/* 0x26 */ u16 NotificationOffset;
/* 0x28 */ u16 MagnificationTargetFixedLength;
/* 0x2a */ u16 MagnificationTargetCalibrationLength;
/* 0x2c */ u8  <unbenutzt>[4]
/* 0x30 */ u8  CaptureMode;                        /* 0 Disabled, 1 Normal, 2 shutter */
/* 0x31 */ u8  CaptureStyle;                       /* Front und Rear im selben Byte */
/* 0x32 */ u16 FrontOverscanLength;
/* 0x34 */ u16 RearOverscanLength;
/* 0x36 */ u8  FifoEnable;
/* 0x37 */ u8  CaptureRate;                        /* 0x0f = All lines             */
/* 0x38 */ u16 MultifeedResponse;                  /* 0 Notify only, 1 Stop+leave paper, 2 Stop+clear */
/* 0x3a */ u8  <unbenutzt>[6]
/* 0x40 */ u8  PatchToggleMode;
/* 0x41 */ u8  <unbenutzt>[7]
/* 0x48 */ u16 FeederOnDistance;                   /* byteweise geschrieben        */
/* 0x4a */ u8  <unbenutzt>[46]
};
```

Die Enum-Werte stammen aus den `SETUPSCANCONFIG:`-Log-Strings der jeweiligen
`SetupConfig*`-Funktion, die unmittelbar neben der Konstante steht. **[C]**

### Gegenprobe an den echten 120 Byte

`tools/protocol/scancfg.py` dekodiert den Block aus jedem Mitschnitt. Ergebnis
für `b-simplex-lineart-200` (Referenzdurchgang / Scandurchgang) und
`c-duplex-lineart-200`:

| Offset | Feld | b #1 (Referenz) | b #2 (Scan) | c #1 | c #2 |
|---|---|---|---|---|---|
| `0x00` | FeedType | 0 Normal | 0 Normal | 0 | 0 |
| `0x01` | **FeedStyle** | **0 None** | **1 Normal** | 0 | 1 |
| `0x02` | PageCount | 1 | 0 | 1 | 0 |
| `0x06` | MaxPageLength | 3677 | 3677 | 3677 | 3677 |
| `0x0a` | GapLength | 2154 | 2154 | 2154 | 2154 |
| `0x0d` | FrontCaptureLEOffset | 901 | 901 | 901 | 901 |
| `0x0f` | RearCaptureLEOffset | 373 | 373 | 373 | 373 |
| `0x11` | TraySensorMode | 1 Enabled | 1 | 1 | 1 |
| `0x12` | FrontCaptureTEOffset | 901 | 901 | 901 | 901 |
| `0x14` | RearCaptureTEOffset | 373 | 373 | 373 | 373 |
| `0x20` | **SpeedId** | **0 Stopped** | **4 = 300 dpi** | 0 | 4 |
| `0x24` | ImageDataId | 1 Gray | 1 Gray | 1 | 1 |
| `0x28` | MagnificationTargetFixedLength | 3126 | 3126 | 3126 | 3126 |
| `0x2a` | MagnificationTargetCalibrationLength | 3122 | 3122 | 3122 | 3122 |
| `0x30` | **CaptureMode** | **2 shutter** | **1 Normal** | 2 | 1 |
| `0x32` | FrontOverscanLength | 197 | 197 | 197 | 197 |
| `0x34` | **RearOverscanLength** | **0** | **0** | **197** | **197** |
| `0x36` | **FifoEnable** | **0x11** | **0x01** | **0x33** | **0x03** |
| `0x37` | CaptureRate | 0x0f All lines | 0x0f | 0x0f | 0x0f |
| `0x38` | MultifeedResponse | 1 Stop+leave paper | 1 | 1 | 1 |

Alle übrigen Bytes sind in allen acht `0x30`-Blöcken aller Mitschnitte **null**.
**Die Feldkarte deckt damit jedes nicht-null-Byte ab** — es bleibt kein
unerklärtes Byte übrig. **[MC]**

### Was die gefragten Werte bedeuten

* **Byte `0x30` = `CaptureMode`.** `02` = **shutter**, `01` = **Normal**.
  Die Werte kommen aus `SetupConfigCaptureMode` mit den Log-Strings
  *„Capture mode - Disabled"*, *„… - Normal"*, *„… - shutter"* neben den
  Konstanten 0/1/2. **[C]** Damit ist die Deutung des 51 824-Byte-Blocks aus
  PROTOKOLL.md Abschnitt 5 **belegt**: der erste Durchgang läuft mit
  geschlossenem Shutter, dazu `FeedStyle = None` und `SpeedId = Stopped`, der
  Transport steht. Das ist eine **Dunkelreferenz**, keine Vermutung mehr.
  **[MC]**
* **Byte `0x36` = `FifoEnable`.** Das untere Nibble ist eine Kamerabitmaske:
  `0x01` nur Front (Simplex), `0x03` Front + Rear (Duplex). **[M]** In den
  Shutter-Durchgängen sind zusätzlich die Bits 4/5 derselben Kameras gesetzt
  (`0x11` bzw. `0x33`); wofür, ist **nicht belegt [?]**. Die Bezeichnung
  „Duplexflags" aus PROTOKOLL.md trifft das untere Nibble korrekt.
* **`0x0e5d` = 3677 = `MaxPageLength`.** Berechnet in
  `SetupConfigMaxPageLength` als
  `ceilf( trunc(sheetHeight_1200dpi / 1200) * m_f / 100 )`, wobei
  `sheetHeight` aus dem Fenster kommt und `m_f` ein float-Feld des
  `SetupScanner` ist. Beide Durchgänge und alle Mitschnitte haben denselben
  Wert. Die **Einheit ist nicht geklärt [?]** — der Teiler 100 und `m_f` sind
  nicht aufgelöst.
* **`0x086a` = 2154 = `GapLength`.** Aus `SetupConfigGapLength` →
  `CalcGapSize(float, int, IMAGEFORMAT)` und der Tabelle `GapTable`
  (480 Byte, `.rodata`). `CalcGapSize` verzweigt über
  `*(u16*)(Data+0x3ad4)`, und das ist die **USB-Produkt-ID**: die Fälle sind
  `0x100a`, `0x100b`, `0x100c`, `0x0201`, `0x601c`, **`0x601d`** (= i2600),
  `0x601e`, `0x6031`, `0x6032`. Es ist also ein modellspezifischer
  Einzugsabstand. **[C]** Die Einheit ist **nicht geklärt [?]**.
* **`0x0385` = 901 und `0x0175` = 373** sind `Front`/`RearCaptureLEOffset` und
  dieselben Werte nochmals als `…TEOffset` — die Leading-/Trailing-Edge-Offsets
  der beiden Kameras. Dass sie je Kamera verschieden und je Kante gleich sind,
  passt zu einem **mechanischen Abstand Sensor ↔ Einzugsreferenz**. Die
  Einheit ist **nicht geklärt [?]**; bei 1200 dpi wären es 0,75″ und 0,31″,
  was plausibel aussieht, aber unbelegt ist.
* **`0x0c36` = 3126 und `0x0c32` = 3122** sind
  `MagnificationTargetFixedLength` und `MagnificationTargetCalibrationLength`.
  Die Namen stehen 1:1 so in den NvRam-Dumpstrings
  (`u16MagnificationTargetFixedLength`). **Die Vermutung aus PROTOKOLL.md
  Abschnitt 5, 3126 sei `u16FrontLineWidth`, ist damit widerlegt** — es sind
  Sollängen des Vergrößerungs-Kalibriertargets, keine Zeilenbreiten. **[C]**

---

## 9. Vorbedingungen: `Scanner::IsOkToStartScan()`

Liest `GetStatus()` (Request `0x00` IN, 32 Byte) und prüft einzelne Bytes. Die
Statusstruktur selbst ist **nicht vollständig dekodiert [?]**, die geprüften
Offsets sind es. **[C]**

```c
EDEVMGRSTS IsOkToStartScan(ELAMP_WARMUP_TYPE lampType, bool checkAdfEmpty)
{
    st = GetStatus();                              /* 0x00 IN, 32 Byte */
    if (Data+0x40b8) {                             /* „vor dem Scan Kamera homen" */
        HomeCamera(true);
        st = GetStatus();                          /* nochmals lesen */
        /* dieser Pfad prüft Deckel/Papierweg/ADF NICHT */
    } else {
        if (st[0x13] == 2) return CoverOpen;       /*  6  „Scanner pod is open"            */
        if (st[0x10] != 2) return PaperJam;        /* 20  „Paper path is blocked at UDDS"  */
        if (checkAdfEmpty && st[0x0f] == 1)
                           return AdfEmpty;        /*  2  „ADF is Empty"                   */
    }
    if (st[0x0e] != 0)     return Fail;            /*  1  „Transport is not idle"          */
    if (st[0x1f] & 0x80)   return CriticalError;   /*  7  „Critical error on scanner"      */
    if (GetLampWarmupTime(lampType, *(u16*)&st[0x11]) != 0)
                           return LampsWarming;    /* 12  „Lamps are warming up"           */
    return Success;
}
```

Aufgerufen aus `Scanner::ProcessCommand`, `Scanner::CheckBackgroundColor` und
`ImageManager::GetOutput`. **[C]** Im Mitschnitt wird `C0 00` tatsächlich
gepollt. **[M]**

---

## 10. Bildabholung und Abbau

### `Scanner::CaptureImage(EDMCAMERA, unsigned long)`

```
L2124 >>> CaptureImage
      Scanner::GetMaxChunkSize(camera, &chunk)
L2128                                   "Unable to get the maximum chunk size"
      ImageManager::GetFreeImage(camera)
L2141                                   "Unable to get a free image buffer"
      Image::GetImage(camera, …, Scanner&)      <- liest EP 0x82 / 0x86
L2149                                   "No Image Data Received. This is OK if the scan was cancelled"
      Image::ParseImageFooter()                  <- Abschnitt 11
L2156                                   "Failed to parse the image footer"
      Data::GetBoolean("useimagingcomplete")     (= true)
      ImageManager::FindDocumentIdInQueues(camera, docId, …)
L2174                                   "Failed to find the document id in our image queues"
L2185 <<< CaptureImage
```
**[C]**

`GetMaxChunkSize` fragt die Blockgröße **nicht beim Gerät**, sondern bei der
USB-Schicht: `UsbIoctl(0x1f)` für Kamera 0 und `UsbIoctl(0x20)` für Kamera 1 —
das ist die Gruppe „Chunk-Size abfragen" aus PROTOKOLL.md Abschnitt 2, bedient
von `COsUsbImpl::CalculateMaxChunkSize`. Die 16 384 Byte der Mitschnitte sind
also ein **Hostwert**. **[MC]**

`Scanner::GetMaxImageSizeInBytes(camera)` ist nur ein Getter auf `this+0x158`
bzw. `this+0x160`; die Werte entstehen in `SetupScanner::CalculateMaxImageSize()`
und sind **Hostpuffergrößen**, keine Gerätegrößen: aus
`Data::GetInt("max_scan_pixels") * 2`, geteilt durch 2 (bei 300/400/600 dpi)
oder 4 (bei ≤ 240 dpi), mal Höhe, mal 3 bei Farbe, plus
`image_footer_bytes` (2400) plus `extra_image_padding_bytes` (65535),
aufgerundet auf 64 KiB. **[C]**

### `Scanner::CleanupScan()`

```
L2029 >>> CleanupScan
      ImageManager::StopCapture()
      GetCurrentState(); CancelImageIO(front) / (rear)
      GetStatus()
L2055 "Need to stop the transport"
L2061 "Waiting for transport stopped event..."
      EventReset(); ScannerOperation(0x01)  -> 0x10 val=0 idx=0
      EventWait(5000); EventReset()
L2067 "Halt scanner"        ScannerOperation(0x02)  -> 0x10 val=0 idx=1
L2072 "Cancel IO"           CancelImageIO(front) / (rear)
L2082 "Read in progress, waiting 100ms before purge"   Sleep(100)
L2089 "Timed out waiting for reads to cancel.  ReadInProgess[%d][%d]"
L2092 "Purge Scanner Buffers"      Scanner::PurgeScannerBuffers()
L2096 "Clear Image Manager Queues" ImageManager::ClearQueues()
      Scanner::HomeCamera(true)
```
**[C]** Im Mitschnitt: `40 10 val=0 idx=0` (zweimal), `40 10 val=0 idx=1`,
danach der abgebrochene EP-0x82-URB (Status −2), `40 11 val=0` (Lampe aus),
`40 3a idx=0` (Interrupts aus). **[M]**

---

## 11. Der Bildfooter — und damit die Geometrie

**Aufgabe 4 dieser Session. Vollständig gelöst, mit vier unabhängigen
Belegen.**

`DeviceManager::Image::ParseImageFooter()` (`devicemanager.so` `0x11af30`)
dekodiert einen **TLV-Anhang am Ende jedes Bildblocks, der rückwärts gelesen
wird**. Jeder Datensatz ist 4 Byte: `<WertHi> <WertLo> <01> <Tag>`, der Wert ist
**big endian**. Die Feldnamen stehen wörtlich in
`Image::DumpImageFooter()`. **[C]**

| Tag | Feld | Kodierung |
|---|---|---|
| `0xff` | **Junk Words** | `n` Füllwörter; überspringt danach `n × 2 × buf[i−1]` Byte |
| `0xfe` | **Format** | u16 |
| `0xfd` | **Type** | u16 |
| `0xfc` | **Width (pixels)** | u16 **× 2** — der Wert zählt in Pixelpaaren |
| `0xfa` | **X Resolution** | u16-Code → `ResolutionToDPI` |
| `0xf9` | **Y Resolution** | u16-Code → `ResolutionToDPI` |
| `0xf8` | **Print String** (≤ 22 Zeichen) + **Printer Index** (u32 BE) | längenpräfigiert |
| `0xf7` | — | 4 Byte, Wert wird verworfen |
| `0xf6` | **Segment** (u16 & 0x7fff) + **Last Segment** (Bit 15) | |
| `0xf5` | **Document Count** | u16, bzw. u32 wenn `documentcountsize == 4` (so beim i2600) |
| `0xf4` | zwei u16 ohne Dumpstring | **unbenannt [?]** |
| `0x00` | Füllbyte | 2 Byte |
| `0x01` | **Footerbeginn** | Footerlänge = Blocklänge − hier; Bilddaten = Blocklänge − Footerlänge |

`ImageFooterTags::ResolutionToDPI` ist eine 5-Einträge-Sprungtabelle **[C]**:

| Code | 0 | 1 | 2 | 3 | 4 |
|---|---|---|---|---|---|
| dpi | 600 | 400 | 300 | 200 | 150 |

### Das Ergebnis an den echten Mitschnitten

`tools/protocol/footer.py` wendet den Algorithmus auf alle vollständig
erfassten Blöcke an (`captures/analysis/image-footer.txt`). Sieben Blöcke sind
im pcapng vollständig; alle sieben parsen fehlerfrei bis zum
`0x01`-Startmarker. **[M]**

| Mitschnitt / Block | Blocklänge | Footer | Bilddaten | Width | Zeilen | X/Y |
|---|---|---|---|---|---|---|
| b, 0x82, Referenz | 51 824 | 224 | 51 600 | 1290 Paare = **2580 px** | **20** | 300/300 |
| b, 0x82, Blatt 1 | 10 421 120 | 500 | 10 420 620 | 2580 px | **4039** | 300/300 |
| c, 0x82, Referenz | 51 824 | 224 | 51 600 | 2580 px | 20 | 300/300 |
| c, 0x82, Blatt 3 | 10 503 416 | 236 | 10 503 180 | 2580 px | **4071** | 300/300 |
| c, 0x86, Referenz | 51 824 | 224 | 51 600 | 2580 px | 20 | 300/300 |
| e, 0x82, Referenz | 51 824 | 224 | 51 600 | 2580 px | 20 | 300/300 |
| f, 0x82, Referenz | 51 824 | 224 | 51 600 | 2580 px | 20 | 300/300 |

**In allen sieben Fällen geht `Bilddaten / 2580` restlos auf.** **[M]**

### Damit ist das Rohbildformat festgelegt

```
Eine Bildzeile = 2580 Byte = 2580 Pixel zu 8 bit.
Ein Bildblock  = Zeilen × 2580  +  2 × JunkWords  +  36 Byte Tagdatensätze.
```

Die 36 Byte sind neun Datensätze à 4 Byte (`0xff`, `0xfe`, `0xfd`, `0xfc`,
`0xfa`, `0xf9`, `0xf6`, `0xf5`) plus 2 Byte Füllung plus 2 Byte Startmarker;
gemessen 224 = 188 + 36, 500 = 464 + 36, 236 = 200 + 36. **[M]**

**Die 508-Byte-Granularität aus PROTOKOLL.md ist damit erklärt.** Für **alle 17
Bildblöcke aller Mitschnitte** gilt

```
Blocklänge ≡ 8  (mod 508)
```

und die `JunkWords`-Zahl ist genau die Füllung, die das herstellt. Nachgerechnet
für die drei Blöcke mit bekannter Zeilenzahl **[M]**:

| Zeilen | 2 × JunkWords nötig | gemessen |
|---|---|---|
| 20 | 188 | 188 (JunkWords = 94) |
| 4039 | 464 | 464 (JunkWords = 232) |
| 4071 | 200 | 200 (JunkWords = 100) |

Drei unabhängige Übereinstimmungen. **508 Byte ist also eine
Übertragungsgranularität des Geräts, keine Bildstruktur** — die Füllwörter
hängen sie an, und der Footer sagt dem Host, wie viele davon zu verwerfen
sind. **[M]**

Der „8-Byte-Trailer" aus PROTOKOLL.md Abschnitt 5 war die Beobachtung
desselben Sachverhalts von außen: `00 5e 01 ff` sind nicht `00 <u16 LE> ff`,
sondern der **Datensatz `0xff`**: `00 5e` = JunkWords (BE!), `01` = Multiplikator,
`ff` = Tag. Die „Verdopplung der letzten vier Nutzdatenbytes" war der Anfang des
Füllbereichs. **[M]** Der dort als unerklärt notierte u16 (0x015e beim
Referenzblock, 0x01e8 / 0x0164 bei Bildblöcken) ist die
**Füllwortzahl 94 / 232 / 100 plus das Multiplikatorbyte**.

### Und `FrontImageWidth` ist in Pixelpaaren

Vier unabhängige Belege:

1. **Der Log-String sagt es wörtlich.** `SetupScanner::SetupImageWidth` gibt aus:
   `"%s: %s Image Width (in pixel pairs) - 0x%x, %d"`. **[C]**
2. **Der Footer multipliziert mit 2.** Tag `0xfc` → `Width (pixels) = u16 × 2`,
   und der u16 im Mitschnitt ist `0x050a` = 1290 — exakt der Wert, den der
   Treiber vorher in `FrontImageWidth` geschrieben hat. **[MC]**
3. **Die Rechnung geht auf.** 2580 Byte pro Zeile teilt die Bilddatenmenge
   aller sieben vollständig erfassten Blöcke restlos. **[M]**
4. **Der Referenzblock passt zum Höhenregister.** 20 Zeilen × 2580 = 51 600
   Byte, und `FrontFixedPageLength` stand im Referenzdurchgang auf
   `0x0014` = 20 — mit dem Log-String `"Image Height (lines)"`. **[MC]**

Woher die 1290 kommen, zeigt `SetupImageWidth` **[C]**:

```
w = Fensterbreite, sonst Data::GetInt("max_scan_width") = 10320   (1200-dpi-Pixel)
switch (m_Resolution) {
    case 300: case 240: w >>= 3;  break;      /* 10320 / 8  = 1290 */
    case 600:           w >>= 2;  break;      /* /4  = 2580        */
    case 400:           w = w/3/4; break;     /* /12 =  860        */
    case 200:           w = w/3/8; break;     /* /24 =  430        */
    case 150: case 100: w >>= 4;  break;      /* /16 =  645        */
}
if (!Data+0x40b8) w &= ~1;
SetFront/RearImageWidth(w);
```

Der Teiler ist `1200/dpi × 2`: einmal für die Umrechnung von 1200-dpi-Pixeln auf
Scanpixel, einmal für Pixel → Pixelpaare. Für 1290 passt **nur** der
300-dpi-Zweig. **[C]**

### Das Gerät scannt immer mit 300 dpi

Vier zusammenpassende Hinweise:

* `SetupImageWidth` benutzt den 300-dpi-Teiler — bei 200 dpi wären es 430
  Pixelpaare, gemessen sind es 1290. **[MC]**
* `SDmScanConfig.SpeedId = 4`, Log-String *„Speed Id - 300 dpi"*. **[MC]**
* Der Footer meldet X- und Y-Auflösungscode 2 = 300 dpi. **[M]**
* `X_Y_ResolutionFooterOnly = 0x22` — zwei Nibbles mit je Code 2, genau die
  beiden Footerwerte. **[M]**

2580 Pixel bei 300 dpi sind 8,6″ Scanbreite; der i2600 nimmt Vorlagen bis 8,5″
(`max_sheet_width = 10200` = 8,5″ bei 1200 dpi). **[C]** Das passt, ist aber
kein Beweis für die physische Sensorauflösung **[?]** — der Registername
`X_Y_ResolutionFooterOnly` sagt, dass die Angabe nur in den Footer geht. Belegt
ist: **der Host konfiguriert und erwartet 300 dpi, unabhängig von der
angeforderten Auflösung.** Das bestätigt Aufgabe A aus PROTOKOLL.md
Abschnitt 3 noch einmal von der Geometrieseite.

**Offene Frage 1 des README ist damit geschlossen** — bis auf die Frage, warum
ein Blatt 4039 bzw. 4071 Zeilen liefert (13,5″ bei 300 dpi, deutlich mehr als
A4), was auf Vor- und Nachlauf plus `FrontOverscanLength` zurückgeht und sich
mit `FrontFixedPageLength = 4650` als Obergrenze deckt. Der in PROTOKOLL.md
vorgeschlagene Versuch mit einer kürzeren Vorlage ist **nicht mehr nötig**, um
die Einheiten zu klären; er wäre nur noch zur Bestätigung interessant.

---

## 12. Mindestsequenz für ein natives Backend

Zusammengefasst aus den Abschnitten 5–10, **ohne** Firmware-Download,
Kalibrierläufe, Drucker, Diagnose und Flachbett. Alle Schritte sind im
Mitschnitt belegt **[M]**, die Reihenfolge im Code **[C]**.

```
--- Öffnen ---------------------------------------------------------------
 1  USB: SET_CONFIGURATION 1, Interface 0, Alternate 0
 2  C0 00  GetStatus                (32 B)   -> Zustand prüfen
 3  C0 02  GetFirmwareVersions      (56 B)
 4  C0 34  GetEndOfLine             (55 B)   -> VID/PID, Modellstring
 5  C0 36  GetBoardSerialNumber     (16 B)
 6  C0 37  GetSessionInfo           (64 B)
 7  40 1f  SetTime
 8  C0 03  GetMachineConstants      (32 B)
 9  C0 35  GetNvRamData  wValue=0   (128 B)  -> Kalibrierdaten, Zeilenbreiten
10  40 a3  wValue=0xda27/0xdb27, 4 B = DocId
11  C0 e3  ReadFlashMemory wIndex=0xfc00 (256 B)
12  40 e0  SetAfeStandby, Kamera 0 und 1:
              Seite 0 Reg 0x02 <- 0x20
              Seite 2 Reg 0x05 <- 0x00
              Seite 3 Reg 0x00..0x04 <- 0x88
           dazwischen 0xa3 auf 0xd901 (Testmuster- und Camera-Enable-Bit)
13  C0 45  GetBatchData             (3 B)
14  40 11  wValue=0                          -> Lampe aus
--- Scan vorbereiten ------------------------------------------------------
15  40 3a  wValue=1 wIndex=1                 -> Interrupt-Ereignisse EIN
16  40 1b  wValue=0                          -> ScannerOperation 0x0d
17  EP 0x88 lesen, auf erstes Ereignis warten (5 s Timeout)
18  40 31  SetEnergyStar
19  40 11  wValue=1                          -> Lampe ein
20  40 37  SetSessionInfo           (64 B)
21  C0 00  GetStatus                -> IsOkToStartScan (Abschnitt 9)
--- Registerprogrammierung (je Durchgang) ---------------------------------
22  40 a3  d9xx: X_Scaling=0, Y_Scaling=0, Rephase/BlueLineBuffer,
           X_Y_ResolutionFooterOnly=0x22, JpegControl..SDRamDriverEnable (78 B)
23  40 e0  SetCameraResolution: Seiten 0..5 je 32 B aus
           s_dmcamerags300{slave,master}DT, dann
           Seite 6 Reg 0x01 <- 0x22;  Seite 0 Reg 0x02 <- 0x28, dann 0x08, dann 0x19
           (davor, noch vor den Seitenblöcken: Seite 0 Reg 0x02 <- 0x20, beide Kameras)
24  40 e0  Seite 1 Reg 0x00, 3 B = AFE-Gains; Gegenprobe mit C0 e0
25  40 a3  daxx/dbxx: FixedPageLength, GrayScaleSelection, ImageWidth,
           PageDetectLeftLimit, IP_Control, PageStartThreshold,
           PageDetectWidth, HorizontalCropLeft
26  40 a3  CommonGeneralControl = 0x28, 100 ms warten,
           WhiteBalanceTarget lesen, WhitePatchAverage dreimal lesen
27  40 a3  UddsTwoPageSampleInterval, UddsMultifeedSetup
--- Referenzdurchgang -----------------------------------------------------
28  40 30  SDmScanConfig, 120 B:  CaptureMode=2 (shutter), FeedStyle=0 (None),
           SpeedId=0 (Stopped), PageCount=1, FifoEnable=0x11 (bzw. 0x33)
29  40 10  wValue=1                          -> Start
30  EP 0x88: 10 Power State, 02 Start of Operation
31  40 17  wValue=1                          -> StartCapture
32  EP 0x82 lesen bis Kurzpaket: 51 824 B, Footer -> 20 Zeilen à 2580 B
33  40 10  wValue=0                          -> Stop
34  EP 0x88: 42 Imaging Complete, 01 End of Operation
--- Scandurchgang ---------------------------------------------------------
35  40 a3  FrontValidPixelLevelThreshold, FrontFixedPageLength = 4650
36  40 30  SDmScanConfig, 120 B:  CaptureMode=1 (Normal), FeedStyle=1 (Normal),
           SpeedId=4 (300 dpi), PageCount=0, FifoEnable=0x01 (bzw. 0x03)
37  40 10  wValue=1                          -> Start
38  EP 0x88: 10 Power State, 12 Transport State=1
39  je Blatt: EP 0x82 (und 0x86) lesen bis Kurzpaket
              -> Footer parsen -> Zeilen = (Blocklänge − Footer) / 2580
              EP 0x88: 42 Imaging Complete (Zähler), 41 Page Exit (Blattnummer)
40  EP 0x88: 12 Transport State=0 wenn der Einzug leer ist
--- Abbau -----------------------------------------------------------------
41  40 10  wValue=0 wIndex=0   (2x)          -> Transport stoppen
42  40 10  wValue=0 wIndex=1                 -> Halt
43  EP 0x82/0x86 URBs abbrechen
44  40 11  wValue=0                          -> Lampe aus
45  40 3a  wValue=1 wIndex=0                 -> Interrupt-Ereignisse AUS
46  40 e0  SetAfeStandby nochmals
```

Die genauen Registerwerte des Mitschnitts stehen in PROTOKOLL.md
Abschnitt 4a und in `captures/analysis/*.fpga-registers.txt`, die
Kameraseiten in `captures/analysis/camera-afe-registers.txt` und
`camera-page-tables.txt`, der `0x30`-Block in
`captures/analysis/scanconfig-0x30.txt`.

---

## 13. Was offen bleibt

| Punkt | Stand |
|---|---|
| Typbezeichnung des Kamerabausteins hinter `0xe0` | **nicht im Binary** — keine Chipnamen, keine Registernamen. Struktur (7 Seiten × 32 Register, Seitenwahl `0x1f`) und alle benutzten Register sind belegt |
| Bedeutung der einzelnen Kameraregister | **nicht belegt [?]**. Belegt sind die *Werte* (byteidentisch zu `s_dmcamerags300{master,slave}DT`) und die Funktionen, die sie schreiben |
| `0x1b` (`ScannerOperation 0x0d`) | Aufrufstelle belegt, **Bedeutung offen [?]** |
| `wValue` von `0x3a` | **offen [?]**; in `StopInterruptEvents` ist es `(Data+0x2aa3 == 0)` |
| `0x62` (768 Byte) | unverändert **offen [?]** |
| Einheit von `MaxPageLength`, `GapLength`, `Capture{LE,TE}Offset`, `OverscanLength` | **offen [?]**; die Rechenwege sind bekannt, die Skalierungsfaktoren nicht |
| Hohes Nibble von `FifoEnable` im Shutter-Durchgang | **offen [?]** |
| Tag `0xf4` des Bildfooters | zwei u16, **kein Dumpstring, unbenannt [?]** |
| Die 32-Byte-Statusstruktur von `0x00` | nur die sechs von `IsOkToStartScan` geprüften Bytes sind zugeordnet |
| `Data+0x40c4` (DT-Auswahl), `Data+0x40b8` (Kamera-Homing) | als Schalter identifiziert, Herkunft **offen [?]** |
| `KDS_DEBUGUSB` / `KDS_DEBUG=1` | weiter **nicht ausprobiert**. Mit `KDS_DEBUG=1` müsste der Treiber die oben zitierten Log-Zeilen samt `dm_*.cpp`-Zeilennummern ausgeben; das wäre die billigste Gegenprobe der ganzen Sequenz |

### Nächste Schritte, nach Nutzen sortiert

1. **`KDS_DEBUG=1` einschalten und einen Scan protokollieren.** Jede
   Ablaufaussage dieses Dokuments trägt eine Quellzeilennummer; ein Lauf mit
   Treiberprotokoll prüft sie alle in einem Durchgang.
2. **Kalibrierdaten lesen.** `C0 35 wValue=0` (NvRam, 128 B) enthält laut den
   Dumpstrings `u16FrontLineWidth`, `u16RearLineWidth`, `u16Front/RearLeftPixel`,
   `u16Front/RearCameraDistance`, `u16MagnificationTargetFixedLength`. Mit der
   jetzt bekannten Geometrie (2580 px/Zeile) sollte sich die Feldlage des
   NvRam-Blocks aus dem Mitschnitt zuordnen lassen — das ist der letzte
   Baustein für korrekte Beschnittgrenzen.
3. **`/var/kodak/kds_i2000/data/48785942/` leeren und neu mitschneiden**, um
   `0xe4`/`0xe5` (Color LUT, Color Correction) mit vollen Nutzdaten zu sehen.
4. **Prototyp schreiben**, der Schritte 1–34 der Mindestsequenz ausführt und
   den Referenzblock einliest. Wenn 51 824 Byte mit einem gültigen Footer
   zurückkommen, ist die halbe Arbeit verifiziert — ohne Papier im Einzug.

---

## 14. Werkzeuge

Neu in `tools/protocol/`:

| Datei | Zweck |
|---|---|
| `dmtrace.py` | annotierter statischer Trace einer Funktion aus `devicemanager.so`, mit Quellzeilennummern aus `COsLog::Message` und Rekonstruktion der Quellreihenfolge |
| `footer.py` | dekodiert den Bildfooter jedes vollständig erfassten Bildblocks (Reimplementierung von `Image::ParseImageFooter`) |
| `camregs.py` | benennt die `0xe0`-Zugriffe eines Mitschnitts als Kamera/Seite/Register |
| `camtab.py` | vergleicht die sechs Kameraseiten eines Mitschnitts mit den zwölf `s_dmcamera*`-Tabellen |
| `scancfg.py` | dekodiert den 120-Byte-`SDmScanConfig`-Block von `0x30` |

`footer.py`, `camregs.py` und `scancfg.py` brauchen `usbmon.py` und `ctrl.py`
aus demselben Verzeichnis im `PYTHONPATH`; `camtab.py` erwartet zusätzlich
`devicemanager.so` (Standardpfad `driver/opt/kodak/kds_i2000/lib/`, sonst als
zweites Argument). `dmtrace.py` braucht vorab
`objdump -d -C --no-show-raw-insn devicemanager.so > dm.dis`.

Neue Ergebnisse in `captures/analysis/`:

| Datei | Inhalt |
|---|---|
| `image-footer.txt` | dekodierte Bildfooter aller Mitschnitte |
| `camera-afe-registers.txt` | benannte `0xe0`-Zugriffe aller Mitschnitte |
| `camera-pages.txt` | Abgleich Kameraseiten ↔ statische Tabellen |
| `camera-page-tables.txt` | die zwölf `s_dmcamera*`-Tabellen, nach Seiten aufgelöst |
| `scanconfig-0x30.txt` | dekodierte `SDmScanConfig`-Blöcke aller Mitschnitte |
