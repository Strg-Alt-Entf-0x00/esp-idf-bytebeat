# ESP Bytebeat – Basic Example

Dieses Example ist die schlanke Endanwender-Version fuer die Wiedergabe von
Bytebeat-Formeln. Es enthaelt keine Entwicklungs-Telemetrie, kein Profiling,
keine Filebridge und keine internen Sample-Skip-Befehle.

## Voraussetzungen

- ESP32-P4-Board mit ES8311-Audio-Codec und passender Verstaerker-/Lautsprecherbeschaltung
- ESP-IDF 6.0 oder neuer, in einer ESP-IDF-Terminalumgebung
- USB-Datenkabel; der serielle ESP-IDF-Port muss erreichbar sein

Die mitgelieferte Konfiguration zielt auf das ESP32-P4-Boardprofil und die
GPIO-Belegung in `main/Kconfig.projbuild`. Andere Boards oder Pinbelegungen
muessen passend konfiguriert werden. Ein eingebauter DAC oder ESP32-S3 wird
von diesem Example nicht automatisch unterstuetzt.

## Build und Flash

```powershell
cd D:\github-repositorys\ESP32\esp-idf-bytebeat\examples\console_bytebeat
idf.py build
idf.py -p COM4 flash monitor
```

Ersetze `COM4` durch den seriellen Port deines Boards. Fuer andere Hardware
kannst du die Audio-Pins und Sample-Rate mit `idf.py menuconfig` unter
**Bytebeat Console Configuration** anpassen. Die Board-Vorgaben liegen in
`sdkconfig.defaults`; `sdkconfig` ist die lokale, generierte
Build-Konfiguration.

## Bedienung

Beim Start ist die Audioausgabe auf 50 % Lautstaerke eingestellt. Im seriellen
Monitor werden Befehle zeilenweise eingegeben. Sende `HELP`, um die
verfuegbaren Befehle direkt am Geraet anzuzeigen.

```text
FORMULA:(t*5)&(t>>7)
FORMULA:sin(t/100)
V:50
S:1.0
M:0
```

- `FORMULA:<expression>` kompiliert und spielt eine Bytebeat-Formel.
- `V:0` bis `V:100` setzt die Lautstaerke.
- `S:0.01` bis `S:16` setzt den Wiedergabe-Geschwindigkeitsfaktor.
- `M:0` ist unsigned Bytebeat, `M:1` Floatbeat, `M:2` signed Bytebeat.
- `HELP` zeigt die Befehlsuebersicht.

Ein nicht kompilierbarer Ausdruck wird gemeldet und die Ausgabe stumm
geschaltet. Unbekannte, ungueltige oder zu lange Befehle werden mit einer
Fehlermeldung abgewiesen. Die automatische interne Anpassung der
Render-Last bleibt aktiv; sie ist kein Endanwender-Befehl.

## Was bewusst nicht enthalten ist

Messwerte pro Audio-Chunk, Core-/Compiler-Details, manuelles Sample-Skipping,
Legacy-Befehle sowie Dev-Telemetrie und Filebridge gehoeren in
`console_bytebeat_dev`, nicht in dieses Basic-Example.
