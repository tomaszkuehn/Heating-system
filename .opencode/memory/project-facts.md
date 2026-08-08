'# Architektura
- system składa się z 
1. modułu czujnika temperatury czyli modułu ESP32 z podłączonym przez serial modułem LoRa Ebyte E32 433T20D i czujnikiem temperatury 1-wire DS18B20 podłączonym do GPIO32. Kod tego modułu jest w folderze Sensor
2. modułu sterownika systemu na bazie ESP32 z podłączonym przez serial modułem LoRa Ebyte E32 433T20D. Kod tego modułu jest w folderze Heating

- czujnik przesyła w regularnych odstepach czasu pomiar temperatury do sterownika.
- sterownik na podstawie przesłanych odczytów temperatury załącza i wyłącza piec.

# Konwencje
- (uzupełnij: reguły kodowania, testy)

# Komendy
- Build: WSL
- Testy:
- Formatowanie:

# Ryzyka i znane problemy
- moduł czujnika temperatury musi być wysoce niezawodny, wyposażony w watchdog i restartujący w razie zawieszenia aplikacji. Jest zasilany w sposób ciągły ze źródła energii elektrycznej.'
