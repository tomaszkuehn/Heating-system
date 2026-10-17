# System sterowania ogrzewaniem CO

Sterownik pieca CO z panelem webowym i bezprzewodowymi czujnikami
temperatury na LoRa 433 MHz.

## Struktura repozytorium

| Katalog / plik   | Zawartość |
|------------------|-----------|
| `Heating/`       | **Sterownik** — WT32-ETH01 (ESP32 + LAN8720): Ethernet, panel webowy, odbiór LoRa, sterowanie przekaźnikiem pieca i pompą, NVS/LittleFS, aktualizacja OTA. Architektura i konfiguracja: [`Heating/README.md`](Heating/README.md) |
| `Sensor/`        | **Węzeł czujnika** — ESP32-WROOM + DS18B20 (1-Wire) + E32-433T20D (LoRa). Budowanie, parowanie, reset fabryczny: [`Sensor/README.md`](Sensor/README.md) |
| `LoRa.txt`       | Specyfikacja protokołu radiowego (ramki binarne, parowanie, ACK, moc) |
| `WT32-ETH01.pdf` | Dokumentacja sprzętowa płytki sterownika |

## Jak to działa (w skrócie)

- Węzły czujników mierzą temperaturę co **20 s + losowe 0–2 s** i nadają ją
  tekstem (`TT.TTT T<id>&`) lub binarnie (`02 E3 <temp>T<id>#`); sterownik
  potwierdza odbiór (`ACK <id>&` / `02 E3 X<id>T9#`). Pełny protokół:
  [`LoRa.txt`](LoRa.txt).
- Sterownik filtruje odczyty (rozgrzewka, odcięcie > 60 °C), liczy średnią
  kroczącą i temperaturę systemową ważoną, po czym steruje piecem zgodnie
  z profilem dobowym (histereza + zabezpieczenia: karencja, limity czasu
  pracy, tryb awaryjny). Panel webowy: `http://<ip>/`.
- Parowanie nowego węzła odbywa się z panelu („＋ Dodaj czujnik LoRa"):
  kontroler nadaje binarną ramkę `02 E3 PR<n>T9#` i oczekuje potwierdzenia
  ogłoszeniem `T<n>`; slot czujnika powstaje dopiero po potwierdzeniu.
  Opis: [`Heating/README.md`](Heating/README.md) → „Parowanie węzłów LoRa".
- Reset fabryczny węzła (powrót do ID 0): przytrzymanie **BOOT ≥ 5 s**.

## Budowanie

Oba firmware'y wymagają **ESP-IDF v5.x** (rozwijane pod WSL Ubuntu,
ESP-IDF w `~/esp/esp-idf`):

```bash
source ~/esp/esp-idf/export.sh
cd Heating && idf.py build     # sterownik -> build/heating_ctrl.bin
cd Sensor  && idf.py build     # czujnik   -> build/ds18x20_sensor.bin
```

- **Sterownik — OTA:** `POST /api/login` (`admin`), potem `POST /api/ota`
  z surowym `heating_ctrl.bin` (`Content-Type: application/octet-stream`).
  **Uwaga — okno samo-weryfikacji OTA:** bezpośrednio po restarcie (~180 s)
  aplikacja jest w stanie PENDING_VERIFY i `esp_ota_begin` odrzuca każdy upload
  (`ESP_ERR_OTA_ROLLBACK_INVALID_STATE`). To normalne, chwilowe — poczekaj >
  3 min od bootu, potem OTA przejdzie. Odpowiedź błędu zawiera nazwę kodu i
  wolny heap. `POST /api/restart` (po zalogowaniu) wykonuje zdalny reboot.
- `/api/state` zwraca `uptime` w **milisekundach** (nie sekundach).
- **Czujnik — flash:** `idf.py -p COMx flash` (przy trudności z wejściem
  w tryb download przytrzymaj BOOT; UWAGA: trzymanie BOOT ≥ 5 s kasuje
  sparowane ID — patrz `Sensor/README.md`).
