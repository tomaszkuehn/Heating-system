# Kontroler ogrzewania na ESP32

Firmware sterownika ogrzewania dla **ESP32-ETH01 (WT32-ETH01 V1.4)** z
LAN8720, realizujący autonomiczne sterowanie urządzeniem grzewczym na podstawie
temperatur z 1–6 czujników wewnętrznych (i opcjonalnego czujnika zewnętrznego),
z interfejsem WWW po **kablowym Ethernetcie (jedynym łączu)**, rejestracją
danych, detekcją awarii i trybem symulacyjnym. (Stos Wi-Fi i jego ustawienia
UI/API zostały wycofane — `CONFIG_ESP_WIFI_ENABLED=n`.)

## Budowanie i wgranie

Wymaga ESP-IDF v5.x (testowane na v5.3.2, z toolchainem i Pythonem).

Zależność LittleFS (`joltwallet/littlefs`) jest zadeklarowana w
`main/idf_component.yml` i pobierana automatycznie przy pierwszym budowaniu —
nie trzeba dodawać jej ręcznie.

```bash
idf.py set-target esp32
idf.py build
idf.py -p COMx flash monitor
```

Panel WWW: **`http://10.168.34.55/`** (statyczny IP po Ethernetcie, stałe
z `app_config.h`; pól konfiguracji sieci w UI nie ma).

## Ethernet (WT32-ETH01 V1.4, jedyne łącze)

Wi-Fi zostało usunięte z firmware (stos `libnet80211`/`libwpa` nie jest
już linkowany; ustawienia SSID/hasło/tryb nie są przyjmowane z UI ani API).
Kluczowe cechy portu na WT32-ETH01 V1.4:

- **PHY:** LAN8720, **adres MDIO = 1** (auto-detekcja przy starcie: skan 0–31
  przez SMI, dopasowanie PHY ID `0x0007:xxxx`; fallback: dowolny niemy adres).
- **Zasilanie PHY + oscylator 50 MHz:** GPIO16 (HIGH przed inicjalizacją,
  150 ms na ustabilizowanie się szyny).
- **RMII:** GPIO0 (clk, input), MDC=23, MDIO=18 (z pull-upem), piny danych
  twardo podłączone w krzemie.
- **IP:** statyczne `10.168.34.55/24`, brama `10.168.34.1` (DHCP wyłączone na
  netif ETH; stałe w `app_config.h`: `HE_ETH_STATIC_IP/NETMASK/GATEWAY`).
- **Rejestracja handlerów zdarzeń przed `start_ethernet()`** — statyczne IP
  odpala `IP_EVENT_ETH_GOT_IP` synchronicznie w `esp_eth_start()`, późna
  rejestracja gubi zdarzenie (było źródłem błędu `net:false`).
- **Brak fallbacku:** gdy PHY nie odpowie na MDIO, sterownik ETH nie
  instaluje się i urządzenie pozostaje bez sieci — diagnostyka przez log
  (`ESP_LOGE "No PHY answered"`).

### Wgrywanie OTA (kabel zamiast przycinania)

Panel: `POST /api/ota` (binarna zawartość = firmware). Przykład (login + upload
w jednym wywołaniu curl):

```bash
curl -s -c /tmp/hc.txt -X POST -d '{"user":"admin","pass":"12345678"}' http://10.168.34.55/api/login >/dev/null
curl -s -m 240 -b /tmp/hc.txt -X POST -H 'Content-Type: application/octet-stream' \
  --data-binary @heating_ctrl.bin http://10.168.34.55/api/ota
```

**Okno samo-weryfikacji po restarcie:** bezpośrednio po bootcie (~180 s)
aplikacja jest w stanie PENDING_VERIFY i `esp_ota_begin` odrzuca każdy upload
(`ESP_ERR_OTA_ROLLBACK_INVALID_STATE`). To normalne, chwilowe — poczekaj >
3 min od restartu, potem OTA przejdzie (`POST /api/restart` po zalogowaniu
wykonuje zdalny reboot). Odpowiedź błędu zawiera nazwę kodu i wolny heap.
Nowy obraz trafia do nieaktywnej partycji OTA i po udanej weryfikacji urządzenie
restartuje się do niej.

`/api/state` zwraca `uptime` w **milisekundach** (nie sekundach).

## Architektura (spec pkt 13)

Firmware jest podzielony na moduły w `main/`:

| Plik                  | Moduł (spec)         | Odpowiedzialność                                                  |
|-----------------------|----------------------|-------------------------------------------------------------------|
| `sensor_manager.c`    | sensor_manager       | UART z zewn. interfejsem, średnia krocząca 8, temp. efektywna, walidacja, otwarte okno, estymacja strat łącza radiowego |
| `lora_receiver.c`     | lora_receiver        | odbiornik LoRa (Ebyte E32), zadanie FreeRTOS, dekodowanie ramek binarnych `02 E3` (protokół `LoRa.txt`) ze zdalnego węzła DS18x20, sterowanie kanałem RF, tryb testowy `/api/lora/test` |
| `simulation_manager.c`| simulation_manager   | symulacja czujników i model cieplny budynku                       |
| `control_engine.c`    | control_engine       | automat stanów, histereza, profil dobowy, BOOST, wybieg, awaryjny |
| `heating_output.c`    | heating_output       | linia GPIO, impulsy wybiegu pompy, symulacja wyjścia              |
| `fault_manager.c`     | fault_manager        | detekcja awarii z modelem odpowiedzi cieplnej, powiadomienia       |
| `storage_manager.c`   | storage_manager      | NVS (konfig) + LittleFS (profile, historia, logi), agregacja      |
| `network_manager.c`   | network_manager      | Ethernet (LAN8720, jedyne łącze, statyczne IP), SNTP, mDNS        |
| `web_ui_api.c`        | web_ui_api           | serwer HTTP + REST API + SPA (`web/`)                             |
| `notification_manager.c` | notification_manager | e-mail (SMTP) / SMS (brama HTTP), wysyłane z osobnego zadania workera (kolejka FreeRTOS, off-loop)                              |
| `profile.c`           | —                    | profil dobowy 24h: walidacja, JSON, zapis/odczyt                  |
| `data_model.h`        | —                    | wspólne typy (stan, jakość, czujnik, profil)                      |

Warstwa sterowania (`control_task`) działa w osobnym zadaniu FreeRTOS i nie
zależy od warstwy WWW — awaria serwera HTTP nie blokuje sterowania (spec 13).

## Odbiornik LoRa (zdalny węzeł DS18x20)

Sterownik odbiera pomiary temperatury z zdalnego węzła (patrz `Sensor/` w
repozytorium) przez radio LoRa Ebyte E32 433T20D podłączone do UART2.

### Okablowanie modułu E32 433T20D ↔ WT32-ETH01

| Pin E32 | Pin WT32 (nr) | Pin WT32 (nazwa) |
|---------|---------------|------------------|
| RXD     | 16            | IO12 (UART2 TX)  |
| TXD     | 15            | IO14 (UART2 RX)  |
| M0      | 18            | IO4              |
| M1      | 4             | RXD (IO5)        |
| VCC     | 7 lub 9       | 3V3 lub 5V       |
| GND     | 6, 8 lub 11   | GND              |
| AUX     | —             | — (niepodłączony; firmware nie używa) |

Kierunki i znaczenie: RXD/TXD/M0/M1 = linie sterowane z ESP32 (M0/M1 na LOW
= tryb normalny, nasłuch); VCC przy 5 V wymaga dzielnika/serii na liniach
logicznych do ESP32; GND wspólna z ESP32 (obowiązkowa).

Ważne: GPIO12/14 są wolne, bo nie należą do magistrali RMII; GPIO4/5 wybrano
celowo — GPIO25/26 to RMII RXD0/RXD1 (nie wolno ich używać po dodaniu
Ethernetu). Konfiguracja pinów: `app_config.h` (`HE_LORA_*`).

Uwaga 1: GPIO12 to strapping pin MTDI (napięcie flash przy starcie) — linia
RXD modułu E32 jest wejściem high-Z, więc nie zaburza bootu; nie podłączać
do GPIO12 żadnego wyjścia push-pull.
Uwaga 2: przed first power-on zweryfikować multimetrem (ciągłość) pozycje
mas na headerze — datasheet WT32-ETH01 (ver. 1.3, s. 9–10): masa = 4. pin
od góry i skrajny dolny prawego headera oraz 2./9./11. lewego; IO14/IO12
zwykłe GPIO.

Moduł LoRa na sterowniku działa w trybie nasłuchu (AUX/TX-RX na LOW). Sterownik
obsługuje **wyłącznie protokół binarny** zgodny z `LoRa.txt` — ramka
`<0x02 0xE3><payload>T<id>#`, ACK to `<0x02 0xE3>X<id>T9#`, parowanie
`<0x02 0xE3>PR<id>T9#` (węzeł o ID 0 zapisuje nowe ID; kontroler potwierdza
parowanie dopiero po świeżym ogłoszeniu `T<id>`).

Każda inna linia — zdeformowana ramka binarna lub tekst starodawnego
węzła (`"TT.TTT T<id>&"`, `ACK <id>&`, podpisane `PAIR ...`) — jest
**odrzucana**: trafia do logu zdarzeń (`lora rx: ...`) i licznika
`lora_garbage`, a jej dane **nie mogą** zasilić żadnego czujnika. Stary
protokół tekstowy nie ma ani nagłówka, ani sumy kontrolnej, więc akceptacja
pozwalałaby pojedynczej zmianie bitu na RF bezpowrotnie podmienić pomiar.

`lora_receiver` dekoduje każdą ramkę w osobnym zadaniu FreeRTOS (`lora_rx`)
i przekazuje pomiar do `sensor_manager_lora_update()` pod blokadą
`he_config_lock()`. Bufor akumuluje bajty i kończy ramkę na `&` lub `#`
(kilka ramek obecnych w buforze modułu jest przetwarzanych osobno); scanner
binarny resynchronizuje się na bajtach magic `02 E3` w dowolnym miejscu
linii. Aktualizacje pomijane są dla czujników wyłączonych lub symulowanych —
radio nie może ich nadpisać.

### Test łącza i weryfikacja modułu (`/api/lora/test`)

GET (po logowaniu) wykonuje sondę modułu: pauzuje zadanie `lora_rx`
(`lora_receiver_test_mode`), mierzy poziom spoczynkowy linii TXD modułu na
GPIO14 (`rx_idle` = 1 oznacza, że moduł jest zasilony i podłączony), wpisuje
tryb konfiguracji (M0=M1=1) i wysyła zapytanie parametrów `C1 C1 C1`
(9600 8N1). Odpowiedź 6 B (`C0 ADDH ADDL SPED CHAN OPTION`) jest dekodowana do
adresu, bodów, parzystości, przepustowości powietrznej, kanału/częstotliwości
i mocy. Pola błędu zawierają podpowiedź serwisową (okablowanie/zasilanie).
Wynik ostatniego uruchomienia (urządzenie): `rx_idle=1`, moduł odpowiedział —
parametry fabryczne `0000/0x1A`, kanał 0x17 = 433,125 MHz, 9600 8N1, 2,4k, 10 dBm.

### Kanał radiowy

Obie strony muszą nadawać na **tym samym kanale**. Moduł sterownika jest
sprowadzany do `HE_LORA_CHANNEL = 23` (433 MHz) przy starcie
(`lora_receiver_init`): firmware odczytuje rejestry E32 (`C1 C1 C1` →
`C0 ADDH ADDL SPED CHAN OPTION`) i tylko gdy `CHAN` się różni, zapisuje nową
wartość i weryfikuje odczytem. Endpoint `POST /api/lora/config?chan=N`
pozwala ustawić kanał ręcznie z panelu (zakres 0..83 → 410..493 MHz).

### Kadencja i straty

Zgodnie z `LoRa.txt` węzeł wysyła pomiar **co 20 s + losowe 0..2 s**, więc
nominalny odstęp to ~21 s (`HE_LORA_FRAME_PERIOD_MS = 21000` — używane do
estymacji strat, patrz sekcja A2). Obce ramki LoRa odbierane na tym samym
kanale są odrzucane przez parser (wymagany magic `02 E3`; wszystko inne
trafia do logu i licznika `lora_garbage`).

#### Prawidłowy węzeł — protokół binarny

Węzeł (`Sensor/`, wersja ESP-IDF, i równoległy szkic `Sensor/LoRa_sensor.ino`)
musi nadawać **wyłącznie** format binarny z magic `0x02 0xE3`:

```
<0x02 0xE3><payload>T<ID>#
```

- Pomiar: `02 E3 22.361 T4#` (payload = `22.361`, id=4)
- Brak sondy: `02 E3 ERRT2#` (payload = `ERR`)
- ACK kontrolera: `02 E3 X2T9#` (payload = `X2`, kontroler T9)
- Parowanie (kontroler→węzeł): `02 E3 PR3T9#` (payload = `PR3`)

Kontroler akceptuje TYLKO ten protokół. Ramki tekstowe (`TT.TTT T<id>&` itp.)
są zapisywane w logu i liczone jako `lora_garbage` — nigdy nie są akceptowane
jako dane pomiarowe (patrz wyżej).
Szczegóły w `LoRa.txt` (katalog główny repozytorium).

### Parowanie węzłów LoRa (dodanie czujnika / zmiana ID)

- Węzeł niesparowany (ID 0) ogłasza się binarnie `02 E3 <temp> T0#` (pętla:
  okno nasłuchu 5 s + 1 s przerwy, czyli co ~6 s; bez sondy zamiast temperatury
  wysyła `ERR`).
- Węzeł sparowany nadaje `02 E3 <temp> T<n>#`.
- Kontroler potwierdza wyłącznie pomiary zarejestrowanych ID 1..8:
  `02 E3 X<id>T9#`. Węzeł ignoruje ACK innego ID. **Ogłoszenia `T0` nie są
  potwierdzane** — ramka `X0` nie istnieje w protokole, a okno nasłuchu
  węzła w trybie parowania zamyka się po dowolnej poprawnej linii, więc zbędny
  ACK sprowadzałby ogłoszenia do cyklu ~2 s i wyprzedzał ramkę `PR` przy
  parowaniu. Odpowiedzią parującą jest wyłącznie `PR<n>T9#` z zadania
  parowania.
- Węzeł bez odczytu sond wysyła marker błędu `02 E3 ERRT<id>#`.

Procedura parowania (UI → „＋ Dodaj czujnik LoRa" albo banner „Wykryto
nieskonfigurowany czujnik"):

1. Węzeł w trybie niesparowanym nadaje w pętli ramki `T0` (co ~6 s).
2. Kontroler rejestruje ostatnie ogłoszenie dla każdego wykrytego ID i
   udostępnia je przez `GET /api/lora/pair` →
   `{"nodes":[{"id":0,"temp":21.9,"age":3}],"free":[2,3,4,5,6]}`.
   `free[]` = ID radiowe 1..6 nieobsługiwane przez żaden aktywny czujnik.
3. Użytkownik wybiera ID z listy; `POST /api/lora/pair?id=N` uruchamia
   **asynchroniczne zadanie parowania**: do 3× nadanie binarnej ramki
   `02 E3 PR<n>T9#` (~9 s — dłużej niż cykl słuchania węzła, więc trafia
   w jego okno odbioru), a po każdej próbie 30-sekundowe okno nasłuchu
   (dłuższe niż najdłuższy możliwy odstęp ramek węzła wg `LoRa.txt`:
   20 s + 0..2 s). Bieżący stan zadania zwraca
   `GET /api/lora/unpair/status` → `{"state":"pending|confirmed|failed","rid":N}`
   (`rid` pozwala UI wznowić wiersz „w trakcie parowania" po odświeżeniu strony).
4. Parowanie jest **potwierdzone wyłącznie wtedy, gdy węzeł zacznie
   ogłaszać się nowym ID `T<n>`** — dopiero wtedy tworzony jest slot
   czujnika (nazwa domyślna „Czujnik LoRa %d", waga 1.0) i zapisywana
   konfiguracja. Potwierdzające ogłoszenie jest rejestrowane **przed**
   bramką ACK (opis „po usunięciu czujki" niżej) — świeżo przypisane ID nie ma
   jeszcze slota, a właśnie to ogłoszenie zamyka zadanie. Po wyczerpaniu
   prób zadanie kończy się stanem `failed` i **żaden slot nie powstaje**
   (brak martwych slotów po nieudanym parowaniu). Starsza, tekstowa ramka
   `PAIR <n>&` z podpisem HMAC nie jest już wysyłana; węzeł ESP-IDF
   akceptuje ramkę binarną `PR<n>T9#` (fallback: podpisana ramka tekstowa
   dla starszych buildów kontrolera).
   W trakcie parowania lista czujników pokazuje wiersz placeholder
   „**Nowy czujnik — w trakcie parowania…**" (kolumna Jakość: `PAROWANIE…`):
   po powodzeniu zastępuje go właściwy wiersz, po porażce znika bez tworzenia
   czujnika.
5. Węzeł o ID 0 zapisuje nowe ID w NVS (przetrwa restart) i od tej chwili
   nadaje jako `T<n>`.

Uwaga: **zmiana ID istniejącego węzła oraz zdalny reset (REPAIR/RESET) nie są
już obsługiwane** — nie istnieje `POST /api/lora/repair`. Węzeł trzymający ID
można przywrócić do fabrycznego 0 wyłącznie jego przyciskiem BOOT (przytrzymanie
> 5 s, patrz `Sensor/README.md`). Endpoint usuwania czujnika
`POST /api/lora/unpair?id=N` tylko zwalnia slot w konfiguracji (węzeł zachowuje
swoje ID).

Uwaga: po usunięciu czujki kontroler **nie potwierdza** ramek dawnego węzła —
ramka `X<id>` jest potwierdzeniem pomiaru **zarejestrowanego** czujnika
(`LoRa.txt`), więc ID zwolnione przez `/api/lora/unpair` dostaje bez ACK:
ramki widoczne w logu (`lora rx: T<n> — brak czujnika, bez ack`) i liczniku
odrzuconych. Węzeł reaguje na brak ACK zgodnie ze spec podnoszeniem mocy
(+2 dB/ramka) i rebootem po 120 s — do ponownego parowania trzeba go
przywrócić do ID 0 przyciskiem BOOT.

Uwaga: ramka `ERR T&` (brak sond) jest ignorowana przez logikę parowania —
dawniej była błędnie traktowana jako ogłoszenie `T00` z temperaturą NaN.

### Walidacja pomiarów i moc radia

- **Filtr rozgrzewkowy**: pierwsze 3 rzeczywiste odczyty po zimnym starcie
  (`HE_WARMUP_READINGS`) są odrzucane — DS18B20 po zasileniu potrafi zgłosić
  zawyżoną wartość (samoogrzewanie / power-on 85 °C). Licznik uzbrajany jest
  dokładnie raz (sentinel 0xFF) — wcześniejsza wersja uzbrajała go od nowa
  przy każdej ramce i czujnik pozostawał w `TIMEOUT` w nieskończoność.
- **Odcięcie bezsensownych wartości**: ramka z temperaturą > 60 °C
  (`HE_TEMP_MAX_ACCEPT`) jest ignorowana w całości — nie wchodzi do średniej
  kroczącej ani nie zmienia jakości czujnika. Granica logiczna 85 °C
  (`HE_TEMP_MAX_LOGICAL`) oznacza odczyt jako `OUT_OF_RANGE`.
- **Moc radia**: przy starcie kontroler wymusza na E32 maksymalną moc TX
  (20 dBm, OPT[1:0] = 00) razem z synchronizacją kanału. Obniżona moc modułu
  psuje łącze **asymetrycznie**: ramki węzła dochodzą do kontrolera, ale
  komendy kontrolera nie docierają do węzła — parowanie nigdy się nie kończy.

## Pamięć trwała (spec pkt 9–10)

- **NVS** — konfiguracja klucz-wartość (sieć, czujniki, wagi, offsety,
  alarmy, tryby, flagi) oraz liczniki zużycia flash.
- **Samonaprawa konfiguracji po aktualizacji** — odczyt konfiguracji jest
  **tolerancyjny na rozmiar** (`storage_load_config`): gdy zapisany blob jest
  krótszy niż aktualny `system_config_t` (np. po aktualizacji firmware z nowymi
  polami w środku struktury), nieodczytany ogon jest zerowany, a `repair_config`
  przywraca domyślne dla pustej nazwy urządzenia i nieprawidłowych limitów
  (`fault_grace_sec` / `max_on_sec` / `max_on_break_sec` < 60 s). Sprzęt
  samonaprawia się po aktualizacji — bez `erase_flash` (WiFi i reszta konfigu
  przetrwają). `fault_manager` ma dodatkowo zero-fallback karencji, więc nawet
  „wyzerowany" limit nie wywoła fałszywej awarii `NO_HEAT_RISE`.
- **LittleFS** — pliki użytkownika, profile dobowe, agregaty dobowe i logi
  (odporny na zaniki zasilania, lepszy od SPIFFS do logowania).
- **Format `daily.csv`** — jeden wiersz na dzień:
  `YYYYMMDD,śr_zbioru_sterującego,śr_zewnętrzna,minuty_grzania`
  (np. `20240115,21.30,3.10,420`; kolumna systemowa od wycofania Wi-Fi =
  średnia ważona zbioru sterującego wg maski `ctrl_mask`). `minuty_grzania` = liczba minut w dobie z
  aktywnym przekaźnikiem; znak `-99` w temperaturze oznacza brak odczytu.
- **Historia minutowa (24 h) jest wyłącznie w RAM** — bufor pierścieniowy
  `s_ring[HE_RING_SIZE = 1440]` (`storage_manager.c`). Wykres 24 h czyta ten
  bufor bezpośrednio (`storage_ring_copy()`), więc zawsze pokazuje najświeższe
  dane, przewija się w lewo i **nie zużywa flasha**. Skutek uboczny: po zaniku
  zasilania wykres 24 h startuje pusty (dane ulotne), ale konfiguracja i
  agregaty dobowe przetrwają. Kolumna per-czujnik w historii pokazuje NaN
  (brak danych) podczas rozgrzewki nowego czujnika (pierwsze odczyty odrzucane
  w `HE_WARMUP_READINGS`), a nie 0 °C.

### Ochrona flash przed szybkim zużyciem

Zasada: pisać do flasha **rzadko, partiami i bez nieograniczonego przyrostu
plików**. Mechanizmy (w `storage_manager.c`):

1. **Historia minutowa tylko w RAM.** Próbki minutowe nie trafiają na flash —
   `storage_record_minute()` zapisuje je do bufora pierścieniowego, a
   `storage_flush_samples()` jest pustym no-op. Zamiast ~1440 zapisów/dobę
   powstaje **1 zapis agregatu na dobę**.
2. **Agregacja dobowa + kasowanie surowych danych.** Przy zmianie dnia próbki
   z bufora RAM są redukowane do jednego wiersza (~20 B) w `daily.csv`
   (`aggregate_day()`), a ewentualne stare pliki `YYYYMMDD.csv` z poprzednich
   wersji firmware są usuwane (`prune_old_samples()`). Agregacja odbywa się
   **tylko gdy zegar jest zsynchronizowany SNTP** (`he_time_valid()` — flaga
   `s_cur_day_real`): zanim urządzenie pobierze czas z sieci, wirtualny zegar
   jest zasiewany na epokę `HE_TIME_VALID_EPOCH` (2023-11-14), więc próbki
   z tego czasu **nie są agregowane** (nie powstają sztuczne wiersze „20231114").
   Dodatkowo przy starcie `dedup_daily_csv()` czyści ewentualne powielone /
   nieposortowane wiersze z poprzednich uruchomień (zachowuje **ostatni** wiersz
   na każdy dzień, sortuje rosnąco) — `daily.csv` jest zawsze krótki i
   posortowany. **Bieżący dzień** nie czeka na agregację przy rolloverze —
   endpoint `/api/daily` dokleja go w czasie rzeczywistym z bufora RAM
   (`storage_read_daily_current()`), więc wykres energii pokazuje najświeższy
   słupek bez czekania na północ). Kolumna temperatury w wierszu dziennym to
   od chwili wprowadzenia tej funkcji **średnia ważona zbioru sterującego**
   (`sensor_manager_ctrl_minute_avg()` — per-minuta z `per_sensor[]` wg maski
   `ctrl_mask`, 0 = wszystkie poprawne), a nie średnia wszystkich czujników;
   wiersze sprzed zmiany zachowują starą semantykę.
3. **Log zdarzeń w RAM.** Dziennik zdarzeń (restart, awarie) jest trzymany w
   **buforze pierścieniowym w RAM** (`HE_LOG_MAX_ENTRIES = 100` wpisów), a nie
   w pliku na LittleFS — eliminuje to całkowicie zapis flash przy logowaniu
   (wcześniej log był dołączany do `events.log` przy każdym zdarzeniu i
   kompaktowany). Historia zdarzeń jest krótka (operacyjna) i **nie przetrwa
   restartu**, co jest akceptowalne — służy bieżącej diagnostyce w UI.
   `raise_fault()` nadal **limituje częstotliwość** zapisu: ten sam typ awarii
   (`fault_class_t`) trafia do logu co **najwyżej raz na 60 s**
   (`s_last_log_fault` / `s_last_log_us`), więc oscylacje awarii (np.
   `NO_HEAT_RISE` ↔ `SENSOR_IFACE`) nie zalewają bufora. Log można też
   **wyczyścić ręcznie** przyciskiem „Wyczyść
   log" w sekcji „Logi / alarmy" (`POST /api/log/clear` → `storage_clear_log()`)
   — czyści bufor w RAM (operacji nie da się cofnąć).
4. **Konfiguracja w NVS zapisywana tylko przy zmianie.** `storage_save_config()`
   jest wołane przy faktycznej edycji z UI/API, nie cyklicznie; NVS ma własny
   wear-leveling.
5. **Wybór LittleFS zamiast SPIFFS.** Copy-on-write i wbudowany wear-leveling
   są lepsze do zapisów logopodobnych i odporne na zanik zasilania.

### Wskaźnik i analiza zużycia flash

`storage_get_flash_wear()` liczy szacunkowe zużycie na podstawie trwałych
liczników w NVS (`nvs_writes` — commity konfiguracji, `fs_writes` —
skumulowane KB zapisane do LittleFS). Parametry pamięci ESP32 (NOR, np.
W25Q32): sektor **4 KB**, wytrzymałość **100 000 cykli kasowania/sektor**,
założona amplifikacja zapisu LittleFS **~2×**. Szacunek:
`cykle ≈ zapisane_KB / 4 KB × 2`, procent = `cykle / 100 000 × 100`.
Wartości (`flash_wear_pct`, `flash_erase_cycles`, `flash_nvs_writes`) są w
`/api/state` i `/api/diagnostics`, a na pulpicie kafelek „Pamięć flash (dane)"
pokazuje użycie KB i procent zużycia (żółty od 0,5%, czerwony od 1%).

## Interfejs WWW (`web/`)

Jednostronicowa aplikacja (bez zależności) serwowana z firmware. Zasoby WWW
(`index.html`, `style.css`, `app.js`) są **kompresowane gzip w trakcie
budowania** (`main/CMakeLists.txt`, `gzip -9 -n -f`) i serwowane z nagłówkiem
`Content-Encoding: gzip` — przeglądarka dekompresuje je transparentnie. To
odchudza firmware o ~35 KB (z ~816 B do ~36 KB wolnego w partycji 1 MB) bez utraty
funkcji. Flaga `-f` nadpisuje istniejący `.gz` z poprzedniej kompilacji — bez niej
przyrostowa edycja pliku WWW kończy się błędem `X.gz already exists; not
overwritten` (patrz komentarz w `main/CMakeLists.txt`). Możliwe sekcje:

- **Wykres temperatur 24 h** — oś X w czasie rzeczywistym (okno kotwiczone do
  najnowszej próbki, więc przewija się w lewo w miarę napływu danych), ze skalą
  godzinową. Rysuje temperaturę systemową, zewnętrzną oraz osobną linię dla
  każdego aktywnego czujnika wewnętrznego. W tło nałożony jest profil dobowy
  (pasmo ON/OFF). **Czerwony pasek na dole** pokazuje minuty, w których
  przekaźnik grzania był aktywny.
  **Czujniki z awarią** (TIMEOUT, STALE, OUT_OF_RANGE) **nie są rysowane**
  na wykresie — ich linia znika do czasu powrotu do stanu OK, a w legendzie
  pojawia się znacznik ❌. Zapobiega to zanieczyszczaniu wykresu zamrożonymi
  lub błędnymi odczytami.
- **Zoom osi czasu** — przyciski **1h · 6h · 12h · 24h** nad wykresem.
  Tiki osi X dostosowują się automatycznie: co 15 min dla okna 1 h, co 1 h
  dla 6 h, co 3 h dla szerszych. Aktywny przycisk jest podświetlony.
- **Wybór czujników na wykresie** — kolorowe checkboxy z nazwą czujnika
  (w jego kolorze linii) pozwalają pokazać/ukryć poszczególne czujniki
  wewnętrzne bez przeładowania strony. Domyślnie wszystkie widoczne.
  Etykieta to **pierwsze 12 znaków nazwy** (pełna nazwa w dymku `title`);
  po zmianie nazwy czujnika i „Zapisz" lista przebudowuje się w ramach
  pełnego odświeżenia strony — nowa nazwa natychmiast widoczna nad wykresem.
  Na końcu tej samej listy checkbox **system** (niebieski) pokazuje/ukrywa
  linię średniej systemowej; linia zewnętrzna (zielona) jest rysowana zawsze.
- **Celownik (crosshair) na wykresie 24 h** — najechanie kursorem na pole
  wykresu rysuje dwie przerywane linie: pionową i poziomą. Na górze pionowej
  pokazywany jest wskazywany czas (HH:MM), po prawej poziomej — temperatura
  z osi. Linie znikają po zjechaniu kursora z wykresu. Uwaga: po aktualizacji
  OTA przeglądarka może trzymać stary `app.js` w cache — potrzebny twardy
  refresh (Ctrl+Shift+R).
- **Health check czujników** — w tabeli czujników kolumna **Health** z kolorową
  kropką: 🟢 zielona = dane napływają bez zakłóceń (straty ≤ 30%), 🟡 żółta =
  > 30% oczekiwanych ramek radiowych gubionych w oknie pomiarowym, 🔴 czerwona =
  brak danych (TIMEOUT/OUT_OF_RANGE), ⚫ szara = DISABLED. Dla czujników
  radiowych (LoRa) priorytetem jest **wskaźnik strat** `loss` (pole w
  `/api/state`): sterownik porównuje odstępy między 8 ostatnimi przyjęciami
  z nominalną kadencją (`HE_LORA_FRAME_PERIOD_MS = 21 s`) — duplikaty
  (odstęp < połowa okresu) nie liczą się jako dane. Dla czujników polled
  kropka zachowuje stare znaczenie (WINDOW_OPEN = żółta, STALE = czerwona;
  detekcja STALE dotyczy wyłącznie czujników polled — stabilna temperatura
  radiowa nie jest błędem). **Kliknięcie** na czerwoną lub żółtą kropkę
  rozwija panel ze szczegółowym opisem problemu: przyczyna, czas od
  ostatniego odczytu (`last_seen`) i skutek dla systemu (np. wykluczenie
   ze średniej). Dane o wieku odczytu pochodzą z pola `last_seen`
   (sekundy od ostatniej aktualizacji), dodanego do `/api/state`.
- **Parowanie czujników LoRa** — przycisk **„＋ Dodaj czujnik LoRa"** nad tabelą
  czujników (oraz w banerze o wykryciu węzła) otwiera modal z listą wolnych ID
  (`free[]` z `GET /api/lora/pair`, odświeżane co 5 s) i statusem wykrytego
  węzła (temperatura ogłoszenia + wiek). Po wyborze ID węzeł jest parowany
  radiowo binarną ramką `02 E3 PR<n>T9#` (patrz sekcja „Parowanie węzłów LoRa").
  Zmiana ID istniejącego węzła nie jest możliwa z UI (wymaga resetu BOOT na
  węźle i ponownego parowania).
- **Wykres zużycia energii (365 dni)** — słupki **minut grzania na dobę**
  (pomarańczowe) z nałożoną linią **średniej temperatury zbioru sterującego**
  (niebieska) — średnia ważona liczona wyłącznie z czujników z zaznaczonym
  „Steruje" w tabeli czujników (brak zaznaczonych = wszystkie poprawne).
  Uwaga: wiersze `daily.csv` zapisane przed tą zmianą trzymają jeszcze
  średnią wszystkich czujników — nowe znaczenie ma dzień bieżący (agregat
  na żywo) i wszystkie kolejne.
  Każdy słupek to jeden dzień; oś X to stałe okno **365 dni** kończące się
  dzisiaj. Dane pochodzą z `daily.csv` (kolumna `heat_mins`) uzupełnione o
  **bieżący dzień w czasie rzeczywistym** (agregat z bufora RAM, widoczny jako
  najbardziej prawy słupek, aktualizowany co odświeżenie). Gdy urządzeniu brakuje
  danych za któryś dzień (np. nowa instalacja, luka po awarii), dzień ten jest
  pokazywany jako **0 minut** — wykres zawsze ma 365 segmentów i przewija się w
  lewo wraz z nadejściem nowego dnia. `heat_mins` to liczba minut w ciągu doby, w
  których przekaźnik grzania był aktywny (zliczana z minutowych próbek).
- **Czyszczenie historii (12 miesięcy)** — przycisk **„🗑 Wyczyść historię
  (12 mies.)"** w nagłówku wykresu zużycia energii usuwa z `daily.csv` wszystkie
  wiersze dzienne z ostatnich 365 dni (~`POST /api/history/clear` z body
  `{"confirm":"WYCZYŚĆ"}`). Usuwa dokładnie to, co wykres pokazuje — dane
  **starsze niż rok zostają w pliku**, wykres 24 h (RAM) i dzisiejsza kolumna
  (liczona na żywo z pierścienia minut) są **nietknięte**. Zabezpieczenia: (1)
  podwójne potwierdzenie w UI — czerwone ostrzeżenie + wpisanie frazy `WYCZYŚĆ`;
  (2) odrzucenie serwera przy pustym/błędnym `confirm` (HTTP 400 — przypadkowy
  POST nic nie usuwa); (3) refusal przy niezsynchronizowanym zegarze (HTTP 409 —
  bez SNTP nie da się wyznaczyć okna). Operacja logowana do dziennika zdarzeń.
- **Kolory czujników** — czujniki wewnętrzne mają przypisaną **stałą paletę**
  (6 kolorów per indeks): linia na wykresie 24 h, checkbox legendy **oraz
  kropka + kolor ID w tabeli czujników** pochodzą z tej samej palety — każdy
  wiersz tabeli jest więc wizualnie powiązany ze swoją krzywą wykresu. Czujnik
  zewnętrzny jest zawsze zielony (`★`).
- Sekcje konfiguracyjne (profil dobowy, wybieg pompy / tryb awaryjny,
  **zabezpieczenia i limity**, sieć, powiadomienia, symulacja) są domyślnie
  zwinięte do paska nagłówka i rozwijane kliknięciem.
- **Sekcja „Sieć"** — wyłącznie komunikat że łącze to Ethernet (statyczne IP);
  ustawienia Wi-Fi i `/api/network` zostały wycofane. Karta zawiera nadal
  konto panelu (zmiana hasła, wylogowanie).
- **Powiadomienia pokazują bieżące ustawienia** — odbiorcę/serwer
  SMTP/użytkownika/telefon/bramę SMS. Hasło SMTP nie jest pokazywane; puste
  pole hasła przy zapisie zachowuje bieżące (patrz sekcja E).
- **Profil dobowy** — grid **12 kolumn × 2 rzędy** (12 godzin w wierszu).
  Temperatura wyłączenia (OFF) nad załączenia (ON). Trzy sloty pamięci
  **na urządzeniu** (1/2/3) z jedno-klikowym zapisem i odczytem
  (`/api/profile/file?name=profile_N`). **Eksport do pliku .json** (pobranie
  na komputer) i **import z pliku** (wgranie z powrotem). Przycisk
  „Zastosuj" waliduje i zapisuje do aktywnej konfiguracji. Walidacja
  (UI i serwer mają te same limity): zakres −40..85 °C, `ON ≤ OFF`,
  minimalna szczelina (histereza) `OFF − ON ≥ 0,1 °C` (komunikat serwera
  `hour N: gap<0.1`).
  - **Pasek korekty** nad gridem (chowa się razem z zewiniętą kartą):
    **Δ ±1 / ±0,25 °C** przesuwa wszystkie wartości profilu (zachowuje
    relacje progów — wymagany tylko globalny clamp −40..85), wskaźnik
    `Δ ±x,xx °C` (zielony/pomarańczowy).
    **Szczelina ±0,05 °C** zmienia w każdym punkcie odstęp `OFF − ON`
    o 0,05 °C, **ruszając wyłącznie temperaturą górną** (ON nietknięte):
    powiększanie zatrzymuje się na **2,0 °C** (punkt przy limicie przestaje
    rosnąć; możliwe także ze szczeliny 0,1 °C — 0,1 → 0,15), a zmniejszanie
    na **0,1 °C** (bez osobnych blokad — graniczne punkty przestają się
    zmieniać; gdy cały profil stoi na granicy → alert). Wskaźnik
    `Szczelina: ±x,xx °C` (niebieski) liczy naciśnięcia przycisku.
    **↺ Oryginał** przywraca wartości ostatnio pobrane ze sterownika i zeruje
    oba wskaźniki (korekty bez „Zastosuj" są operacją w przeglądarce —
    ręczne edycje komórek nie wliczają się do wskaźników).
  - **Bezpieczny zapis** — „Zastosuj" i zapis slotu wysyłają snapshot siatki
    i **czyszczą flagę edycji dopiero po potwierdzeniu serwera**; odrzucenie
    (np. naruszenie limitów) pokazuje komunikat serwera („Profil odrzucony:
    hour N: …") i **zostawia wartości w siatce** — brak cichego powrotu do
    poprzedniego stanu w trakcie POST (odświeżenie nie nadpisuje
    edytowanego grida aż do potwierdzenia). Analogicznie wczytanie slotu
    aktualizuje UI dopiero po sukcesie, a import z pliku (jak wcześniej)
    ostrzega po odrzuceniu.
- **Responsywność** — na urządzeniach mobilnych tabela czujników przewija się
  poziomo (nie wychodzi poza kartę), siatka kafelków dashboardu zwija się do
  2 kolumn, a wykresy dopasowują szerokość do ekranu.
- **Pulpit** — nagłówek panelu zawiera **klikalną nazwę urządzenia**
  (edycja inline, zapis w NVS), **zegar systemowy** (HH:MM:SS
  z czasu SNTP lub wirtualnego), wskaźnik zdrowia systemu, stan automatu,
  adres IP urządzenia i status synchronizacji czasu (🕐 zielony = SNTP,
  żółty = lokalny). Kafelki pokazują: temperatury, liczbę sprawnych
  czujników, uptime, zużycie flash, czas grzania w oknie (24h/zoom) i status
  pieca.
- **Powiadomienie po restarcie** — **60 sekund po uruchomieniu** (liczone po
  rzeczywistym uptime `esp_timer_get_time()`, więc działa poprawnie także w
  trybie symulacji/przyspieszenia ×10) wysyłane jest jednorazowe powiadomienie.
  Wysyłka odbywa się **z osobnego zadania workera** (kolejka FreeRTOS,
  off-loop) — blokujące I/O SMTP/SMS nie blokuje pętli sterowania i nie
  wyzwala task-watchdog; worker dodatkowo **ponawia do 30 s**, gdy sieć nie
  zdąży jeszcze powstać. E-mail zawiera szczegółowy raport: nazwa urządzenia,
  temperatury systemowa i zewnętrzna, liczba sprawnych czujników, stan każdego
  czujnika wewnętrznego (nazwa, jakość, temperatura efektywna) **oraz czujnika
  zewnętrznego**, gdy jest skonfigurowany. SMS to krótka wiadomość
  `[nazwa] RESTART`. Wysyłka tylko gdy skonfigurowany e-mail (`email_enabled`)
  i/lub SMS (`sms_enabled`). Konfiguracja w karcie „Powiadomienia".
- **Nazwa urządzenia** konfigurowalna przez kliknięcie tytułu w nagłówku
  dashboardu lub przez `POST /api/device {"name":"..."}`. Domyślnie
  „Sterownik CO". Przetrzymuje restart (NVS). Używana w temacie i treści
  powiadomień restartowych. **Walidacja:** znaki `"`, `\` i kontrolne (<0x20)
  są usuwane przy zapisie, a pusta/w całości odrzucona nazwa → HTTP 400 (zapobiega
  zepsuciu JSON `/api/state` i wstrzyknięciu nagłówków SMTP); UTF-8 (np. polskie
  znaki) jest dozwolone, a w temacie powiadomień kodowane RFC 2047
  (`=?UTF-8?B?…?=`), gdy zawiera znaki non-ASCII.
- **Status pieca** — kafelek z dużym kołem:
  - 🔥 **czerwone koło + płomień** = grzanie aktywne, podpis „Grzeje"
  - 🔵 **niebieskie koło** = grzanie włączone, ale nie grzeje, podpis „Nie grzeje"
  - ◯ **szare koło + ✕** = ogrzewanie wyłączone (kill switch), podpis „Wyłączony"

### Dostęp przez mDNS

Po uzyskaniu IP (STA, **Ethernet** — także statyczny) urządzenie rejestruje
nazwę **`heating.local`**
przez mDNS (`espressif/mdns`). Panel jest wtedy dostępny jako
**`http://heating.local/`** — nie trzeba znać adresu IP. Wymaga obsługi
mDNS/Bonjour po stronie klienta (Windows 10+ natywnie,
Linux wymaga `avahi-daemon`, Android/iOS natywnie).

Adres IP urządzenia jest też widoczny w nagłówku panelu „Pulpit" (obok stanu systemu).

## Konfigurowalne limity i zabezpieczenia

Karta **„Zabezpieczenia i limity"** w UI (domyślnie zwinięta) oraz endpoint
`POST /api/limits` umożliwiają zmianę trzech parametrów ochronnych bez
przekompilowywania firmware. Wszystkie są przechowywane w NVS jako część
`system_config_t` i przetrwają restart.

| Parametr | Pole JSON | Domyślnie | Zakres | Opis |
|---|---|---|---|---|
| Karencja awarii grzania | `fault_grace_sec` | 300 s (5 min) | 60–3600 s | Czas od startu grzania, po którym `fault_manager` zaczyna oceniać skuteczność — daje instalacji czas na reakcję cieplną |
| Maks. ciągłe grzanie | `max_on_sec` | 14400 s (4 h) | 300–86400 s | Po przekroczeniu tego czasu przekaźnik jest **twardo wyłączany** (niezależnie od histerezy) — ochrona przed „zawieszonym" termostatem |
| Przerwa po maks. grzaniu | `max_on_break_sec` | 600 s (10 min) | 60–86400 s | Wymuszony postój po wyłączeniu przez `max_on_sec` — sterownik ignoruje temperaturę i nie załączy grzania, dopóki przerwa nie minie |

**Mechanizm:** w `control_engine.c` zmienna `s_max_on_break_ms` odlicza pozostały
czas przerwy (wraz z blokadą antyoscylacyjną na początku `control_tick()`).
W `hysteresis_decision()` warunek `s_max_on_break_ms > 0` wymusza `false`
niezależnie od progu `on_temp` — przerwa jest absolutna. Gdy `s_on_ms` przekroczy
`max_on_sec`, ustawiane jest `s_max_on_break_ms = max_on_break_sec * 1000`.

**Górne klampy naprawcze (`repair_config()` w `main.c`):** blob NVS o poprawnym
rozmiarze, lecz ze śmieciowymi wartościami (np. timestamp w polu `max_on_sec`),
przechodzi dolne kontrole `< 60`, ale konwersja `*1000` przepełnia int32 do
wartości ujemnej — max-on wyłącza grzanie w pierwszym ticku (objaw: 1 s ON /
~122 s OFF, w próbkach minutowych aliasowane do „załączenia na chwilę co
godzinę"). Dlatego limity mają też górne granice: `max_on_sec`,
`max_on_break_sec`, `fault_grace_sec` ≤ 86400, pompa 1–600 / 10–3600 / 0–86400 s,
emergency 0–86400 s z `on ≤ period`; poza zakresem → wartość domyślna. Repair
działa na kopii w RAM przy starcie (NVS zostaje nietknięty, naprawiany
deterministycznie co boot).

### Watchdogi i nadzór systemowy (sdkconfig.defaults)

Cztery niezależne warstwy wykrywania awarii — każda z inną odpowiedzią:

| Warstwa | Parametr | Reakcja |
|---|---|---|
| **Panic handler** | `ESP_SYSTEM_PANIC_PRINT_REBOOT` | exception / abort → wydruk + **restart** |
| **Interrupt WDT** | `ESP_INT_WDT=300 ms` (CPU0+CPU1) | zablokowane przerwania → panic → restart |
| **Task WDT** | `ESP_TASK_WDT`, 10 s | **`ESP_TASK_WDT_PANIC=y`** → zawieszenie taska → **twardy restart** (nie tylko ostrzeżenie) |
| **Brownout** | `ESP_BROWNOUT_DET` (lvl 0) | zanik zasilania → sprzętowy reset |

Task WDT pilnuje tasków **idle na obu rdzeniach** (auto) oraz `control_task`
(`esp_task_wdt_add`, main.c). Tryb panic to świadomy wybór dla sterownika CO:
zamrożony task (np. deadlock na blokadzie konfiguracji) bez niego tylko
wypisywałby ostrzeżenia — urządzenie stałoby z przekaźnikiem w stanie ON.
Reboot jest bezpieczny: konfiguracja przeżywa w NVS, profil jest walidowany
przy starcie, grzanie startuje od stanu IDLE z histerezą, a rollback OTA ma
nadal swoje 3-minutowe okno. Z blokad TWDT celowo **nie** są subskrybowane
taski sieciowe (worker HTTP powiadamień blokuje się na SMTP/SMS do 30 s —
patrz `notification_manager.c`), więc wolny serwer poczty nie wymusza restartu.

**Karencja awarii:** parametr zastąpił stałą `s_grace_ms = 300000` w
`fault_manager.c` — `fault_manager_observe()` pobiera go z `s_cfg->fault_grace_sec`
i używa do oceny `NO_HEAT_RISE` oraz `LOW_HEAT_RISE`.

**Ignorowanie heating-rise:** checkbox w karcie limitów (`ignore_heat_rise`,
`POST /api/limits`, tail-appended w NVS — domyślnie wyłączone). Gdy włączony,
`fault_manager` w ogóle nie ocenia skuteczności grzania i od razu zwalnia
ewentualnie zatrzaśnięty `NO/LOW_HEAT_RISE` (sam nie zniknie, bo zatrzask trzyma
przekaźnik OFF). Przeznaczony dla instalacji, gdzie model wzrostu nie ma sensu
(np. bardzo wolne ogrzewanie podłogowe) — grzanie nigdy nie staje z tego powodu.

### Sterowanie wybranymi czujnikami (cfg v3)

Karta limitów zawiera sekcję **„Sterowanie czujnikami"** (`control_mode` +
`window_ignore_sec` w `POST /api/limits`); wybór czujników to kolumna
**„Steruje"** w tabeli czujników (`ctrl` w `POST /api/sensor?id=`).

| Parametr | Pole JSON | Domyślnie | Zakres | Opis |
|---|---|---|---|---|
| Tryb sterowania | `control_mode` | 0 (średnia) | 0–1 | `0` = temperaturą sterującą jest ważona **średnia systemowa**; `1` = steruje **najzimniejszy z wybranych** czujników — spadek któregokolwiek poniżej dolnego progu profilu włącza grzanie, a wyłączenie następuje dopiero gdy **wszystkie** wybrane ≥ górnego progu |
| Czas ignorowania po spadku | `window_ignore_sec` | 300 s | 0–3600 s | Po wykryciu gwałtownego spadku (okno) czujnik jest wykluczony ze sterowania **co najmniej** ten czas — nawet gdy temperatura już wraca; po wygaśnięciu decyduje histereza powrotu (0,4 °C) |

- Wybór czujników (`ctrl_mask` w NVS) działa jak maska bitowa per czujnik
  wewnętrzny; `mask = 0` = wszyscy aktywni uczestniczą (zachowanie zgodne
  wstecz). Pierwsze kliknięcie „Steruje" materializuje maskę dla aktywnych
  czujników, żeby odznaczenie jednego nie wyłączyło reszty.
- Czujnik zewnętrzny **nigdy** nie steruje grzaniem.
- W trybie `any` decyzję podejmuje temperatura z `sensor_manager_control_any_temp()`
  (minimum z wybranych); `avg` bez zmian używa `sensor_manager_system_temp()`.
  Przy braku kwalifikowanych czujników (brak danych / wszystkie tłumione) sterownik
  wraca do średniej systemowej.
- Dashboard pokazuje **„Temp. sterująca"** — temperaturę, której aktualnie używa
  histereza (w `avg` = średnia, w `any` = minimum z wybranych).

## Zdarzenia obsługiwane przez system

Dla każdego zdarzenia opisano: **warunek wystąpienia**, **sposób zakończenia**
oraz **akcje i rezultat** w działaniu systemu. Wszystkie awarie i przejścia
stanów trafiają do dziennika zdarzeń w RAM (widoczny w UI: „Logi /
alarmy"), a awarie dodatkowo wyzwalają powiadomienia (patrz niżej).

Wpisy sprzed synchronizacji SNTP — gdy `time(NULL)` zwraca jeszcze czas
uruchomienia, a nie czas rzeczywisty — są wyświetlane jako **`boot +Ns`**
(liczba sekund od startu), a nie jako data z 1970 r.; dzięki temu pierwszy
wpis po restarcie (np. `FAULT_RESTART`) ma czytelny timestamp. Sekcja „Logi /
alarmy" ma przycisk **„Wyczyść log"** (`POST /api/log/clear` →
`storage_clear_log()`), który czyści bufor w RAM — operacji nie da się
cofnąć. Ramki radiowe są logowane (RX i TX, w tym ACK `X<id>T9#` oraz
parowanie `PR<id>T9#`); wiersze **wysłane** przez kontroler (`lora tx:`) są
wyróżnione **kolorem pomarańczowym** w podglądzie logu.

### A. Zdarzenia jakości i awarii czujników (`sensor_manager.c`)

Każdy czujnik ma status `sensor_quality_t`, wyznaczany w każdym cyklu odpytania
(`sensor_manager_poll`, co `HE_CONTROL_TICK_MS = 1000 ms`). Status decyduje, czy
odczyt czujnika **wchodzi do średniej systemowej** — `sensor_manager_system_temp()`
uwzględnia wyłącznie czujniki o statusie `OK` lub `SIMULATED`.

#### `QUAL_TIMEOUT` — brak komunikacji z czujnikiem
- **Warunek:** brak świeżego odczytu przez `HE_SENSOR_TIMEOUT_SEC = 90 s`
  (`mono_ms() - last_update_ms > 90 s`), albo interfejs UART nie odpowiedział na
  poll (dla czujników rzeczywistych), albo źródło symulacji zwróciło NaN.
- **Zakończenie:** automatycznie, gdy nadejdzie poprawna ramka i czujnik znów
  otrzyma świeży odczyt (status wraca do `OK`).
- **Akcje i rezultat:** czujnik jest **wykluczony ze średniej systemowej**; jego
  waga jest pomijana, a pozostałe czujniki proporcjonalnie przejmują udział
  (średnia ważona po malejącej sumie wag). Jeśli w wyniku wszystkie czujniki
  wypadną — patrz `FAULT_SENSOR_IFACE` niżej. Na wykresie linia czujnika ma
  przerwę (wartość `-99`/NaN).

#### `QUAL_OUT_OF_RANGE` — odczyt poza zakresem logicznym
- **Warunek:** odczyt < `HE_TEMP_MIN_LOGICAL (-40 °C)` lub > `HE_TEMP_MAX_LOGICAL
  (85 °C)` — typowo zwarcie/rozwarcie toru pomiarowego lub błąd interfejsu.
- **Zakończenie:** automatycznie, gdy kolejny odczyt wróci do zakresu (status
  `OK`).
- **Akcje i rezultat:** odczyt **odrzucony**, czujnik wykluczony ze średniej.
  Chroni sterowanie przed reakcją na wartość absurdalną (np. −50 °C nie wywoła
  ciągłego grzania).

#### `QUAL_STALE` — odczyt „zamrożony" (podejrzenie uszkodzenia)
- **Warunek:** temperatura efektywna nie zmieniła się o >0,05 °C przez **ponad
  10 minut** (`stale_detect`) — realny czujnik prawie zawsze lekko dryfuje, więc
  idealna stałość sugeruje zawieszony/uszkodzony czujnik lub interfejs.
  Detekcja obejmuje **wyłącznie czujniki polled** (wired UART); czujnik zasilany
  radiowo (LoRa, `rx_count > 0`) jest z niej zwolniony — stabilna temperatura
  pokoju to nie uszkodzona sonda.
- **Zakończenie:** automatycznie przy pierwszej znaczącej zmianie odczytu
  (>0,05 °C) — licznik bezruchu jest zerowany, status wraca do `OK`.
- **Akcje i rezultat:** czujnik przechodzi ze `OK` w `STALE` i **wypada ze
  średniej systemowej**, dopóki nie zacznie znów reagować. Zapobiega
  „przyklejeniu" sterowania do martwej wartości.

### A2. Średnia krocząca i wskaźnik strat łącza radiowego

- **Średnia krocząca 8 odczytów** — każdy czujnik (`sensor_t`) trzyma bufor
  kołowy **8 ostatnich odczytów** (`HE_MOVING_AVG_WINDOW = 8`). Do aplikacji
  (średnia systemowa, sterowanie, wykresy) trafia **średnia bufora** powiększona
  o offsety kalibracji i komfortu (`push_reading()`), co wygładza pojedyncze
  skoki odczytu.
- **Estymacja strat radiowych** (`sensor_manager_note_rx`): przy każdym
  przyjęciu ramki LoRa zapisywany jest czas przyjścia w drugim pierścieniu
  (8 slotów). Odstęp < `HE_LORA_FRAME_PERIOD_MS/2` (duplikat/retransmisja)
  tylko odświeża znacznik czasu; normalny odstęp → `expected = span / 21 s`,
  `missed = expected − interwały`, `loss_pct = 100·missed/(missed+interwały)`.
  Gdy przez całe okno (8·21 s) nie ma przyjęcia, `loss` forsowane jest do 100%.
   Czujnik wyłączony zeruje bookkeeping. Wynik dostępny w `/api/state` jako
   `"loss"` (procent) i `"rx"` (bool: czy czujnik jest zasilany radiem) —
   panel zamienia to na kolor kropki Health (żółta przy >30%).
- **Długookresowa diagnostyka łącza** (liczniki od startu sterownika, tylko RAM):
  per slot przyjęte ramki (`lt_rx`: OK + ERR) kontra sloty MISS (`lt_miss`) —
  w `/api/state` jako `"lt_rx"`/`"lt_miss"`, w UI jako **Σ %** pod kropką Health
  i w nagłówku panelu Bufor. Globalnie (`"lora_garbage"`, `"lora_rejected"`):
  linie nieczytelne dla parsera (szum RF, kolizje, urwane dane UART) oraz ramki
  poprawne składniowo, lecz odrzucone (temperatura poza zakresem). Rozróżnienie
  cisza / śmieci / odrzut mówi, czy ramka nie wyszła z węzła, zderzyła się
  w eterze, czy padł pomiar na węźle.

#### `QUAL_WINDOW_OPEN` — wykrycie otwartego okna (spec 6.1)
- **Warunek:** lokalny szybki spadek — `drop > 1,5 °C` względem bazy **oraz**
  spadek o >1,0 °C większy niż średni spadek pozostałych aktywnych czujników
  (`window_detect`). Porównanie z innymi czujnikami odróżnia otwarte okno od
  naturalnego wychłodzenia całego budynku (unika fałszywych alarmów).
- **Zalogowanie:** oba przejścia trafiają do dziennika zdarzeń UI („Logi /
  alarmy"): otwarcie jako ostrzeżenie („S<N> (<nazwa>): gwałtowny spadek
  (okno?) — ignoruję <X> s"), zamknięcie jako informacja („okno zamknięte").
- **Zakończenie:** automatyczne, gdy **minie czas ignorowania**
  (`window_ignore_sec`, domyślnie 300 s — wykryty czujnik zostaje wykluczony
  ze sterowania jeszcze przez ten czas, nawet gdy temperatura wraca) **i**
  temperatura odbuduje się do wnętrza histerezy bazy
  (`drop < HE_WINDOW_RECOVERY_HYST = 0,4 °C`), lub **ręcznie**
  przyciskiem „Przywróć" w tabeli czujników (`/api/sensor/restore`,
  `sensor_manager_restore`).
- **Akcje i rezultat:** czujnik jest **czasowo tłumiony** — wypada ze średniej
  systemowej (i z decyzji trybu „dowolny wybrany"), więc chwilowe wychłodzenie
  przy oknie **nie wymusza grzania** całego obiektu. Baza czujnika nie jest w
  tym czasie adaptowana. Po zamknięciu okna czujnik wraca do `OK`, a baza jest
  ustawiana na bieżącą temperaturę.

#### `QUAL_DISABLED` — czujnik wyłączony przez użytkownika
- **Warunek:** odznaczenie „Aktywny" w tabeli czujników (`active = false`,
  `/api/sensor`). Dotyczy zarówno czujników wewnętrznych, jak i **zewnętrznego**.
- **Zakończenie:** ponowne włączenie czujnika w UI.
- **Akcje i rezultat:** czujnik **nie jest odpytywany ani symulowany** i nie
  wpływa na nic. Wyłączenie **samoczynnie czyści flagę `window_open`**
  (`s->window_open = false` w `sensor_manager_poll`), więc po ponownym
  włączeniu czujnik nie dziedziczy przestarzałego stanu „otwarte okno" sprzed
  wyłączenia. Dla czujnika zewnętrznego oznacza to, że
  `sensor_manager_external_temp()` zwraca NaN — na pulpicie „Temp. zewn."
  pokazuje `--`, a model kompensacji cieplnej / symulacji używa wartości
  zastępczej (5 °C), a nie „ducha" wyłączonego czujnika.

#### `QUAL_SIMULATED` — odczyt ze źródła symulacji
- **Warunek:** czujnik ma ustawione źródło symulacji (≠ `SIM_SRC_REAL`) i jest
  aktywny. Traktowany w sterowaniu **równorzędnie z `OK`** (wchodzi do średniej).
- **Zakończenie:** przełączenie źródła z powrotem na „Rzeczywisty".
- **Akcje i rezultat:** pozwala testować logikę bez sprzętu; w UI zaznaczone
  kolorem/etykietą.

### B. Awarie systemowe (`fault_class_t`, `fault_manager.c`)

Awaria jest podnoszona przez `raise_fault()`: ustawia stan awarii, **loguje**
zdarzenie (severity 2) i **jednokrotnie** wysyła powiadomienie (flaga
`s_notified` blokuje spam do czasu skasowania). Logowanie jest dodatkowo
**limitowane czasowo**: ten sam typ awarii (`fault_class_t`) trafia do
dziennika w RAM co najwyżej raz na 60 s (`s_last_log_fault` / `s_last_log_us`),
więc szybkie oscylacje (np. `NO_HEAT_RISE` ↔ `SENSOR_IFACE`) nie zalewają logu
tysiącami wpisów. Tylko jedna awaria jest aktywna naraz (o najwyższym
priorytecie wykrycia).

#### `FAULT_SENSOR_IFACE` — wszystkie czujniki niedostępne
- **Warunek:** `total_sensors > 0` i `healthy_sensors == 0` — żaden czujnik nie
  ma statusu `OK`/`SIMULATED` (np. cały interfejs UART padł, wszystkie w
  `TIMEOUT`).
- **Zakończenie:** **automatyczne** — gdy choć jeden czujnik wróci do zdrowia
  (`healthy_sensors > 0`), awaria jest kasowana samoczynnie (analogicznie do
  `FAULT_NETWORK`).
- **Akcje i rezultat:** domyślnie stan sterownika przechodzi w `ST_FAULT`,
  **przekaźnik wyłączony** (`target_relay = false`) — brak wiarygodnych danych =
  brak grzania ze zwykłej histerezy. Bezwarunkowy tryb awaryjny cykliczny
  (`emergency.enabled`) ma wyższy priorytet i podtrzymuje minimalne grzanie mimo
  braku czujników (działa też przy sprawnych czujnikach). Dodatkowo opcja
  **`emergency_on_sensor_fault`** (domyślnie **WYŁ**) zmienia zachowanie tylko
  dla tej awarii: zamiast `ST_FAULT`/OFF utrzymuje ten sam duty cycle
  (`on_seconds` co `period_seconds`) przez czas trwania `FAULT_SENSOR_IFACE` —
  ochrona przeciwzamrożeniowa na wypadek długiej awarii czujników. Jest to
  wariant **warunkowy** (tylko gdy awaria aktywna), w przeciwieństwie do
  bezwarunkowego `emergency.enabled`; oba współdzielą parametry `on`/`period` i
  akumulator fazy. Po odzyskaniu choć jednego czujnika awaria kasuje się
  samoczynnie i sterowanie wraca do histerezy.

#### `FAULT_NO_HEAT_RISE` — brak wzrostu temperatury przy grzaniu
- **Warunek:** grzanie aktywne dłużej niż karencja `fault_grace_sec` (domyślnie
  5 min, konfigurowalne w karcie „Zabezpieczenia i limity"), a
  zaobserwowane tempo wzrostu `observed ≤ 0` (temperatura nie rośnie mimo
  załączonego pieca). Model nie jest naiwny — porównuje z **wyuczonym tempem**
  `s_learned_rate` (EMA z udanych cykli).
- **Zakończenie:** automatycznie — gdy przy kolejnym grzaniu (po karencji)
  `observed ≥ 0,5 × s_learned_rate`, awaria jest kasowana.
- **Akcje i rezultat:** log + powiadomienie („no temperature rise while
  heating"). Sygnalizuje np. brak paliwa/zapłonu, zamknięty zawór, awarię pieca.
  Sterowanie nie jest twardo blokowane (awaria informuje operatora), ale stan
  zdrowia (`health_ok`) jest fałszywy — czerwony marker w UI.

#### `FAULT_LOW_HEAT_RISE` — wzrost poniżej oczekiwanego
- **Warunek:** jak wyżej, ale `0 < observed < 0,5 × expected`, gdzie `expected`
  to wyuczone tempo skorygowane o temperaturę zewnętrzną
  (`expected = s_learned_rate − 0,01 × (T_sys − T_zewn)`, min. 0,05 °C/min) —
  zimniej na zewnątrz = większe straty = niższy akceptowalny przyrost.
- **Zakończenie:** automatycznie, gdy przyrost wróci do ≥ 0,5 × wyuczone tempo.
- **Akcje i rezultat:** log + powiadomienie („heating rise below expected
  (airlock/valve?)") — typowo zapowietrzenie instalacji lub przymknięty zawór.
  Grzanie trwa, ale operator jest ostrzeżony.

#### `FAULT_NETWORK` — brak sieci (Ethernet bez IP / link down)
- **Warunek:** `network_up == false` i brak innej aktywnej awarii (najniższy
  priorytet — informacyjna).
- **Zakończenie:** **automatyczne** — po ponownym połączeniu (`network_up`)
  `fault_manager` sam kasuje tę awarię.
- **Akcje i rezultat:** **nie blokuje sterowania** — piec pracuje dalej wg
  profilu. W maszynie stanów `FAULT_NETWORK` jest jawnie wykluczony z warunku
  wejścia w `ST_FAULT`. Służy tylko sygnalizacji i powiadomieniu (jeśli droga
  powiadomień działa mimo braku sieci lokalnej).

#### `FAULT_STORAGE` — błąd pamięci/systemu plików
- **Warunek:** `storage_ok == false` (`storage_healthy()` — np. nieudany zapis
  NVS lub niezamontowany LittleFS).
- **Zakończenie:** po skasowaniu awarii, gdy pamięć znów jest sprawna.
- **Akcje i rezultat:** log + powiadomienie. Sterowanie działa dalej (grzanie
  nie zależy od zapisu historii), ale rejestracja danych/konfiguracji jest
  niepewna — sygnalizowane w diagnostyce i markerze zdrowia.

#### `FAULT_RESTART` — nieoczekiwany restart
- **Warunek:** przy starcie `esp_reset_reason()` zwraca powód inny niż
  POWERON/SW/DEEPSLEEP (np. panic, watchdog, brownout) — wykrywane w `main.c`.
- **Zakończenie:** zdarzenie jednorazowe (log przy starcie); nie „trwa".
- **Akcje i rezultat:** wpis do dziennika w RAM (severity 2) — ślad do diagnostyki
  niestabilności zasilania/oprogramowania. Konfiguracja jest odtwarzana z NVS,
  a uszkodzony profil naprawiany (`repair_config`).

> `FAULT_SENSOR` jest klasą zarezerwowaną dla awarii pojedynczego czujnika;
> obecnie utrata wszystkich czujników jest raportowana jako `FAULT_SENSOR_IFACE`.

### C. Zdarzenia sterowania i stany pieca (`control_engine.c`)

Automat stanów rozstrzyga priorytetowo (malejąco): kill switch → BOOST → tryb
awaryjny → awaria → histereza normalna. Każda **zmiana stanu** jest logowana
(`enter_state`, severity 0).

| Zdarzenie | Warunek | Zakończenie | Akcje / rezultat |
|---|---|---|---|
| **Grzanie ON (histereza)** | `T_sys ≤ on_temp` danej godziny profilu, minęło `min_off_sec` OFF (domyślnie 90 s, konfigurowalne 30..3600 w sekcji „Zabezpieczenia i limity") i brak blokady antyoscylacyjnej | `T_sys ≥ off_temp` po min. `min_on_sec` ON (domyślnie 90 s, konfigurowalne 30..3600) | przekaźnik ON, stan `ST_HEATING` |
| **Grzanie OFF (histereza)** | `T_sys ≥ off_temp`, min. czas ON dotrzymany, brak blokady | spadek `T_sys ≤ on_temp` | przekaźnik OFF, stan `ST_IDLE` (lub wybieg pompy) |
| **Blokada antyoscylacyjna** | każde wejście w nowy stan ustawia `anti_osc_lock_sec` (domyślnie 120 s, konfigurowalne 30..600) | odliczenie do zera (skalowane ×10 w symulacji) | wstrzymuje przełączenie ON↔OFF, tłumi migotanie na progu |
| **Zabezpieczenie MAX ON** | ciągłe grzanie ≥ `max_on_sec` (domyślnie 4 h, konfigurowalne) | po upływie `max_on_break_sec` (domyślnie 10 min, konfigurowalne) | wymusza OFF + twardą przerwę niezależnie od histerezy (ochrona przed „zawieszonym" termostatem) |
| **BOOST 5 min** | przycisk „Grzanie 5 min" (`/api/boost`) | upływ `HE_BOOST_DURATION_SEC = 300 s` lub ponowne kliknięcie (anuluj) | wymusza grzanie ponad histerezę; po zakończeniu wraca do decyzji histerezy |
| **Kill switch** | przycisk „Wyłącz ogrzewanie" (`/api/heating?disable=1`) z potwierdzeniem „Na pewno?" | ponowne włączenie tym samym przyciskiem (zielony „Włącz ogrzewanie") | najwyższy priorytet: przekaźnik OFF, stan `ST_IDLE`, ignoruje profil i BOOST; status pieca pokazuje szare koło z ✕ i podpis „Wyłączony" |
| **Wybieg pompy** | przejście `HEATING → OFF` przy `pump.enabled` | upływ `total_seconds` | stan `ST_PUMP_OVERRUN`: krótkie impulsy (`impulse_seconds` co `period_seconds`) rozpraszają ciepło resztkowe |
| **Tryb awaryjny cykliczny** | `emergency.enabled` | wyłączenie opcji | stan `ST_EMERGENCY_CYCLIC`: ON przez `on_seconds` co `period_seconds` niezależnie od czujników — ochrona przeciwzamrożeniowa gdy brak danych |
| **Awaryjne grzanie po awarii czujników** | `emergency_on_sensor_fault` + aktywne `FAULT_SENSOR_IFACE` | odzyskanie choć jednego czujnika (auto-clear awarii) | jak wyżej — duty cycle `on_seconds`/`period_seconds`, ale **warunkowo** (tylko na czas awarii, domyślnie WYŁ); bezwarunkowy `emergency.enabled` ma priorytet |
| **Tryb symulacji** | włączona symulacja czujników/ogrzewania | wyłączenie | stan `ST_SIMULATION`; decyzja ON/OFF liczona jak zwykle, ale GPIO nie jest sterowane (chyba że tryb mieszany) |

### D. Zdarzenia sieciowe i czasu (`network_manager.c`)

| Zdarzenie | Warunek | Zakończenie / akcje |
|---|---|---|
| **Połączenie ETH** | link + poprawne IP | po `IP_EVENT_ETH_GOT_IP` ustawiany `network_up`, start SNTP |
| **Utrata ETH link** | `ETHERNET_EVENT_DISCONNECTED` | czyszczenie bitu ETH + `network_up=false` (`FAULT_NETWORK` do czasu powrotu) |
| **Synchronizacja czasu (SNTP)** | uzyskanie IP w ETH | ustawia zegar rzeczywisty; poniżej `HE_TIME_VALID_EPOCH` czas jest „nieważny" i używany jest zegar wirtualny |

(Funkcje Wi-Fi AP/STA i przycisk resetu sieci zostały usunięte razem z
wycofaniem Wi-Fi; GPIO15 (przycisk) i GPIO2 (LED) są wolne.)
| **Got IP po ETH** | `IP_EVENT_ETH_GOT_IP` | j.w. — ustawia zegar, startuje mDNS, `network_up=true` (handlery muszą być zarejestrowane przed `esp_eth_start()`) |

### E. Powiadomienia (`notification_manager.c`)

Powiadomienia są **odkładane do kolejki** (`notification_dispatch_alert` /
`notification_dispatch_restart`) i realizowane przez **osobne zadanie workera**
(`notify_worker_task`, prio 4, **nie subskrybowane task-watchdog**). Dzięki temu
blokujące I/O SMTP/SMS (DNS + TCP + handshake, rzędu sekund) **nigdy nie blokuje
pętli sterowania** ani nie trzyma `he_config_lock` — nawet przy wolnym /
nieosiągalnym serwerze nie ma ryzyka TWDT-reboot. Miejsca wyzwalania
(`fault_manager` przy awarii, `control_engine` 60 s po restarcie) kopiują potrzebne
pola pod lockiem i odkładają komendę bez blokowania (głębokość kolejki 2; pełna
→ porzucenie + log).

- **Warunek:** podniesienie awarii (`fault_manager`) → alert; uruchomienie
  (po 60 s real uptime) → restart. Pojedynczo na awarię (flaga `s_notified`).
- **Akcje:** e-mail przez SMTP (`email_enabled` + adres) i/lub SMS przez bramę
  HTTP (`sms_enabled` + telefon). Temat alertu: `[Heating] fault N`; temat
  restartu: `[nazwa urządzenia] RESTART` (non-ASCII → RFC 2047). Treść = opis
  awarii / szczegółowy raport restartu (patrz „Powiadomienie po restarcie").
  Błędy wysyłki są logowane, ale nie blokują sterowania.
- **Konfigurowane typy zdarzeń:** przełączniki w karcie „Powiadomienia"
  (podsekcja „Typy zdarzeń (e-mail)", `POST /api/notify/events`) decydują, czy
  dana kategoria generuje powiadomienie:
  - `notify_ev_faults` — powiadomienia o awariach (domyślnie **WŁ**),
  - `notify_ev_restart` — powiadomienie o resecie (domyślnie **WŁ**).
  Oba domyślnie WŁ zachowują dotychczasowe zachowanie (e-mail przy każdej
  awarii i przy resecie). Wyłączenie „awarii" wyłącza i e-mail, i SMS dla awarii
  (bramkowanie na poziomie dispatch, wspólnym dla obu kanałów). Pola są
  przechowywane w `system_config_t` (nie w `notify_cfg_t`); ponieważ `false` od
  upgrade byłby nieodróżnialny od „użytkownik wyłączył", stosujemy sentinel
  `notify_ev_ver`: przy pierwszym boot/upgrade `repair_config` ustawia oba na WŁ
  jednorazowo, a następnie ustawienia użytkownika są chronione (`ver=1`).
  Stan przełączników jest udostępniany w `/api/state` (`notify_ev`). Poza
  zakresem: powiadomienia o zmianach stanu i jakości czujników.
- **Echo ustawień w UI i ochrona haseł:** karta Powiadomienia ładuje bieżące
  wartości (odbiorca, serwer SMTP, użytkownik, telefon, brama SMS oraz
  przełączniki e-mail/SMS) z bloku `notify` w `/api/state`. **Hasła (SMTP) nie
  są nigdy udostępniane do przeglądarki** — pole hasła pozostaje puste, a pusta
  wartość przy zapisie (`POST /api/notify`) oznacza
  „zachowaj bieżące hasło" (nadpisanie następuje tylko po wpisaniu niepustej
  wartości). Pozwala to zmienić np. odbiorcę bez ponownego wpisywania hasła.

#### Diagnostyka i testowanie e-mail (przycisk „Testuj e-mail")

Karta Powiadomienia zawiera przycisk **„Testuj e-mail"** — zapisuje aktualną
konfigurację SMTP, wysyła testową wiadomość i pokazuje **pełny log rozmowy
SMTP** (każda komenda `C:` i odpowiedź `S:` serwera). Dzięki temu widać
dokładnie, na którym etapie występuje problem.

**Implementacja SMTP (klient):**
- Połączenie TCP, a następnie **STARTTLS** (`openssl`-free, ręczna negocjacja
  przez `mbedtls`) → szyfrowany kanał. Dzięki temu działają publiczne serwery
  wymagające TLS: **Gmail** (`smtp.gmail.com:587`), Outlook.com, WP, OVH,
  Home.pl itp.
- **Uwierzytelnianie AUTH PLAIN** (login + hasło aplikacji). Dla Gmaila w polu
  „Użytkownik SMTP" wpisz pełny adres, a w „Hasło SMTP" **hasło aplikacji**
  (nie główne hasło konta Google).
- Działa tylko w trybie **STA** (klient routera) — w trybie AP ESP32 nie ma
  dostępu do internetu.

**Pole „Serwer SMTP"** akceptuje format `host` lub `host:port` (np.
`smtp.gmail.com:587`). Od wersji z dedykowanym polem **„Port"** port można
podać osobno — wtedy ma on priorytet nad ewentualnym portem wklejonym w
„Serwer SMTP" (czyli `host:port`). Pusty/domyślny port (0) oznacza: użyj
portu z `host:port`, a gdy go brak — domyślnego **25**. Port jest
przechowywany w NVS (`smtp_port`, dołączony na końcu `system_config_t` dla
bezpieczeństwa upgrade'u) i widoczny w `/api/state` (`notify.smtp_port`).

**Najczęstsze błędy (widoczne w logu diagnostycznym):**

| Komunikat | Przyczyna | Rozwiązanie |
|---|---|---|
| `FAIL: cannot resolve host` | Nieprawidłowa nazwa hosta lub brak DNS | Użyj adresu IP lub poprawnej nazwy |
| `FAIL: connect refused/timeout` | Zły port, firewall lub serwer nie nasłuchuje | Sprawdź port (587 dla STARTTLS, 25 dla plain) |
| `STARTTLS not supported` | Serwer nie oferuje STARTTLS | Użyj serwera z obsługą STARTTLS (np. Gmail 587) |
| `535 authentication failed` | Złe hasło / brak hasła aplikacji | Dla Gmaila wygeneruj hasło aplikacji |
| `OK: email accepted by server` | Sukces — mail dotarł do serwera SMTP | Sprawdź spam w skrzynce odbiorcy |

**Testowe powiadomienie** można też wywołać ręcznie przez
`POST /api/notify/test` (zwraca JSON `{"result":"..."}` z logiem SMTP).

## Tryb symulacji (spec pkt 8)

W UI oznaczony bannerem/kolorem. Możliwa symulacja pojedynczych czujników
(stała, narastanie, nagły spadek, brak odpowiedzi, poza zakresem, wirtualny)
oraz symulacja ogrzewania z modelem bezwładności cieplnej (skuteczne /
nieskuteczne / przegrzewanie). W symulacji fizyczne GPIO nie jest aktywne,
chyba że włączono tryb mieszany (laboratorium).

Opcja **przyspieszonego czasu ×10** (`sim_time_accel`) uruchamia całą pętlę
sterowania na wirtualnym zegarze 10×: model cieplny, liczniki BOOST / trybu
awaryjnego / wybiegu pompy, histereza i blokada antyoscylacyjna oraz zapis
próbek postępują 10× szybciej. Próbki otrzymują wirtualne znaczniki czasu
odstępniane co „minutę”, więc historia (a z nią wykres) zapełnia się i przewija
ok. 10× szybciej — pozwala to obserwować cykle grzania bez czekania i bez
sieci (SNTP). Współczynnik: `HE_SIM_TIME_SCALE` w `app_config.h`.

## GPIO (WT32-ETH01 V1.4)

| Sygnał            | GPIO | Uwaga                       |
|-------------------|------|-----------------------------|
| Załączenie pieca  | 32   | linia do przekaźnika (16→32: GPIO16 = zasilanie LAN8720) |
| UART czujników TX | 17   | zewn. interfejs czujników   |
| UART czujników RX | 13   | (18 = MDIO, stąd przeniesione) |
| LoRa TX / RX      | 12 / 14 | Ebyte E32 (M0=GPIO4/pin 18, M1=GPIO5/pin 4 `RXD`) |
| ETH: PHY power    | 16   | LAN8720 + osc. 50 MHz, HIGH=on |
| ETH: MDC / MDIO   | 23 / 18 | SMI                      |
| ETH: clk          | 0    | 50 MHz z oscylatora (input) |

GPIO15 (dawny przycisk resetu sieci) i GPIO2 (dawna LED) są od tej zmiany
**wolne** — funkcja Wi-Fi AP/STA i związany z nią przycisk zostały usunięte.
Przycisk EN na module to sprzętowy reset procesora — nie jest obsługiwany
programowo (podczas trzymania EN kod nie działa).

Protokół ramek czujników: `[0xAA][len][payload][CRC8][0x55]`; odpowiedź na
poll: `[count][id, t_hi, t_lo]...` (temperatura w setnych stopnia C).