# Węzeł czujnika DS18x20 na ESP32

Bezprzewodowy węzeł pomiarowy dla systemu ogrzewania: odczytuje
termometry DS18B20 na magistrali 1-Wire i wysyła temperatury do sterownika
(`Heating/`) przez radio LoRa, formatem binarnym zgodnym z `sensor_manager`.

## Architektura

Firmware podzielony na moduły w `main/`:

| Plik             | Moduł     | Odpowiedzialność                                          |
|------------------|-----------|-----------------------------------------------------------|
| `onewire.c`      | onewire   | bit-banging 1-Wire (RESET, R/W, SEARCH_ROM, CRC8 0x31)   |
| `ds18b20.c`      | ds18b20   | enumeracja DS18B20, CONVERT_T, odczyt scratchpad          |
| `lora.c`         | lora      | UART do radia LoRa, komendy AT, non-blocking odbiór       |
| `main.c`         | —         | zadanie FreeRTOS, watchdog, logika statusu i rekonfiguracji|

## Budowanie i wgrywanie

Wymaga ESP-IDF v5.x.

```bash
idf.py set-target esp32
idf.py build
idf.py -p COMx flash monitor
```

## Format danych

Tekstowy, zgodny z oryginalnym szkicem: po jednej linii na czujnik,

```
TT.TTT T<NN>&<CR><LF>
```

np. `21.500 T01&` dla czujnika 1 = 21,50 °C. Brak odczytu → linia `ERR T&`.

## Alternatywna implementacja Arduino (`LoRa_sensor.ino`)

Katalog `Sensor/` zawiera równoległą implementację Arduino (`LoRa_sensor.ino`)
opartą na bibliotekach Arduino (`OneWire`, `DallasTemperature`,
`EspSoftwareSerial`). Jest to samodzielny szkic wgrany przez `arduino-cli` na
moduł ESP32 + E32-433T20D + DS18B20.

Różnice względem wersji ESP-IDF:

- Ramki **binarne** z magiciem `0x02 0xE3` (`<magic><payload>T<ID>#`)
  zamiast tekstowego `TT.TTT T<id>&`
- Wysyłka przez `LoRa.write()` (binarnie), nie `Serial.print` tekst
- Parsowanie odbioru na bajtach (`parseFrame(frame, length, ...)`) zamiast
  `strstr` po tekście
- Resynchronizacja po overflow bufora polega na przeszukiwaniu sekwencji
  bajtów `{0x02, 0xE3}`
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