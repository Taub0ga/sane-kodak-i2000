# Kodak i2600 unter Linux (Manjaro)

Dieses Verzeichnis enthält

* ein **PKGBUILD**, das den letzten offiziellen Kodak-Alaris-Linux-Treiber
  (v04.14.00.00, Dezember 2016, closed source, ursprünglich für Ubuntu 16.04)
  so paketiert, dass pacman alle Dateien verwaltet und der Treiber auf einem
  aktuellen System (glibc 2.44, Kernel 6.18) funktioniert,
* **Werkzeuge**, um den USB-Verkehr des Scanners mitzuschneiden und auszuwerten,
* **Mitschnitte** von vier Szenarien plus eine erste Protokollanalyse.

Getestet am 2026-10-19 auf Manjaro, Kernel 6.18.49, glibc 2.44,
sane-backends 1.4.0, mit einem Kodak i2600 (USB 040a:601d) an einem
Intel-Mainboard-Port.

> **Hardware-Hinweis:** Der Scanner darf nicht an der ASMedia-USB-Buchse
> hängen (PCI 0000:07:00.0 meldet ungültige Port-IDs und enumeriert nicht).
> Außerdem meldet sich das Gerät nach einiger Zeit von selbst vom Bus ab
> (`usb 1-6: USB disconnect` im Kernel-Log), ohne dass darauf zugegriffen
> wurde. Vor jedem Versuch prüfen: `lsusb -d 040a:601d`.

---

## Status: was verifiziert ist

**Verifiziert** (selbst ausgeführt und beobachtet):

* Paket baut und installiert sich mit `makepkg`/`pacman`.
* `ldd -r` löst für alle installierten Binaries auf — mit einer Ausnahme, die
  schon im Original so ist (siehe [Vendor-Bug in hippo.so](#vendor-bug-in-hipposo)).
* `scanimage -L` findet das Gerät: `kds_i2000:i2000`.
* `scanimage -A` liefert den vollen Optionssatz (Modus, Auflösung bis 1200 dpi,
  Duplex, Cropping, Perfect-Page-Optionen).
* **Simplex-Scan** durchgeführt: 1658 × 2395 px, Lineart, 200 dpi.
* **Duplex-Scan** durchgeführt: zwei Seiten, 1657 × 2399 und 1661 × 2395 px,
  je ~22 % Schwarzanteil, also echter Inhalt auf beiden Seiten.
* Fünf USB-Mitschnitte im Textformat und drei mit vollen Nutzdaten (pcapng)
  unter `captures/`.
* Der vollständige 120-Byte-Scan-Setup-Block und alle Antwortblöcke der
  Vendor-Requests sind erfasst und dokumentiert.
* Vergleichsscans mit 300 dpi und in Graustufen belegen: **Auflösung und
  Farbmodus ändern am Gerät nichts** — gleicher Setup-Block, gleiche
  Rohdatenmenge (Spannweite 0,4 %).

**Nicht getestet / nicht verifiziert:**

* Die Einstellungs-GUI (`/opt/kodak/kds_i2000/lib/twaingui` → Mono-Programm
  `twaingui.exe`). Mono ist auf diesem System nicht installiert.
* Auflösungen über 200 dpi, Farb- und Graustufenscans, ADF-Stapel mit mehr als
  zwei Blättern, Flachbett-Zubehör.
* `pnphelper` und die Tastenüberwachung des Herstellers.
* Das Paket auf einem anderen Rechner oder mit einem anderen Modell der
  i2000-Serie (i2400/i2800, 29cc:100a/b/c).

---

## Teil A — Installation

### Voraussetzungen

```bash
sudo pacman -S --needed base-devel sane
```

`binutils` (für `ar`) und `python3` stecken in `base-devel` bzw. im Basissystem.
Die drei Herstellerpakete müssen im Projektverzeichnis liegen:

| Datei | SHA-256 (Anfang) |
|---|---|
| `kodak_i2000-04.14.00.00-1.amd64.deb` | `4aad3e5f…` |
| `libopenusb_1.1.13-0_amd64.deb` | `51f9b230…` |
| `twaindsm.deb` | `5356fed7…` |

`packaging/` enthält Symlinks darauf; die vollständigen Summen stehen im
`PKGBUILD`.

### Bauen und installieren

```bash
cd "packaging" && makepkg -f && sudo pacman -U kodak-i2000-driver-04.14.00.00-1-x86_64.pkg.tar.*
```

`makepkg` legt `src/` und `pkg/` an; mit `BUILDDIR=/tmp/kodak-build makepkg -f`
bleibt das Projektverzeichnis sauber.

Danach:

```bash
scanimage -L
```

sollte `device 'kds_i2000:i2000' is a KODAK i2000 sheetfed scanner` zeigen.

### Was das Paket installiert

| Pfad | Inhalt |
|---|---|
| `/opt/kodak/kds_i2000/` | Treiberkern: `device.so`, `driver.so`, `hippo.so` (Perfect Page), `devicemanager.so`, `lexexe`, `driverlexexe`, mitgelieferte Fremdbibliotheken, Profile, `pnphelper` |
| `/usr/local/lib/twain/kodak/kds_i2000/kds.ds` | TWAIN Data Source |
| `/usr/local/lib/libopenusb.so*`, `openusb_backend/` | libopenusb 1.1.13 |
| `/usr/local/lib/libtwaindsm.so*` | TWAIN Data Source Manager 2.3.1 |
| `/usr/local/lib/libudev.so.0` | Kompatibilitäts-Shim (siehe unten) |
| `/usr/local/include/twain.h` | TWAIN-Header, nützlich für eigene Arbeit |
| `/usr/lib/sane/libsane-kds_i2000.so*` | SANE-Backend (Shim auf TWAIN) |
| `/etc/sane.d/kds_i2000.conf` | Backend-Konfiguration |
| `/etc/sane.d/dll.d/kodak-i2000` | aktiviert das Backend |
| `/etc/ld.so.conf.d/kodak-i2000.conf` | legt `/usr/local/lib` auf den Loader-Pfad |
| `/usr/lib/tmpfiles.d/kodak-i2000.conf` | erzeugt `/var/kodak/...` |
| `/usr/local/bin/deviceprobe_i2000` | Hersteller-Hilfsprogramm zur USB-Suche |

Die Pfade `/usr/local/lib/libopenusb.so` und `/usr/local/lib/libtwaindsm.so`
sind in `device.so`, `driver.so` und `kds.ds` **hart verdrahtet** und müssen
genau dort liegen.

### Deinstallation

```bash
sudo pacman -R kodak-i2000-driver && sudo rm -rf /var/kodak
```

`pacman -R` entfernt restlos alle Dateien aus der Tabelle oben samt der
`ld.so.conf.d`- und `tmpfiles.d`-Fragmente. `/var/kodak` bleibt absichtlich
stehen (dort liegen Logs und der Geräte-Lock) und muss, wenn gewünscht, von
Hand weg. Das Hersteller-Deinstallationsskript
(`/opt/kodak/kds_i2000/install/uninstall.sh`) ist nicht im Paket enthalten: es
würde versuchen, `dpkg`/`rpm` zu bedienen.

---

## Teil A.1 — Warum nicht das `setup`-Skript des Herstellers

Das mitgelieferte `setup` wird **nicht** benutzt. Es

* prüft `lsb_release` gegen eine Distributions-Whitelist (Manjaro fehlt → Exit 4),
* ruft `apt-get` und `alien` auf,
* macht `chmod -R 777 /var/kodak`,
* patcht `/etc/udev/rules.d/50-udev-default.rules`,
* schreibt `/etc/udev/rules.d/55-kodakdi.rules` mit `MODE:="666"`, also
  Lese- und Schreibzugriff für jeden Benutzer auf den rohen USB-Knoten,
* hängt `kds_i2000` an `/etc/sane.d/dll.conf` an, eine Datei, die dem
  `sane`-Paket gehört.

Das PKGBUILD macht nur, was tatsächlich nötig ist, und stattdessen:

### Keine eigene udev-Rule — es braucht keine

sane-backends 1.4.0 führt den i2600 selbst:

```
/usr/lib/udev/hwdb.d/20-sane.hwdb:  usb:v040Ap601D*  →  libsane_matched=yes
/usr/lib/udev/rules.d/70-uaccess.rules:  ENV{libsane_matched}=="yes", TAG+="uaccess"
```

systemd-logind setzt daraufhin eine ACL für den **lokal angemeldeten**
Benutzer:

```
$ getfacl /dev/bus/usb/001/006
user::rw-
user:tauboga:rw-      ← uaccess
group:saned:rw-
other::r--
```

Das ist deutlich restriktiver als `MODE=666` und braucht keine zusätzliche
Regel. Nachprüfen mit `udevadm info -q all -n /dev/bus/usb/<bus>/<dev> | grep TAGS`.

Falls doch einmal eine eigene Regel nötig wird — etwa für einen Dienst ohne
Sitzung (saned, Headless-Server) —, wäre die restriktive Variante:

```
# /etc/udev/rules.d/60-kodak-i2600.rules
SUBSYSTEM=="usb", ATTR{idVendor}=="040a", ATTR{idProduct}=="601d", \
    MODE="0660", GROUP="scanner"
```

und der betreffende Benutzer bzw. Dienst in die Gruppe `scanner`. **Nicht**
nötig für den Normalbetrieb am Desktop.

`RUN+="/opt/kodak/kds_i2000/pnphelper action:add"` aus dem postinst des
Herstellers ist für den SANE-Betrieb ebenfalls nicht nötig: ohne jede
Kodak-Regel wird das Gerät gefunden, geöffnet und gescannt. `pnphelper` dient
der Tastenüberwachung und einer PnP-Liste des Herstellers.

### `/var/kodak` mit Sticky-Bit statt 777

Der Treiber legt dort zur Laufzeit als scannender Benutzer an:

* `kds_i2000/device/usb040a601d<serial>.lock` — **Geräte-Lock, wird bei jedem
  Öffnen gebraucht**
* `kds_i2000/twain/log.<timestamp>/<modul>/` — Debug-Logs
* `kds_i2000/profiles/profile.share` — Scan-Profile
* `kds_i2000/sanetwain/sanetwain_N.log` — Log des SANE-Shims

`/usr/lib/tmpfiles.d/kodak-i2000.conf` erzeugt diese Verzeichnisse mit Modus
**1777** (sticky, wie `/tmp`): jeder darf anlegen, keiner darf die Dateien
anderer löschen oder ersetzen. `/var/kodak` selbst bleibt `0755 root:root`.

### Backend-Aktivierung über `dll.d`

`/etc/sane.d/dll.d/kodak-i2000` mit dem Inhalt `kds_i2000` statt eines
Eintrags in der fremden `dll.conf`.

---

## Teil A.2 — Die drei Dinge, die auf einem aktuellen System brechen

### 1. `libudev.so.0` existiert nicht mehr

`openusb_backend/linux.so` hat `DT_NEEDED: libudev.so.0`, also die
Vor-systemd-SONAME. Alle 21 udev-Symbole, die es importiert, existieren
unverändert in `libudev.so.1`, und die Referenzen sind **unversioniert**:

```bash
comm -23 \
  <(readelf --dyn-syms -W /usr/local/lib/openusb_backend/linux.so \
     | awk '$7=="UND"{print $8}' | sed 's/@.*//' | grep ^udev | sort) \
  <(readelf --dyn-syms -W /usr/lib/libudev.so.1 \
     | awk '$7!="UND"{print $8}' | sed 's/@.*//' | sort -u)
# keine Ausgabe = alles auflösbar
```

Deshalb genügt ein Weiterleitungs-Shim ohne eine Zeile Code
(`packaging/libudev0-shim.c` ist nur ein Kommentar):

```bash
gcc -shared -fPIC -o libudev.so.0 libudev0-shim.c \
    -Wl,-soname,libudev.so.0 -Wl,--no-as-needed -ludev
```

Das Ergebnis trägt `SONAME=libudev.so.0` und `DT_NEEDED=libudev.so.1`; der
Loader erfüllt damit die Abhängigkeit aus der System-libudev. Installiert
nach `/usr/local/lib/libudev.so.0`; damit der Loader es findet, legt
`/etc/ld.so.conf.d/kodak-i2000.conf` `/usr/local/lib` auf den Suchpfad.

### 2. Executable Stack — der eigentliche Blocker

`hippo.so`, `libuic_io.so` und `libuic_jpeg.so` haben
`PT_GNU_STACK = RWE`, verlangen also einen ausführbaren Stack. Seit
**glibc 2.41** macht der Loader den Stack bei `dlopen()` nicht mehr
nachträglich ausführbar, sondern scheitert hart:

```
hippo.so: cannot enable executable stack as shared object requires: Invalid argument
```

Oben im Stack sieht man davon nur (mit `KDS_DEBUG=1`):

```
<lexexe.cpp> <68> <SharedLibraryLoad failed...>
<lexexe.cpp> <678> <LoadDll failed...>
<dev_cdevhippo.cpp> <2434> <lexexe is gone...>
<twn_ctwain.cpp> <1117> <interfaceopen failed...>
scanimage: open of device kds_i2000:i2000 failed: Invalid argument
```

`tools/clear-execstack.py` löscht `PF_X` im `PT_GNU_STACK`-Programmheader —
dasselbe, was `execstack -c` bzw. `patchelf --clear-execstack` täten; beide
sind für Arch nicht gepackt. Das PKGBUILD ruft es in `build()` auf und prüft
anschließend alle übrigen Bibliotheken.

Dass die Markierung ein Build-Artefakt und keine echte Anforderung ist, ist
**wahrscheinlich, aber nicht bewiesen**: die Perfect-Page-Bibliothek enthält
handgeschriebene SSE-Assembly, und `.o`-Dateien aus `.s`-Quellen ohne
`.note.GNU-stack` lassen das Linker-Flag hochschlagen. Dazu passt der
Vendor-Bug im nächsten Abschnitt. Gegenprobe: Simplex- und Duplex-Scans
laufen mit gelöschtem Flag durch, inklusive der kompletten
Perfect-Page-Bildverarbeitung (Autocrop, Deskew, Bitonal-Wandlung).

### 3. `/var/kodak/kds_i2000/device` muss beschreibbar sein

Sonst scheitert der Geräte-Lock und die TWAIN-Quelle meldet `TWCC_NODS`:

```
usb>>> COsUsbImpl::DeviceLock: device lock deviceLock failed to open Lock file...
        /var/kodak/kds_i2000/device/usb040a601d0000000000000000.lock 257
dev_cdevio  LockDevice - Lock failed...1
twn_ctwain  CTwain constructor failed (device probably not found)...
```

Deckt die `tmpfiles.d`-Datei ab.

### Vendor-Bug in `hippo.so`

`hippo.so` lässt vier Symbole offen, die es selbst in der C++-gemangelten
Form definiert:

```
U  _Calculate3x3SumStripSSE          T  _ZN4ripl23Calculate3x3SumStripSSEEPKtS1_S1_Ptj
U  _CalculateSparse9x9SumStripSSE    T  _ZN4ripl29CalculateSparse9x9SumStripSSEE...
U  _Calculate3x3GradientStripSSE     T  _ZN4ripl28Calculate3x3GradientStripSSEE...
U  _Calculate5x5GradientStripSSE     T  _ZN4ripl28Calculate5x5GradientStripSSEE...
```

Es sind `R_X86_64_JUMP_SLOT`-Relocations, also PLT-Einträge, die erst beim
ersten Aufruf aufgelöst werden. `dlopen(..., RTLD_LAZY)` funktioniert,
`RTLD_NOW` nicht — der Treiber benutzt LAZY. Der Fehler steckt im
Originalbinary und ist von uns nicht verursacht; solange diese vier
Funktionen nie aufgerufen werden, fällt er nicht auf. Wird eine davon doch
aufgerufen, stirbt der Prozess mit einem Symbol-Lookup-Error. In den
durchgeführten Scans ist das nicht passiert.

---

## Teil A.3 — Fehlersuche

Nützliche Umgebungsvariablen (aus den Binaries extrahiert):

| Variable | Wirkung |
|---|---|
| `KDS_DEBUG=1` | Konsolenausgabe von `kds.ds`/`driver.so` **mit Quelldateiname und Zeilennummer** — der schnellste Weg zur Ursache |
| `KDS_ALL=1 KDS_FULLLOGGING=1 KDS_LOGDIR=<dir>` | vollständige Modul-Logs (mehrere MB pro Lauf) |
| `KDS_DEBUGUSB` | USB-Protokollierung im Treiber (nicht ausprobiert) |
| `SANETWAIN_DEBUG=1 SANETWAIN_LOG=<datei>` | Log des SANE-Shims mit `kds.c`-Zeilennummern |
| `TWAINDSM_LOG=<datei>` | Log des TWAIN Data Source Managers (zeigt `DG_CONTROL`-Verkehr) |
| `OPENUSB_BACKEND_PATH` | überschreibt den Backend-Pfad von libopenusb |
| weitere | `KDS_LOGDIR`, `KDS_OSCFGDIR`, `KDS_OVERRIDEINI`, `KDS_SIMMODEL`, `KDS_FORCEGUI`, `KDS_TWAINDELAY`, `HIPPO_LOGPATH`, `HIPPO_LOGDEBUG`, `HIPPO_LOGFILTER`, `SANETWAIN_LOGMODE`, `SANETWAIN_PRESCAN` |

Alternativ `option Debug 1` in `/etc/sane.d/kds_i2000.conf`; der
Standard-Logpfad ist dann `/var/kodak/kds_i2000/sanetwain/sanetwain_N.log`.

**Achtung:** `SANETWAIN_DEBUG=1` zusammen mit `SANETWAIN_LOGCONSOLE=1`
**stürzt ab** (SIGSEGV in `sane_init`). `KDS_OVERRIDEINI` hat die
Konfiguration in unseren Versuchen nicht ersetzt (`Debug=0` blieb stehen).

Typische Fehlerbilder:

| Symptom | Ursache |
|---|---|
| `scanimage -L` findet nichts, `deviceprobe` listet das Gerät nicht | Scanner nicht am Bus (`lsusb -d 040a:601d`), oder er hat sich abgemeldet |
| `open ... failed: Invalid argument`, Log zeigt `lexexe is gone` | Executable-Stack-Flag nicht gelöscht |
| `open ... failed: Invalid argument`, Log zeigt `Lock file` | `/var/kodak/kds_i2000/device` nicht beschreibbar |
| `sane_start: Document feeder out of documents` | kein Papier im Einzug (korrektes Verhalten) |

---

## Teil B — USB-Mitschnitte

### Werkzeuge in `tools/`

| Datei | Zweck |
|---|---|
| `usbmon-daemon.sh` | läuft **einmal** als root und wird über Steuerdateien bedient, damit nicht jeder Mitschnitt ein sudo-Passwort braucht |
| `kodak-capture.sh` | Einzelmitschnitt: `sudo kodak-capture.sh <label> <kommando...>` |
| `capture-button-press.sh` | hält das Gerät offen, damit Tastendrücke sichtbar würden |
| `usbmon-analyze.py` | Auswertung: Endpunkt-Statistik, Control-Transfers, Zeitachse; liest Text- **und** pcapng-Mitschnitte (letztere über `tshark`) |
| `vendor-request-table.py` | Kreuztabelle aller Vendor-Control-Requests über mehrere Textmitschnitte |
| `vendor-payloads.py` | vollständige Nutzlast jedes Vendor-Requests aus einem pcapng, mit `--unique` als Inventar |
| `clear-execstack.py` | ELF-Patcher für das Executable-Stack-Flag |

### Mitschneiden

```bash
sudo tools/usbmon-daemon.sh /tmp/kodak-captures
```

lädt `usbmon`, mountet bei Bedarf debugfs und wartet dann auf Kommandos:

```bash
echo "START a-open-close" > /tmp/kodak-captures/cmd
scanimage -d kds_i2000:i2000 -n
echo "STOP"                 > /tmp/kodak-captures/cmd
cat /tmp/kodak-captures/state
```

Der Bus wird über `lsusb -d 040a:601d` selbst ermittelt.

### Aufgezeichnete Szenarien

Alle unter `captures/`: die Rohmitschnitte in `captures/usbmon-text/*.usbmon.gz`,
die Auswertungen in `captures/analysis/`, die gescannten Bilder in
`captures/scans/`.

| Label | Szenario | Events | Dauer |
|---|---|---|---|
| `a-open-close` | Gerät öffnen und schließen, kein Scan (`scanimage -n`) | 444 | 0,59 s |
| `b-simplex-lineart-200` | Simplex-Scan, Lineart, 200 dpi, `--source "ADF Front"` | 2562 | 6,99 s |
| `c-duplex-lineart-200` | Duplex-Scan, Lineart, 200 dpi, `--source "ADF Duplex"`, 2 Seiten | 3914 | 7,04 s |
| `d1-no-paper` | Scanversuch mit leerem Einzug → `Document feeder out of documents` | 622 | 0,61 s |
| `d2-buttons` | Gerät 35 s offen, währenddessen alle drei Tasten mehrfach gedrückt | 444 | 33,6 s |

> Die Vorlage von `b-simplex` lag versehentlich falsch herum im Einzug, das
> Bild ist daher fast weiß (0,36 % Schwarzanteil). Für die
> Protokollbetrachtung ist das ohne Belang: das Gerät überträgt dieselbe
> Datenmenge wie bei einer bedruckten Seite (siehe unten). Die
> inhaltstragenden Bilder sind `c-duplex-1/2` mit je ~22 % Schwarzanteil.

### Wichtige Einschränkung der Mitschnitte

usbmons **Text-Schnittstelle** (`/sys/kernel/debug/usb/usbmon/<bus>u`)
schneidet die Nutzdaten jedes Transfers nach **32 Byte** ab. Nachgeprüft:
das längste Datenfeld im gesamten Mitschnitt hat 64 Hex-Zeichen. Für die
Erkennung der Requests reicht das, für den Inhalt eines 120-Byte-Scan-Setups
oder einer 2048-Byte-Tabelle nicht.

Dazu kommt, dass die Textschnittstelle unter hoher Last **Ereignisse
verwirft**: im Scan-Szenario fehlt rund die Hälfte der Bulk-Transfers
(Nachweis in Teil B.2).

Vollständige Nutzdaten liefert nur die **Binärschnittstelle** (`/dev/usbmonN`),
also `tshark`/Wireshark. `tools/usbmon-daemon.sh` benutzt `tshark`
automatisch, wenn es installiert ist, und schreibt dann `.pcapng` mit
`-s 4096`:

```bash
sudo pacman -S wireshark-cli
sudo tools/usbmon-daemon.sh /tmp/kodak-captures     # erkennt tshark selbst
```

> **Stolperstein:** `dumpcap`, das `tshark` zum Schreiben startet, legt
> sämtliche Capabilities ab, sobald es das Aufnahmegerät geöffnet hat —
> **auch `CAP_DAC_OVERRIDE`**. Ein uid-0-Prozess ohne diese Capability
> unterliegt den normalen Rechteprüfungen und kommt dann nicht mehr durch ein
> `drwx------`-Verzeichnis, das jemand anderem gehört:
>
> ```
> tshark: The file to which the capture would be saved (...) could not be
> opened: Permission denied.
> ```
>
> Das Mitschnittverzeichnis muss also auf dem ganzen Pfad für „other“
> betretbar sein — ein Verzeichnis unterhalb von `/tmp` etwa.
>
> **Nachtrag:** betretbar genügt nicht. `dumpcap` muss die Datei im
> Zielverzeichnis *anlegen*, und dafür braucht es dort auch Schreibrecht
> für „other“ — ein `drwxr-xr-x`-Verzeichnis scheitert mit derselben
> Meldung. `chmod 1777` auf das Mitschnittverzeichnis löst es. Der
> Text-Fallback schreibt das Daemon-Skript selbst und ist davon nicht
> betroffen.

---

## Teil B.1 — Erste Protokollanalyse

### Endpunkte

Aus den Gerätedeskriptoren (`captures/analysis/usb-descriptors.txt`),
ein Interface der Klasse 255 (vendor specific):

| Endpunkt | Typ | wMaxPacketSize | tatsächliche Nutzung im Mitschnitt |
|---|---|---|---|
| EP0 | Control | — | **der komplette Kommando- und Konfigurationskanal** |
| 0x81 | Interrupt IN | 8 | **in keinem Szenario benutzt — 0 Transfers** |
| 0x02 | Bulk OUT | 512 | **in keinem Szenario benutzt — 0 Transfers** |
| 0x82 | Bulk IN | 512 | Bilddaten Vorderseite |
| 0x86 | Bulk IN | 512 | Bilddaten Rückseite |
| 0x88 | Interrupt IN | 8 | 8-Byte-Ereignis-/Statusmeldungen während des Scans |

**Das korrigiert die Arbeitsannahme.** Die Vermutung war, EP 0x02 trage die
Kommandos und EP 0x82 zusätzlich die Kommandoantworten. Tatsächlich:

* EP 0x02 (Bulk OUT) wird **nie** benutzt — in allen fünf Mitschnitten
  zusammen null Transfers (`grep -c 'Bo:1:[0-9]*:2'`).
* EP 0x81 (Interrupt IN) ebenfalls nicht: null Transfers
  (`grep -c 'Ii:1:006:1'`). Nur EP 0x88 wird benutzt, und nur während eines
  Scans — 46 Transfers in `b-simplex`, 42 in `c-duplex`, 0 in allen anderen.
* Kommandos, Statusabfragen, Register- und Tabellenzugriffe laufen
  ausschließlich über **vendor-spezifische Control-Requests auf EP0**.
* EP 0x82 und 0x86 tragen **nur** Bilddaten, keine Kommandoantworten.

Warum das Gerät einen Bulk-OUT-Endpunkt bewirbt, ist offen — plausibel ist
Firmware-Download oder ein anderer Modus (ISIS-Treiber, Service-Werkzeuge).

### Datenmengen

| Mitschnitt | EP 0x82 | EP 0x86 | Ergebnisbild |
|---|---|---|---|
| `b-simplex` (leere Seite) | 650 Transfers, 10 609 596 B | — | 1658 × 2395, 498 193 B PBM |
| `c-duplex` Vorder+Rück | 651 Transfers, 10 622 296 B | 649 Transfers, 10 598 928 B | 1657 × 2399 und 1661 × 2395 |

Beobachtungen:

* Der Treiber liest in Blöcken von 16 384 Byte.
* Je ein Transfer pro Endpunkt endet mit Status `-2` (ENOENT): die beim
  Schließen abgebrochene, noch offene URB. Kein Fehler im eigentlichen Sinn.
* Rund 10 MB Rohdaten ergeben ein 0,5-MB-Bitonalbild. Die gesamte
  Bildverarbeitung — Autocrop, Deskew, Bitonal-Wandlung — passiert also
  **auf dem Host** in `hippo.so`. Das ist für ein natives SANE-Backend die
  zentrale Erkenntnis: das Gerät liefert Rohdaten, Perfect Page ist Software.
* Welches genaue Rohformat (Bittiefe, Breite, interne Auflösung) dahinter
  steckt, ist **noch nicht bestimmt**.

> **Diese Zahlen sind zu niedrig.** Der spätere pcapng-Mitschnitt desselben
> Szenarios zeigt, dass die Textschnittstelle unter Bulk-Last Ereignisse
> verliert: 650 statt 1284 Transfers, 10,6 statt 21,0 MB. Siehe
> [Teil B.2](#teil-b2--mitschnitte-mit-vollen-nutzdaten). Die
> Control-Transfers sind davon praktisch nicht betroffen (0xe0: 212 in beiden
> Mitschnitten), die Endpunkt-Zuordnung oben bleibt also gültig.

### Vendor-Control-Requests

Die vollständige Kreuztabelle steht in
`captures/analysis/vendor-requests.txt` — 31 verschiedene Requests, 1068
Transfers. Auszug mit Deutung:

| bRequest | Richtung | wLength | Kommt vor | Deutung |
|---|---|---|---|---|
| `0x00` | IN | 32 | immer, skaliert mit Scan | **Statusblock**, wird gepollt |
| `0x02` | IN | 56 | 1× pro Sitzung | Fähigkeiten/Konfiguration |
| `0x03` | IN | 32 | 1× pro Sitzung | Zähler? (`98 66 84 03 77 01 …`) |
| `0x10` | OUT | 0 | nur Scan | **Scan starten** (`wValue=1`) / **beenden** (`wValue=0`) |
| `0x11` | OUT | 0 | immer | Reset/Abbruch |
| `0x16` | OUT | 0 | 1× pro Sitzung | `wValue=0x0009 wIndex=0x01ff` |
| `0x17` | OUT | 0 | nur Scan | Transfer freigeben (`wValue=1`/`3`) |
| `0x1b` | OUT | 0 | nur Scan | — |
| `0x1f` | OUT | 0 | 1× pro Sitzung | `wValue=0x3086`, `wIndex` variiert |
| `0x30` | OUT | 120 | 2× pro Seite | **Scan-Setup/Jobbeschreibung** |
| `0x31` | OUT | 1 | nur Scan | — |
| `0x32` | IN/OUT | 2 | immer | 16-Bit-Parameter |
| `0x33` | IN | 20 | immer | — |
| `0x34` | IN | 55 | 1× pro Sitzung | **Identität**: `04 0a 60 1d 00 03 00 "KODAK     " … "i2600 SCA…"` |
| `0x35` | IN/OUT | 128/256 | immer | **Firmware-Stand**: `"2014/02/13 10:32…"` |
| `0x36` | IN | 16 | 1× pro Sitzung | **Seriennummer** `"0000000048785942"` |
| `0x37` | IN/OUT | 64 | immer | Konfigurationsblock (`01 aa 00 00 ff ff …`) |
| `0x3a` | OUT | 0 | nur Scan | `wIndex` 0 und 1 → vermutlich pro Kamera |
| `0x45` | IN/OUT | 3 | immer | — |
| `0x62` | OUT | 768 | 1× pro Sitzung | Tabellen-Download (`wValue=0x0104 wIndex=0x0680`) |
| `0xa3` | IN/OUT | 1, 4, 78 | viel, v.a. Scan | **indirekter Registerzugriff**, 16-Bit-Adresse in `wValue` (`0xd900`–`0xdd0b`) |
| `0xe0` | IN/OUT | 1, 3, 32 | viel | **direkter Registerzugriff**, Registerindex in `wValue`, `wIndex` = 0 oder 1 |
| `0xe3` | IN | 256 | 1× pro Sitzung | Speicherlesen (`wIndex=0xfc00`) |
| `0xe4` | OUT | 1024 | nur Scan, 4× | **Tabellen-Upload** (Gamma/LUT), `wValue` = Tabellennummer |
| `0xe5` | OUT | 2048 | nur Scan, 8× | **Tabellen-Upload**, `wIndex` = Offset 0/0x200/0x400/0x600 |

Zwei Dinge fallen auf:

* `wIndex` ist bei `0xe0` und `0x3a` konsequent 0 oder 1 — mit großer
  Wahrscheinlichkeit **Vorder- und Rückseitenkamera**. Register `0x1f`
  taucht als Handshake auf: schreiben, schreiben, lesen, dann das
  eigentliche Register schreiben.
* `0xe4`/`0xe5` laden pro Scan rund 20 KB Tabellen hoch (4 × 1024 +
  8 × 2048). Inhalt sieht nach monoton steigenden 16-Bit-Paaren aus
  (`00d46a35 00dc6445 00e45e56 …`), also Kalibrier- oder Gammakurven.

### Ablauf eines Scans

Aus `captures/analysis/b-simplex-lineart-200.timeline.txt`, Zeitstempel
relativ zum ersten Transfer:

```
+0.000  Standard-Deskriptoren lesen (mehrfach), SET_CONFIGURATION
+0.014  0xC0 0x00 Statusblock lesen (3×)
+0.018  0xC0 0x02 / 0x34 / 0x36 / 0x37  Identität, Seriennummer, Konfiguration
+0.034  0x40 0x1f  wValue=0x3086
+0.036  0xC0 0x35  Firmware-Datum "2014/02/13 10:32"
+0.055  0x40 0xa3  Register 0xda27/0xdb27 = 00 00 00 01
+0.057  0xC0 0xe3  wIndex=0xfc00, 256 Byte
+0.072  0xa3 Register 0xd901: lesen 0x27, schreiben 0x23
+0.094  lange Folge 0xe0-Registerzugriffe, wIndex 0 dann 1
        Muster: 0x1f=00, 0x1f=00, 0x1f lesen, dann Zielregister schreiben
...     0xe4/0xe5 Tabellen-Uploads
+1.914  0xa3 Register 0xda11..0xda1a lesen (Sensorwerte?)
+1.925  0xa3 0xdd0b = 0x46, 0xdd0a lesen/schreiben 0x40
+1.928  0x40 0x30  120-Byte Scan-Setup
            00 00 01 00 00 00 5d 0e 00 00 6a 08 00 85 03 75
            01 01 85 03 75 01 00 00 ...        (nur 32 von 120 Byte erfasst)
+1.931  0x40 0x10 wValue=1   → Scan starten
+2.010  EP 0x88: 10 00 04 00 00 00 00 00
+2.074  EP 0x88: 02 00 00 00 00 00 00 00
+2.074  0x40 0x17 wValue=1
+2.099  EP 0x82: Bilddaten beginnen
+2.102  0x40 0x10 wValue=0
+2.138  EP 0x88: 42 00 00 00 01 00 00 00
+2.202  EP 0x88: 01 00 00 00 00 00 00 00
+2.205  0x40 0x30  zweiter Setup-Block, Byte 1/2 getauscht:
            00 01 00 00 ...   (statt 00 00 01 00)
+2.207  0x40 0x10 wValue=1   → nächster Durchgang
```

Der 120-Byte-Setup-Block ist in Simplex und Duplex **identisch** (soweit die
32 erfassten Byte reichen), die Duplex-Auswahl steckt also woanders — gut
möglich in den nicht erfassten 88 Byte, oder in den `0x3a`-Requests mit
`wIndex` 0/1. Die ersten vier Byte wechseln zwischen `00 00 01 00` und
`00 01 00 00`; das sieht nach einem Durchgangs-/Kamera-Selektor aus.

Erkennbare 16-Bit-Little-Endian-Werte im Block: `0x0e5d = 3677`,
`0x086a = 2154`, `0x0385 = 901`, `0x0175 = 373`. Eine Zuordnung zu
Geometrie oder Auflösung ist **Spekulation**, solange der Block nicht
vollständig erfasst ist.

### Interrupt-Meldungen auf EP 0x88

Während eines Scans, 8 Byte pro Meldung, erstes Byte sieht wie ein
Ereigniscode aus:

```
01 00 00 00 00 00 00 00      03 00 01 00 00 00 00 00
02 00 00 00 00 00 00 00      03 00 02 00 00 00 00 00
10 00 03 00 00 00 00 00      12 00 00/01/02 00 ...
10 00 04 00 00 00 00 00      13 00 01 00 00 00 00 00
15 00 00 00 00 00 00 00      41 01 00 00 00 00 00 00
42 00 00 00 01 00 00 00      42 00 00 00 02 00 00 00
60 00 09 01 00 00 00 00      f1 00 00 00 20 08 00 00
01 05 01 00 01 00 00 00
```

`42 … 01` und `42 … 02` korrespondieren mit Seite 1 und Seite 2, das zweite
Wort ist also plausibel ein Index. `f1 00 00 00 20 08 00 00` enthält
`0x0820 = 2080`. Eine belastbare Deutung fehlt.

### Tastendrücke: negatives Ergebnis

In `d2-buttons` war das Gerät 35 Sekunden geöffnet, während alle drei Tasten
mehrfach gedrückt wurden. Ergebnis:

```
device 6 events: 444
events per second bucket: [(0, 442), (33, 2)]
```

442 Transfers beim Öffnen, 2 beim Schließen, **dazwischen 33 Sekunden
vollständige Funkstille**. Das SANE-Backend pollt die Interrupt-Endpunkte im
Ruhezustand also nicht, und Tastendrücke erzeugen keinen beobachtbaren
USB-Verkehr. Die Tasten würden die Überwachungsfunktion des Herstellers
brauchen (`COsMonitorCache::InitializeButtonMonitoring` in `driver.so`,
dazu `pnphelper`), die über den SANE-Pfad nicht aktiv ist.

Dass das Gerät die Tasten registriert, war am Bedienfeld sichtbar (die
Anzeige wechselte), es behält sie also lokal.

---

## Teil B.2 — Mitschnitte mit vollen Nutzdaten

Nachgeholt mit `wireshark-cli`: `tools/usbmon-daemon.sh` erkennt `tshark` und
schneidet über `/dev/usbmon1` mit `-s 4096` mit. Ergebnis in
`captures/pcapng/`, Auswertungen als `*.pcapng.summary.txt` und
`*.pcapng.payloads.txt` in `captures/analysis/`.

| Label | Szenario | Ergebnisbild | Größe |
|---|---|---|---|
| `a-open-close.pcapng` | Gerät öffnen und schließen | — | 97 KB |
| `b-simplex-lineart-200.pcapng` | Simplex, Lineart, 200 dpi | 1657 × 2382 | 5,3 MB |
| `c-duplex-lineart-200.pcapng` | Duplex, Lineart, 200 dpi | 1657 × 2369 und 1659 × 2371 | 16 MB |
| `d1-no-paper.pcapng` | Scanversuch mit leerem Einzug | — | 69 KB |
| `e-simplex-lineart-300.pcapng` | Simplex, Lineart, **300 dpi** | 2486 × 3571 | 5,3 MB |
| `f-simplex-gray-200.pcapng` | Simplex, **Graustufen**, 200 dpi | 1657 × 2387, 8 bit | 5,4 MB |

`e` und `f` dienen dem Vergleich: gegenüber `b` ist jeweils genau ein
Parameter geändert.

### Die Textschnittstelle verliert Ereignisse

Gleiches Szenario, beide Verfahren:

| | Textschnittstelle | pcapng |
|---|---|---|
| EP 0x82 Transfers | 650 | 1284 |
| EP 0x82 Bytes | 10 609 596 | 20 989 568 |
| Control `0xe0` | 212 | 212 |
| Control `0xa3` | 64 | 61 |

Die Control-Zählungen stimmen praktisch überein, die Bulk-Zählung ist
halbiert: unter der Bilddatenlast läuft der Ereignispuffer der
Textschnittstelle über und Ereignisse gehen verloren. **Für alles, was
Bulk-Transfers zählt, gelten die pcapng-Zahlen.**

### Ein Simplex-Scan besteht aus zwei Durchgängen

Ein Simplex-Scan einer einzelnen Seite überträgt **zwei** Bilddurchgänge von
je rund 10,4 MB, jeder mit eigenem `0x30`-Setup-Block und eigenem
`0x10 wValue=1`; davor kommt ein kleiner Burst mit Kalibrierdaten. Die
genauen Blockgrößen stehen unter [Burst-Struktur der
Bilddaten](#burst-struktur-der-bilddaten).

Damit ist auch die frühere Zahl aus Teil B.1 erklärt: der Textmitschnitt
zeigte mit 10,6 MB nur einen der beiden Durchgänge.

### Auflösung und Farbmodus gehen den Scanner nichts an

Das ist das wichtigste Ergebnis der Vergleichsscans. Der 120-Byte-Block von
Request `0x30` ist in **allen drei Simplex-Mitschnitten bitgleich** — 200 dpi
Lineart, 300 dpi Lineart und 200 dpi Graustufen senden dasselbe Setup. Und
die übertragene Rohdatenmenge ist es praktisch auch:

| Scan | Ergebnisbild | Rohdaten EP 0x82 |
|---|---|---|
| Lineart 200 dpi | 1657 × 2382, 1 bit | 20 989 568 B |
| Lineart 300 dpi | 2486 × 3571, 1 bit | 20 949 944 B |
| Graustufen 200 dpi | 1657 × 2387, 8 bit | 21 027 668 B |

Spannweite: 0,4 %. **Der Scanner scannt immer gleich**; Auflösung,
Bittiefe und Bitonal-Wandlung macht ausschließlich der Host in `hippo.so`.
Für ein natives SANE-Backend heißt das: man kann von Anfang an nur einen
einzigen Gerätemodus ansteuern und muss alles, was der Nutzer einstellt,
selbst rechnen — aber man muss auch nicht herausfinden, wie man dem Gerät
eine Auflösung beibringt.

### Was im Setup-Block tatsächlich Duplex schaltet

Nur der Duplex-Mitschnitt weicht ab, und zwar in zwei Feldern:

```
            Offset  0x30 0x31 0x32 0x33 0x34 0x35 0x36 0x37
Simplex   Durchg.1   02   00   c5   00   00   00   11   0f
Simplex   Durchg.2   01   00   c5   00   00   00   01   0f
Duplex    Durchg.1   02   00   c5   00   c5   00   33   0f
Duplex    Durchg.2   01   00   c5   00   c5   00   03   0f
```

* **0x34–0x35** ist bei Simplex `0000` und bei Duplex `00c5` — dasselbe
  `0x00c5 = 197`, das bei 0x32–0x33 immer steht. Das sieht nach „Parameter
  für Kamera 2“ neben „Parameter für Kamera 1“ aus.
* **0x36** wechselt von `0x11`/`0x01` auf `0x33`/`0x03`, also Bit 1 und
  Bit 5 gesetzt — plausibel ein Flagsfeld mit „Rückseite aktiv“.
* Byte **0x30** (`02` im ersten, `01` im zweiten Durchgang) und die ersten
  vier Byte (`00 00 01 00` ↔ `00 01 00 00`) sowie Byte 0x20 (`00` ↔ `04`)
  unterscheiden die beiden Durchgänge und sind in Simplex und Duplex gleich.

Alle übrigen 112 Byte sind in allen fünf Mitschnitten identisch, inklusive
`0x0e5d = 3677`, `0x086a = 2154` und der Paare `0x0385`/`0x0175`. Da sie
sich weder mit Auflösung noch Modus noch Duplex ändern, sind es **feste
Geräte- oder Sensorparameter**, keine Auftragsgeometrie.

### Burst-Struktur der Bilddaten

Ein Burst endet hier definitionsgemäß mit einem Transfer < 16 384 Byte:

```
b-simplex (EP 0x82)        4 Transfers      51 824 B   t= 6.80.. 6.81
                         637 Transfers  10 421 120 B   t= 8.64.. 9.44
                         642 Transfers  10 516 624 B   t= 9.83..10.63

c-duplex  (EP 0x82)        4 Transfers      51 824 B   t= 6.72.. 6.72
                         631 Transfers  10 335 776 B   t= 8.57.. 9.36
                         635 Transfers  10 402 832 B   t= 9.73..10.53
                         642 Transfers  10 503 416 B   t=10.92..11.72
c-duplex  (EP 0x86)        4 Transfers      51 824 B   t= 6.73.. 6.73
                         630 Transfers  10 309 868 B   t= 8.45.. 9.24
                         634 Transfers  10 382 004 B   t= 9.62..10.41
                         640 Transfers  10 480 048 B   t=10.80..11.60
```

Gesichert:

* Jeder aktive Endpunkt beginnt mit exakt **51 824 Byte** — in jedem
  Mitschnitt dieselbe Länge, aber jedesmal anderer Inhalt, und die Werte
  liegen alle nahe null (`02 00 00 01 00 00 00 00 00 00 05 00 …`). Das ist
  mit einer frisch gemessenen Dunkelreferenz gut vereinbar, bewiesen ist es
  nicht.
* Danach folgen Blöcke von **10,3–10,5 MB** im Abstand von rund 1,19 s,
  jeder etwa 0,8 s lang. Simplex liefert zwei davon, Duplex drei je Seite.
* Duplex überträgt auf beiden Endpunkten gleichzeitig und praktisch gleich
  viel (31,29 MB vorn, 31,22 MB hinten).

Nicht geklärt: warum ein Simplex-Scan einer einzelnen Seite **zwei**
Bilddurchgänge braucht und Duplex **drei** je Seite. Die Blockgrößen sind
keine Vielfachen einer gemeinsamen Zeilenlänge (ggT von 51 824 und
10 421 120 ist nur 16), was gegen saubere Bildband-Grenzen spricht; ihre
Regelmäßigkeit von unter 2 % Streuung spricht aber dagegen, dass es bloß
zufällige Leerlaufpunkte der Lesesschleife sind.

Zur Größenordnung: 10,4 MB entsprächen bei 8 bit pro Pixel einer Fläche von
rund 2550 × 4090 Pixeln, also etwa 300 dpi über die volle Seitenbreite und
etwa 346 mm Länge. Das passt zu den Größenverhältnissen, ist aber **nicht
nachgewiesen** — dazu müsste man eine physisch kürzere Vorlage scannen und
sehen, ob die Blöcke kleiner werden.

### Der vollständige Scan-Setup-Block (Request `0x30`, 120 Byte)

Beide Blöcke desselben Simplex-Scans, nur die belegten ersten 56 Byte:

```
Durchgang 1 (frame 1042):
  0000  00 00 01 00 00 00 5d 0e 00 00 6a 08 00 85 03 75
  0010  01 01 85 03 75 01 00 00 00 00 00 00 00 00 00 00
  0020  00 00 00 00 01 00 00 00 36 0c 32 0c 00 00 00 00
  0030  02 00 c5 00 00 00 11 0f 01 00 00 00 ...  (Rest 0)

Durchgang 2 (frame 1072):
  0000  00 01 00 00 00 00 5d 0e 00 00 6a 08 00 85 03 75
  0010  01 01 85 03 75 01 00 00 00 00 00 00 00 00 00 00
  0020  04 00 00 00 01 00 00 00 36 0c 32 0c 00 00 00 00
  0030  01 00 c5 00 00 00 01 0f 01 00 00 00 ...  (Rest 0)
```

Unterschiede zwischen den Durchgängen: Byte 1/2 (`00 01` ↔ `01 00`),
Byte 0x20 (`00` ↔ `04`), Byte 0x30 (`02` ↔ `01`) und Byte 0x36
(`11` ↔ `01`) — ein Durchgangsselektor, siehe oben.

Wiederkehrende 16-Bit-Little-Endian-Werte in dem Teil, der sich **nie**
ändert: `0x0e5d = 3677`, `0x086a = 2154`, zweimal das Paar
`0x0385 = 901` / `0x0175 = 373`, `0x0c36 = 3126`, `0x0c32 = 3122`.
Weil sie über Auflösung, Farbmodus und Duplex hinweg konstant sind, können
sie keine Auftragsgeometrie sein.

### Jetzt lesbare Antwortblöcke

`captures/analysis/*.pcapng.payloads.txt` enthält alle Vendor-Requests mit
vollständiger Nutzlast. Die aufschlussreichsten:

**`0xC0 0x34` — Identität (55 Byte)**

```
04 0a 60 1d 00 03 00 "KODAK     " 00×6 "i2600 SCANNER" 00…
```

Vendor- und Produkt-ID stehen big-endian am Anfang, danach zwei
nullterminierte Strings.

**`0xC0 0x36` — Seriennummer (16 Byte)**: ASCII `0000000048785942`.
Der USB-Gerätedeskriptor meldet dagegen `0000000000000000`.

**`0xC0 0x00` — Statusblock (32 Byte), wird gepollt**

```
03 00 01 02 0f 00 09 00 | c9 fc 1a 00 | 03 01 00 02 02 00 00 01
01 00 00 01 01 00 ff 00 | 00 00 00 00
```

Byte 8–11 sind ein monoton wachsender 32-Bit-Little-Endian-Zähler
(`1afcc9`, `1afcca`, `1afccb`, … bei aufeinanderfolgenden Abfragen), also
vermutlich ein Millisekunden-Tick. Der Rest ändert sich im Ruhezustand nicht.

**`0xC0 0x03` — 32 Byte**: `98 66 84 03 77 01 00 00` + Nullen. Als
Little-Endian gelesen `0x03846698` und `0x00000177 = 375` — plausibel
Lebensdauer- und Seitenzähler, **nicht verifiziert**.

**`0xC0 0x37` — 64 Byte**, Paare, die zweimal auftauchen:

```
01 aa 00 00 | 06 06 | a3 8d 80 46 | a3 8d 80 46 | 06 06
12 c4 c2 41 | 12 c4 c2 41 | 94 77 84 72 | 94 77 84 72
12 fb 67 6f | 12 fb 67 6f | a9 88 20 a2 | a9 88 20 a2 | 00…
```

Jeder 4-Byte-Wert kommt doppelt vor — gut vereinbar mit „derselbe Parameter
für Vorder- und Rückseitenkamera“.

**`0x40 0x62` — 768 Byte, einmal pro Sitzung**

Der Inhalt sieht aus wie eine **Zeichensatztabelle**: Gruppen wie
`7f 04 04 04 0c`, `3e 63 41 41 63 7f`, `26 49 49 49 32`,
`3e 6b 49 49 4b 2e` sind klassische 5×8-Spaltenmuster von
Ziffern und Buchstaben. Plausibel ist der Zeichengenerator für das
Bedienfeld-Display oder den Imprinter; **nicht verifiziert**.

**`0x40 0xe0` — direkter Registerzugriff.** Das wiederkehrende Muster ist
deutlich sichtbar:

```
OUT reg 0x1f = 00      (wIndex 0)
OUT reg 0x1f = 00
IN  reg 0x1f           → 00
OUT reg 0x02 = 20
OUT reg 0x1f = 02
OUT reg 0x1f = 02
IN  reg 0x1f           → 02
OUT reg 0x05 = 00
...
```

Register `0x1f` wird also zweimal geschrieben und zurückgelesen, bevor das
eigentliche Zielregister beschrieben wird — ein Bank- oder
Adress-Handshake. Derselbe Ablauf wiederholt sich mit `wIndex=1`, was die
Deutung „zwei Kameras“ stützt.

---

## Offene Fragen

1. **Rohbildformat auf EP 0x82/0x86.** Bittiefe und Zeilenlänge der
   ~10,4-MB-Blöcke. Nächster Schritt: eine physisch kürzere Vorlage scannen
   und sehen, ob die Blöcke proportional kleiner werden — das trennt
   „feste Blockgröße“ von „Seitenlänge“.
2. **Warum zwei Durchgänge pro Simplex-Seite und drei je Seite bei Duplex?**
   Zwei Sensorhälften, zwei Belichtungen, oder ein Vor- und ein Hauptlauf?
3. **Was bedeutet der 51 824-Byte-Block** am Anfang jedes Endpunkts? Länge
   immer gleich, Inhalt jedesmal anders, Werte nahe null.
4. **Wozu EP 0x02 (Bulk OUT)?** In keinem Szenario benutzt.
5. **Registerkarte.** `0xe0` (direkt) und `0xa3` (indirekt, Adressen
   `0xd900`–`0xdd0b`) systematisch aufnehmen. `device.so` ist nicht gestrippt
   und enthält 4387 Funktionssymbole mit Klassennamen, Quelldateinamen und
   Log-Zeilennummern — der Mitschnitt liefert jetzt die Referenz, gegen die
   man Interpretationen aus dem Disassembly prüfen kann.
6. **`KDS_DEBUGUSB`** ausprobieren: der Treiber bringt möglicherweise seine
   eigene USB-Protokollierung mit, benannte Requests inklusive.

---

## Dateien in diesem Verzeichnis

```
packaging/            PKGBUILD, Shim-Quelle, Konfigurationsfragmente, Symlinks auf die .deb
tools/                Mitschnitt- und Auswertungsskripte, ELF-Patcher
captures/
  usbmon-text/        Rohmitschnitte (gzip), Textformat, 32-Byte-Grenze,
                      unter Bulk-Last lückenhaft
  pcapng/             Rohmitschnitte mit vollen Nutzdaten (tshark)
  analysis/           Endpunkt-Statistiken, Zeitachsen, Vendor-Request-Tabelle,
                      vollständige Request-Nutzlasten, USB-Deskriptoren
  scans/              die gescannten Bilder als PBM plus PNG-Vorschauen
*.deb, setup          unveränderte Herstellerdateien
```

> **Protokollanalyse:** Das Kommandoprotokoll, der Interrupt-Kanal und der
> Bilddatenstrom sind in [PROTOKOLL.md](PROTOKOLL.md) beschrieben. Dort
> stehen auch Korrekturen zu Teil B.1 und B.2 dieses README (Deutung der
> Vendor-Requests, "zwei Durchgaenge je Simplexseite").
