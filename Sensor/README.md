# Węzeł czujnika DS18x20 na ESP32

Bezprzewodowy węzeł pomiarowy dla systemu ogrzewania: odczytuje
termometry DS18B20 na magistrali 1-Wire i wysyła temperatury do sterownika
(`Heating/`) przez radio LoRa w binarnym protokole `LoRa.txt`
(`<0x02 0xE3><payload>T<id>#`), obsługiwanym przez `lora_receiver`.

## Architektura

Firmware podzielony na moduły w `main/`:

| Plik             | Moduł     | Odpowiedzialność                                          |
|------------------|-----------|-----------------------------------------------------------|
| `onewire.c`      | onewire   | bit-banging 1-Wire (RESET, R/W, SEARCH_ROM, CRC8 0x31)   |
| `ds18b20.c`      | ds18b20   | enumeracja DS18B20, CONVERT_T, odczyt scratchpad          |
| `lora.c`         | lora      | UART do radia LoRa, komendy AT, non-blocking odbiór       |
| `main.c`         | —         | zadanie FreeRTOS, watchdog, logika ACK/mocy i rekonfiguracji|

## Budowanie i wgrywanie

Wymaga ESP-IDF v5.x.

```bash
idf.py set-target esp32
idf.py build
idf.py -p COMx flash monitor
```

## Format danych

Binarny, zgodny ze specyfikacją `LoRa.txt` (katalog główny repozytorium).
Ramka: `<0x02 0xE3><payload>T<ID>#` — magic to dwa surowe bajty, nie tekst.

- Pomiar: `02 E3 22.361 T4#` — payload = temperatury `%02.3f` (np.
  `21.500` dla 21,500 °C), ID węzła = 4.
- Awaria sondy (DS18B20 nie odpowiedział): payload `ERR` — `02 E3 ERRT4#`.
- ACK kontroler→węzeł: `02 E3 X4T9#` (kontroler = ID 9); węzeł akceptuje ACK
  wyłącznie dla własnego ID w oknie 2 s po wysyłce (ramki `X<n>` innego ID —
  patrz pkt 15 `LoRa.txt` — są ignorowane).
- Parowanie kontroler→węzeł: `02 E3 PR<n>T9#` (tylko dla węzła o ID 0).

Ramki niepasujące do tego wzorca sterownik odzyskuje w logu i liczniku
`lora_garbage` — nigdy nie są akceptowane jako dane pomiarowe (stary format
tekstowy `TT.TTT T<NN>&` nie ma sum kontrolnych i jest odrzucany).

## Parowanie i reset (wersja ESP-IDF)

- Węzeł niesparowany (ID 0) ogłasza się ramką `02 E3 <temp> T0#` i
  nasłuchuje komendy parowania.
- Przyjmowana jest **binarna** ramka `02 E3 PR<n>T9#` (`LoRa.txt`,
  aktualny kontroler) — wyłącznie, gdy `sender` = 9 i ID węzła = 0.
- Po przyjęciu ID zapisywane jest w NVS (klucz `node_id`, przetrwa restart);
  węzeł natychmiast wysyła ogłoszenie `02 E3 <temp> T<n>#` pod nowym ID —
  kontroler potwierdza parowanie dopiero po zaobserwowaniu tej ramki.
- Reset do fabrycznego ID 0: przytrzymanie **BOOT ≥ 5 s** (dioda zapala się
  po osiągnięciu progu, ID zerowane, reboot po zwolnieniu przycisku).
- Rytm pomiarów (LoRa.txt): co **20 s + losowe 0–2 s**, liczone start-do-startu
  (okno ACK i odczyty sond wliczają się w odstęp), po ramce węzeł nasłuchuje
  ACK przez `HE_LORA_ACK_TIMEOUT_MS` = 2 s.
- Zarządzanie mocą (LoRa.txt): brak ACK w 2 s → podniesienie mocy
  (`AT+POWER=3`) i jednorazowa powtórka tej samej ramki; ACK → powrót do
  mocy standardowej (`AT+POWER=1`); brak jakichkolwiek ACK przez
  `HE_LORA_NO_ACK_REBOOT_MS` = 120 s → cykliczny restart węzła.

## Alternatywna implementacja Arduino (`LoRa_sensor.ino`)

Katalog `Sensor/` zawiera równoległą implementację Arduino (`LoRa_sensor.ino`)
opartą na bibliotekach Arduino (`OneWire`, `DallasTemperature`,
`SoftwareSerial`). Jest to samodzielny szkic wgrany przez `arduino-cli` na
moduł ESP32 + E32-433T20D + DS18B20.

Różnice względem wersji ESP-IDF:

- Ramki **binarne** z magiciem `0x02 0xE3` (`<magic><payload>T<ID>#`)
  zamiast tekstowego `TT.TTT T<id>&`
- Wysyłka przez `LoRa.write()` (binarnie), nie `Serial.print` tekst
- Parsowanie odbioru na bajtach (`parseFrame(frame, length, ...)`) zamiast
  `strstr` po tekście
- Resynchronizacja po overflow bufora polega na przeszukiwaniu sekwencji
  bajtów `{0x02, 0xE3}`
- ACK kontrolera to binarna ramka `02 E3 X<id>T9#` (kontroler = ID 9);
  węzeł akceptuje ją tylko dla swojego `nodeId` w oknie po wysłaniu pomiaru
- Parowanie: binarna `02 E3 PR<n>T9#` (węzeł o ID 0 zapisuje nowe ID w NVS)
- Reset do fabrycznego ID 0: przytrzymanie przycisku **BOOT > 5 s** (dioda
  zapala się po 5 s, ID zerowane; patrz `handleBootButton`)
- Watchdog (`esp_task_wdt`) + cykliczny reboot po 120 s bez potwierdzenia
- Specyfikacja: `LoRa.txt` (katalog główny repozytorium)
- Flash: `arduino-cli compile --fqbn esp32:esp32:esp32`, a następnie
  `arduino-cli upload -p COM3 --fqbn esp32:esp32:esp32` (wymaga ręcznego
  BOOT+EN)

## Zachowanie względem oryginału

Oryginał (`DS18x20_Temperature.ino`, Arduino) był pojedynczym szkicem:
SoftwareSerial, DallasTemperature, blokująca pętla `delay(1)` i restart
przez 20 s oczekiwanie na watchdog. Wersja ESP-IDF:

- 1-Wire bit-banging bez bibliotek Arduino (brak zależności od `OneWire`/`DallasTemperature`)
- LoRa na sprzętowym UART (zamiast SoftwareSerial)
- non-blocking odbiór potwierdzenia (`esp_timer`, bez `delay(1)`×5000)
- `ESP.restart()` zamiast `delay(20000)` czekając na watchdog
- wykrywanie błędu czujnika (`-127 C` → INT16_MIN → linia ERR)
- watchdog, osobne zadanie FreeRTOS, strukturalne logi (`ESP_LOG`)
- obsługa wielu czujników (enumeracja SEARCH_ROM) zamiast jednego

## Naprawiony błąd — brak akceptacji ACK (implementacja Arduino)

W `Sensor/LoRa_sensor.ino` (wersja Arduino) potwierdzenie kontrolera
(`02 E3 X<id>T9#`) **nie było** przyjmowane, mimo że ramka docierała do modułu
węzła poprawnie. Skutek: po ~120 s od ostatniego ACK węzeł restartował się
(`No acknowledgement for too long, restarting`), a `Temperature measurement
acknowledged` nigdy się nie logowało.

Przyczyna: w `loop()` znacznik czasu był pobierany **przed** `sendMeasurement()`,
które wewnątrz ustawia `lastSendAt = millis()`. Sprawdzenie
`awaitingAck && now - lastSendAt >= ACK_TIMEOUT_MS` liczyło się na `uint32_t`
z `now < lastSendAt` → niedomknięcie (underflow ~4,29e9), więc okno ACK
zamykało się natychmiast.

**Naprawione**: `now = millis()` po `sendMeasurement()` + porównanie ze znakiem
(`(int32_t)(now - lastSendAt)`). Zweryfikowane na urządzeniu (COM3):
`measurement acknowledged` po każdym ACK, brak restartów. Wgrane przez
`arduino-cli compile/upload --fqbn esp32:esp32:esp32` (wymaga ręcznego BOOT+EN;
pusty flash sam wchodzi w download mode). Uwaga: flashowanie czyści NVS —
węzeł wraca do fabrycznego ID 0 i wymaga ponownego parowania
(`POST /api/lora/pair?id=N` po uprzednim `unpair`, jeśli slot zajęty).

Wersja ESP-IDF (`Sensor/main/main.c`) ma osobne okno odbioru ACK i tego błędu
nie wykazuje.