<!-- ijfw-schema: v1 -->
# Knowledge Base
---
type: decision
summary: Sensor/ przekształcony z .ino do aplikacji ESP-IDF
stored: 2026-08-08T21:53:25.730Z
hash: 264ff9961dd2
tags: [esp-idf, sensor, ds18b20, lora, refactor]
---
<!-- hash:264ff9961dd2 -->
Sensor/ przebudowany z amatorskiego szkicu Arduino (.ino) na profesjonalną aplikację ESP-IDF (C), spójną z konwencjami projektu Heating/ (static const char *TAG, ESP_LOG, esp_task_wdt, ESP_ERROR_CHECK, app_config.h, modułowa struktura, CMakeLists). |  | Struktura Sensor/main/: | - app_config.h — konfiguracja GPIO/pinów/czasów (nazwane stałe zamiast magic numbers) | - onewire.c/.h — bit-banging 1-Wire (RESET, R/W, SEARCH_ROM, CRC8 poly 0x31) | - ds18b20.c/.h — enumeracja DS18B20, CONVERT_T, odczyt scratchpad, centi-degrees | - lora.c/.h — UART do radia LoRa, AT commands, non-blocking odbiór (esp_timer) | - main.c — app_main, sensor_task (FreeRTOS), watchdog, logika statusu 50→40→1 |  | Kluczowe różnice vs oryginał DS18x20_Temperature.ino: | - 1-Wire bit-banging bez bibliotek Arduino (OneWire/DallasTemperature) — zero zależności Arduino | - LoRa na sprzętowym UART zamiast SoftwareSerial | - non-blocking ACK window (esp_timer + callback) zamiast for(i&lt;5000) delay(1) | - esp_restart() zamiast delay(20000) czekając na watchdog | - wykrywanie błędu czujnika (-127°C → INT16_MIN → linia ERR) | - obsługa wielu czujników (SEARCH_ROM) zamiast jednego getTempCByIndex(0) | - CRC8 scratchpad weryfikowany przed użyciem |  | Format danych: tekstowy zgodny z oryginałem "TT.TTT T&lt;NN&gt;&\r\n" (NIE binarny — binarny protokół w Heating/sensor_manager.c dotyczy przewodowego UART do zewnętrznej płyty, nie LoRa). |  | Build: idf.py set-target esp32 && idf.py build (CMakeLists.txt + partitions.csv + sdkconfig.defaults w Sensor/). |  | Uwaga: nie usuwać DS18x20_Temperature.ino — to oryginał referencyjny.

**Why:** Użytkownik chciał profesjonalnej aplikacji ESP32, nie formatu .ino. Istniejący projekt Heating/ w tym samym repo to już profesjonalny ESP-IDF (C, CMake), więc Sensor musiał być spójny z tym standardem.

**How to apply:** Dla nowych węzłów ESP w tym repo: kopiuj strukturę z Sensor/ (app_config.h + moduły .c/.h + main.c z app_main i FreeRTOS task). Używaj ESP_LOG/ESP_ERROR_CHECK/esp_task_wdt, nie Arduino. 1-Wire przez własny bit-banging, LoRa przez sprzętowy UART.
