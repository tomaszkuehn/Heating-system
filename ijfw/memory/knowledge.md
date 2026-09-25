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
---
type: decision
summary: Single repo restructure: submodule dropped, commit 20c5dc2 pushed
stored: 2026-09-24T17:28:26.509Z
hash: 58aeff0bace5
tags: [git, restructure, submodule, commit, push]
---
<!-- hash:58aeff0bace5 -->
Repo restructure (2026-09-24): entire Heating-system project now in ONE git repo (D:\kody\Heating-system, remote https://github.com/tomaszkuehn/Heating-system.git, branch main). The old separate Heating repo (with branch ESP32-ETH) was deleted by user. Root repo previously had a stale gitlink (mode 160000) for Heating + .gitmodules pointing to ./Heating — removed both, then added all Heating sources as plain files (44 files under Heating/: main/*.c/h, web/, partitions.csv, sdkconfig.defaults, README, AGENTS.md, etc.). Commit 20c5dc2 "Merge Heating controller source into single repo; drop submodule" pushed to main (includes WT32-ETH01.pdf + Sensor/E32433T20D.pdf datasheets, .gitignore updated: ijfw/, .claude/, firmware/, build/, managed_components/, sdkconfig, *.bin/*.elf ignored). Sensor/ was already tracked (15 files). Working tree clean after commit. Note: ESP32-ETH branch history is GONE (old repo deleted) — only main branch remains, code state of ESP32-ETH work preserved in this commit.
---
type: decision
summary: LoRa v2 designed, NOT implemented yet (user deferred)
stored: 2026-09-24T20:19:27.127Z
hash: 117a78040bef
tags: [lora, security, design, deferred, v2]
---
<!-- hash:117a78040bef -->
LoRa protocol v2 (AES-128-CTR + CMAC, anti-replay BOOT_ID/SEQ) DESIGNED but user said "NA RAZIE NIE WDRAŻAMY" — no implementation now. Design summary for future: binary frame [MAGIC 2][VER 1][BOOT_ID 2][SEQ 2][LEN 1][CIPHERTEXT][MAC 4], lean binary payload (int16 centi + uint8 id + uint8 flags) → 29 B for 1 probe, 45 B for 4 probes (fits 58 B E32 packet). Receiver anti-replay table (BOOT_ID→MAX_SEQ) per node, NVS persist every 32 frames, SEQ advance +32 on sensor boot; BOOT_ID rotates on SEQ wrap 65535 (~11.4 days at 15s cadence). Stats lora_ok/replay_rejected/mac_bad in /api/diagnostics. Key management: seed in code + POST /api/lora/key. ACK hardening with MAC deferred. User constraints: max 4 sensors; foreign LoRa frames are received but do NOT block own frames (crypto makes them harmless); FH/auto-hop deferred (v3, maybe never). Also decided: sensor node gets Sensor/ IDF firmware flashed via COM3 (replacing old Arduino sketch) — but only when v2 work starts. Current system running fine end-to-end with old text protocol.
