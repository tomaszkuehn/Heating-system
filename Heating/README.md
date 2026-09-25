# Kontroler ogrzewania na ESP32

Firmware sterownika ogrzewania dla **ESP32-ETH01 (WT32-ETH01 V1.4)** z
LAN8720, realizujący autonomiczne sterowanie urządzeniem grzewczym na podstawie
temperatur z 1–6 czujników wewnętrznych (i opcjonalnego czujnika zewnętrznego),
z interfejsem WWW po **kablowym Ethernetcie (primary)** lub Wi-Fi (fallback),
rejestracją danych, detekcją awarii i trybem symulacyjnym.

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

Po pierwszym uruchomieniu urządzenie startuje jako AP `ESP` / `12345678`
(hasło WPA2 musi mieć min. 8 znaków; gdyby zapisana konfiguracja miała krótsze,
`repair_config()` przywraca domyślne, aby AP zawsze startował zaszyfrowany).
Panel WWW: `http://192.168.4.1/` (AP) lub **`http://10.168.34.55/`**
(statyczny IP po Ethernetcie).

## Ethernet (WT32-ETH01 V1.4, primary)

Ethernet jest interfejsem podstawowym; Wi-Fi (STA/AP) działa jako fallback
i ścieżka konfiguracyjna. Kluczowe cechy portu na WT32-ETH01 V1.4:

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
- **Fallback:** gdy PHY nie odpowie na MDIO, sterownik ETH nie instaluje się i
  firmware działa jak dotąd na Wi-Fi (AP/STA).

### Wgrywanie OTA (kabel zamiast przycinania)

Panel: `POST /api/ota` (binarna zawartość = firmware). Przykład:

```bash
curl -X POST http://10.168.34.55/api/ota --data-binary @heating_ctrl.bin
```

Partycje A/B OTA (`ota_0`/`ota_1` po 3 MB, flash 8 MB) — nowy obraz trafia do
nieaktywnej partycji i po udanej weryfikacji urządzenie restartuje się do niej.

## Architektura (spec pkt 13)

Firmware jest podzielony na moduły w `main/`:

| Plik                  | Moduł (spec)         | Odpowiedzialność                                                  |
|-----------------------|----------------------|-------------------------------------------------------------------|
| `sensor_manager.c`    | sensor_manager       | UART z zewn. interfejsem, średnia krocząca 8, temp. efektywna, walidacja, otwarte okno, estymacja strat łącza radiowego |
| `lora_receiver.c`     | lora_receiver        | odbiornik LoRa (Ebyte E32), zadanie FreeRTOS, dekodowanie ramek tekstowych z zdalnego węzła DS18x20, tryb testowy `/api/lora/test` |
| `simulation_manager.c`| simulation_manager   | symulacja czujników i model cieplny budynku                       |
| `control_engine.c`    | control_engine       | automat stanów, histereza, profil dobowy, BOOST, wybieg, awaryjny |
| `heating_output.c`    | heating_output       | linia GPIO, impulsy wybiegu pompy, symulacja wyjścia              |
| `fault_manager.c`     | fault_manager        | detekcja awarii z modelem odpowiedzi cieplnej, powiadomienia       |
| `storage_manager.c`   | storage_manager      | NVS (konfig) + LittleFS (profile, historia, logi), agregacja      |
| `network_manager.c`   | network_manager      | Ethernet (LAN8720, primary), Wi-Fi AP/STA fallback, provisioning, SNTP, przycisk resetu sieci |
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

Moduł LoRa na sterowniku działa w trybie nasłuchu (AUX/TX-RX na LOW). Zdalny
węzeł wysyła linie tekstowe `"TT.TTT T<id>&\r\n"` (po jednej na czujnik);
`lora_receiver` dekoduje je w osobnym zadaniu FreeRTOS (`lora_rx`) i przekazuje
do `sensor_manager_lora_update()` pod blokadą `he_config_lock()`. Po udanym
odkodowaniu i walidacji pomiaru sterownik odsyła bajt `'X'` jako potwierdzenie
(ACK). Węzeł oczekuje ACK w oknie 5 s i na jego podstawie reguluje moc
nadajnika (status 50→40→1): brak ACK obniża moc, a po 49 nieudanych cyklach
wymusza restart. Aktualizacje pomijane są dla czujników wyłączonych lub
symulowanych — radio nie może ich nadpisać.

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

### Kadencja i straty

Nominalna kadencja ramek węzła to ~5,5–5,8 s (`HE_LORA_FRAME_PERIOD_MS = 6000`
— używane do estymacji strat, patrz sekcja A2). Obce ramki LoRa odbierane na
tym samym kanale są obecnie ignorowane przez parser (wymagany format
`TT.TTT T<id>&`) — planowane utwardzenie protokołu (MAC + anti-replay) jest
zaprojektowane, ale jeszcze niewdrożone.

### Parowanie węzłów LoRa (dodanie czujnika / zmiana ID)

Protokół radiowy (tekstowy, 433 MHz):

- Węzeł niesparowany (NVS `cfg`/`node_id` = 0) ogłasza się ramką
  `TT.TTT T00&` — temperatura sonda + ID 00.
- Węzeł sparowany wysyła `TT.TTT T<n>&` (ID = `node_id` + indeks sondy).
- Kontroler potwierdza każdą ramkę pojedynczym bajtem `'X'`.
- Węzeł bez odczytu sond wysyła marker błędu `ERR T&` (temp = NaN).

Procedura parowania (UI → „＋ Dodaj czujnik LoRa" albo banner „Wykryto
nieskonfigurowany czujnik"):

1. Węzeł w trybie niesparowanym nadaje w pętli ramki `T00` (co ~6 s).
2. Kontroler rejestruje ostatnie ogłoszenie (`s_pair_req_temp/us`, TTL 30 s)
   i udostępnia je przez `GET /api/lora/pair` →
   `{"request":true,"temp":21.9,"age":3,"free":[2,3,4,5,6]}`.
   `free[]` = ID radiowe 1..6 nieobsługiwane przez żaden aktywny czujnik.
3. Użytkownik wybiera ID z listy; `POST /api/lora/pair?id=N` tworzy brakujące
   sloty czujników (nazwa domyślna „Czujnik LoRa %d") i wysyła w radiu
   broadcast `PAIR <n>&` powtarzany ~6 s (30 × co 200 ms — broadcast trwa
   dłużej niż cykl węzła, więc trafia w okno nasłuchu ACK węzła).
4. Węzeł zapisuje ID do NVS (przetrwa restart), odpowiada `OK T<n>&` i od tej
   chwili nadaje jako `T<n>`.

Zmiana ID istniejącego węzła (przycisk „ID…" w wierszu czujnika radiowego):

- `POST /api/lora/repair?from=<stare>&to=<nowe>` wysyła broadcast
  `REPAIR <stare> <nowe>&`; akceptuje go wyłącznie węzeł aktualnie
  posiadający ID `<stare>` (parsowane w oknie ACK również w trybie
  sparowanym). Węzeł zapisuje nowy ID w NVS i odpowiada `OK T<n>&`.

Uwaga: ramka `ERR T&` (brak sond) jest ignorowana przez logikę parowania —
dawniej była błędnie traktowana jako ogłoszenie `T00` z temperaturą NaN.

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
  `YYYYMMDD,śr_systemowa,śr_zewnętrzna,minuty_grzania`
  (np. `20240115,21.30,3.10,420`). `minuty_grzania` = liczba minut w dobie z
  aktywnym przekaźnikiem; znak `-99` w temperaturze oznacza brak odczytu.
- **Historia minutowa (24 h) jest wyłącznie w RAM** — bufor pierścieniowy
  `s_ring[HE_RING_SIZE = 1440]` (`storage_manager.c`). Wykres 24 h czyta ten
  bufor bezpośrednio (`storage_ring_copy()`), więc zawsze pokazuje najświeższe
  dane, przewija się w lewo i **nie zużywa flasha**. Skutek uboczny: po zaniku
  zasilania wykres 24 h startuje pusty (dane ulotne), ale konfiguracja i
  agregaty dobowe przetrwają.

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
   słupek bez czekania na północ.
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
- **Health check czujników** — w tabeli czujników kolumna **Health** z kolorową
  kropką: 🟢 zielona = dane napływają bez zakłóceń (straty ≤ 30%), 🟡 żółta =
  > 30% oczekiwanych ramek radiowych gubionych w oknie pomiarowym, 🔴 czerwona =
  brak danych (TIMEOUT/OUT_OF_RANGE), ⚫ szara = DISABLED. Dla czujników
  radiowych (LoRa) priorytetem jest **wskaźnik strat** `loss` (pole w
  `/api/state`): sterownik porównuje odstępy między 8 ostatnimi przyjęciami
  z nominalną kadencją (`HE_LORA_FRAME_PERIOD_MS = 6 s`) — duplikaty
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
  radiowo (`PAIR <n>&`, patrz sekcja „Parowanie węzłów LoRa"). W wierszu
  czujnika radiowego dodatkowy przycisk **„ID…"** pozwala zmienić ID węzła
  (`REPAIR <stare> <nowe>&`) na dowolne wolne.
- **Wykres zużycia energii (365 dni)** — słupki **minut grzania na dobę**
  (pomarańczowe) z nałożoną linią średniej temperatury systemowej (niebieska).
  Każdy słupek to jeden dzień; oś X to stałe okno **365 dni** kończące się
  dzisiaj. Dane pochodzą z `daily.csv` (kolumna `heat_mins`) uzupełnione o
  **bieżący dzień w czasie rzeczywistym** (agregat z bufora RAM, widoczny jako
  najbardziej prawy słupek, aktualizowany co odświeżenie). Gdy urządzeniu brakuje
  danych za któryś dzień (np. nowa instalacja, luka po awarii), dzień ten jest
  pokazywany jako **0 minut** — wykres zawsze ma 365 segmentów i przewija się w
  lewo wraz z nadejściem nowego dnia. `heat_mins` to liczba minut w ciągu doby, w
  których przekaźnik grzania był aktywny (zliczana z minutowych próbek).
- Sekcje konfiguracyjne (profil dobowy, wybieg pompy / tryb awaryjny,
  **zabezpieczenia i limity**, sieć, powiadomienia, symulacja) są domyślnie
  zwinięte do paska nagłówka i rozwijane kliknięciem.
- **Sieć / Powiadomienia pokazują bieżące ustawienia** — SSID i tryb (klient/AP)
  oraz odbiorcę/serwer SMTP/użytkownika/telefon/bramę SMS. Hasła (Wi-Fi, SMTP)
  nie są pokazywane; puste pole hasła przy zapisie zachowuje bieżące (patrz
  sekcja E).
- **Profil dobowy** — grid **12 kolumn × 2 rzędy** (12 godzin w wierszu).
  Temperatura wyłączenia (OFF) nad załączenia (ON). Trzy sloty pamięci
  **na urządzeniu** (1/2/3) z jedno-klikowym zapisem i odczytem
  (`/api/profile/file?name=profile_N`). **Eksport do pliku .json** (pobranie
  na komputer) i **import z pliku** (wgranie z powrotem). Przycisk
  „Zastosuj" waliduje i zapisuje do aktywnej konfiguracji.
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
  wyzwala task-watchdog; worker dodatkowo **ponawia do 30 s**, gdy Wi-Fi nie
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

**Karencja awarii:** parametr zastąpił stałą `s_grace_ms = 300000` w
`fault_manager.c` — `fault_manager_observe()` pobiera go z `s_cfg->fault_grace_sec`
i używa do oceny `NO_HEAT_RISE` oraz `LOW_HEAT_RISE`.

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
cofnąć.

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
  przyjęciu ramki LoRay zapisywany jest czas przyjścia w drugim pierścieniu
  (8 slotów). Odstęp < `HE_LORA_FRAME_PERIOD_MS/2` (duplikat/retransmisja)
  tylko odświeża znacznik czasu; normalny odstęp → `expected = span / 6 s`,
  `missed = expected − interwały`, `loss_pct = 100·missed/(missed+interwały)`.
  Gdy przez całe okno (8·6 s) nie ma przyjęcia, `loss` forsowane jest do 100%.
  Czujnik wyłączony zeruje bookkeeping. Wynik dostępny w `/api/state` jako
  `"loss"` (procent) i `"rx"` (bool: czy czujnik jest zasilany radiem) —
  panel zamienia to na kolor kropki Health (żółta przy >30%).

#### `QUAL_WINDOW_OPEN` — wykrycie otwartego okna (spec 6.1)
- **Warunek:** lokalny szybki spadek — `drop > 1,5 °C` względem bazy **oraz**
  spadek o >1,0 °C większy niż średni spadek pozostałych aktywnych czujników
  (`window_detect`). Porównanie z innymi czujnikami odróżnia otwarte okno od
  naturalnego wychłodzenia całego budynku (unika fałszywych alarmów).
- **Zakończenie:** automatycznie, gdy temperatura odbuduje się do wnętrza
  histerezy bazy (`drop < HE_WINDOW_RECOVERY_HYST = 0,4 °C`), lub **ręcznie**
  przyciskiem „Przywróć" w tabeli czujników (`/api/sensor/restore`,
  `sensor_manager_restore`).
- **Akcje i rezultat:** czujnik jest **czasowo tłumiony** — wypada ze średniej
  systemowej, więc chwilowe wychłodzenie przy oknie **nie wymusza grzania**
  całego obiektu. Baza czujnika nie jest w tym czasie adaptowana. Po zamknięciu
  okna czujnik wraca do `OK`, a baza jest ustawiana na bieżącą temperaturę.

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

#### `FAULT_NETWORK` — brak sieci (Wi-Fi lub Ethernet)
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
| **Połączenie STA** | tryb klient i podane SSID | po `IP_EVENT_STA_GOT_IP` ustawiany `network_up`, start SNTP |
| **Utrata STA** | `WIFI_EVENT_STA_DISCONNECTED` | auto-reconnect do 10 prób; po wyczerpaniu zgłoszony brak sieci (`FAULT_NETWORK`) |
| **Utrata ETH link** | `ETHERNET_EVENT_DISCONNECTED` | czyszczenie bitu ETH + `network_up=false` (`FAULT_NETWORK` do czasu powrotu) |
| **Synchronizacja czasu (SNTP)** | uzyskanie IP w trybie STA | ustawia zegar rzeczywisty; poniżej `HE_TIME_VALID_EPOCH` czas jest „nieważny" i używany jest zegar wirtualny |
| **Reset sieci (przycisk)** | przytrzymanie zewn. przycisku (GPIO15) przez `HE_NET_RESET_HOLD_MS = 5 s` | `storage_reset_network` + restart AP z domyślnym SSID/hasłem; pozostała konfiguracja zachowana |
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
  przełączniki e-mail/SMS) z bloku `notify` w `/api/state`; karta Sieć ładuje
  bieżący SSID i tryb (klient/AP) z bloku `wifi`. **Hasła (SMTP, Wi-Fi) nie są
  nigdy udostępniane do przeglądarki** — pole hasła pozostaje puste, a pusta
  wartość przy zapisie (`POST /api/notify` / `POST /api/network`) oznacza
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
| Reset sieci (btn) | 15   | zewnętrzny, przytrzymanie 5 s |
| Dioda LED         | 2    | wbudowana, miga podczas przytrzymania resetu |
| UART czujników TX | 17   | zewn. interfejs czujników   |
| UART czujników RX | 13   | (18 = MDIO, stąd przeniesione) |
| LoRa TX / RX      | 12 / 14 | Ebyte E32 (M0=GPIO4/pin 18, M1=GPIO5/pin 4 `RXD`) |
| ETH: PHY power    | 16   | LAN8720 + osc. 50 MHz, HIGH=on |
| ETH: MDC / MDIO   | 23 / 18 | SMI                      |
| ETH: clk          | 0    | 50 MHz z oscylatora (input) |

### Przycisk resetu sieci + dioda LED

Przytrzymanie przycisku (GPIO15) powoduje:
- **0–3 s**: wolne miganie diody LED (GPIO2, ~500 ms) — ostrzeżenie.
- **3–5 s**: szybkie miganie (~200 ms) — za chwilę reset.
- **≥5 s**: dioda świeci ciągle, **reset konfiguracji sieci** do AP
  `ESP` / `12345678` (pozostałe ustawienia zachowane), potem gaśnie.
Puszczenie przed upływem 5 s anuluje operację.

Przycisk EN na module to sprzętowy reset procesora — nie jest obsługiwany
programowo (podczas trzymania EN kod nie działa).

Protokół ramek czujników: `[0xAA][len][payload][CRC8][0x55]`; odpowiedź na
poll: `[count][id, t_hi, t_lo]...` (temperatura w setnych stopnia C).