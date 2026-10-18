# AGENTS.md — instrukcja dla agentów

Ten plik jest dla agentów AI (Claude Code i innych) pracujących nad Whisli. `README.md` jest dla
ludzi; tutaj są zasady pracy, decyzje projektowe i mapa kodu. Czytaj go przed każdą zmianą, a gdy
zmieniasz coś, co tu opisano (architektura, limity, format zapisu, zasady), zaktualizuj ten plik w
tym samym commicie.

## 1. Czym jest Whisli

Lokalny sterownik oświetlenia na płytce **Waveshare ESP32-S3-POE-ETH-8DI-8DO** (ESP32-S3, 16 MB
flash, 8 MB PSRAM, Ethernet W5500 + PoE, 8 wejść cyfrowych, 8 wyjść). Przyciski ścienne
(monostabilne) generują gesty, gesty uruchamiają akcje, akcje sterują logicznymi światłami
(przekaźnik Modbus/wyjście płytki albo kanały DMX). Wszystko działa lokalnie, bez Home Assistanta,
internetu i chmury. Panel WWW (React) jest wbudowany w firmware.

Zakres jest celowo wąski: **wejścia, światła, akcje, komunikacja, konfiguracja**. Nie budujemy
platformy automatyki — sceny, harmonogramy i logika zależna od innych urządzeń zostają w Home
Assistancie. Nie proponuj funkcji, które to rozmywają, bez pytania.

## 2. Zasady nadrzędne (nie łam ich)

1. **Przyciski i światła mają absolutny priorytet.** Panel, API, OTA, MQTT, webhooki, logi —
   wszystko im ustępuje. Nic na ścieżce sterowania nie może czekać na sieć, flash ani na zadanie o
   niższym priorytecie.
2. **Ścieżka sterowania nigdy nie blokuje.** Zadanie `whisli-control` (co 5 ms, priorytet 10,
   rdzeń 1) trzyma `StateLock` tylko na czas `controllerIo.poll()` + `lights.tick()`. Wszystko, co
   wolne (zapis do flasha, HTTP, MQTT), idzie przez kolejkę do zadania o niższym priorytecie albo do
   `loop()`. Przykłady wzorca: `LightManager::takeStateToSave()`/`saveState()`,
   `HttpHookManager::trigger()` (kolejka), `MqttBridge` (worker + kolejki).
3. **Zła konfiguracja nie może położyć sterownika.** Nieosiągalny broker, zły URL webhooka, zły
   certyfikat, uszkodzony plik kopii — kończą się komunikatem w panelu/dzienniku, nigdy restartem,
   zawieszeniem ani opóźnieniem przycisków. Każda nowa integracja: timeout, osobne zadanie, limit
   rozmiarów, walidacja przed zapisem.
4. **Nie przełączaj prawdziwych lamp użytkownika bez potrzeby.** Sterownik pod
   `192.168.128.141` steruje realną instalacją testową. Testuj przez API odczytu, akcje na
   tymczasowych obiektach, które potem usuwasz, albo pytaj. Po testach sprzątaj (usuń testowe
   akcje, webhooki, światła).
5. **Format zapisu jest kontraktem.** Wartości enumów są zapisane we flashu (`LightKind`,
   `ActionCommand`, `HookMethod`…) — nie zmieniaj istniejących wartości, dopisuj nowe na końcu.
   Zmiana układu zapisanej struktury wymaga wersji formatu i migracji ze starej (patrz
   `static_assert(sizeof(InputAction) == 70)` i `PersistedHeaderV3` → v4).
6. **Sekrety tylko w zaszyfrowanej części kopii.** Hasła Wi-Fi/MQTT/punktu dostępowego i nagłówki
   webhooków (tokeny) trafiają do kopii konfiguracji wyłącznie w bloku AES-256-GCM. Hasło panelu
   nigdy. Nowe pole z tokenem/hasłem = osobna przestrzeń w `kSecretFileNamespaces` (lub
   odpowiednik dla NVS).

## 3. Zasady pracy z użytkownikiem

- Użytkownik pisze krótko, po polsku, potocznie. **Odpowiadaj po polsku**, zwięźle, z konkretem
  (co zrobione, co sprawdzone, czego nie sprawdzono).
- **Commit, push, publikacja wydania — tylko na prośbę.** Wgranie firmware na sterownik
  („puść na sterownik”) też tylko na prośbę.
- **Żadnych dopisków o Claude/AI w commitach** (bez `Co-Authored-By`, bez „Generated with”).
  Repozytorium ma mieć tylko użytkownika jako autora.
- Praca na gałęzi **`develop`**; `main` dostaje wydania.
- Styl commitów: po polsku, małymi literami, krótki tytuł (`webhooki, nowa akcja „Wywołaj
  webhook”`, `notatki, pomniejszenie czcionki`), przy większych zmianach lista punktów w opisie.
- Po zmianie raportuj uczciwie: czy skompilowane, czy wgrane, czy sprawdzone na sterowniku, czego
  nie widziałeś na ekranie.

## 4. Budowanie, wgrywanie, środowisko

| Co | Jak |
|---|---|
| Build firmware (z panelem) | `PATH=$HOME/.platformio/penv/bin:$PATH ./scripts/build_firmware.sh` |
| Wynik | `_build/firmware.bin` (OTA), `_build/firmware-full.bin` (USB od `0x0`) |
| Wersja | `0.002.RRRRMMDDGGMM`, nadawana przez skrypt, wspólna dla firmware i panelu |
| OTA | `curl -u whisli:<hasło> -H "Content-Type: application/octet-stream" --data-binary @_build/firmware.bin http://192.168.128.141/api/v1/firmware` |
| Po OTA | odpytuj `GET /api/v1/system` aż `firmware.version` = nowa wersja (restart ok. 10–20 s) |
| Pierwsze wgranie USB | `./scripts/build_firmware.sh -t upload` (BOOT przy podłączaniu) |
| Panel dev na żywym sterowniku | `cd ui && WHISLI_DEVICE_URL=http://192.168.128.141 WHISLI_DEVICE_AUTH=whisli:<hasło> npx vite --port 5199` |
| Typecheck panelu | `cd ui && npx tsc -b` (samo `tsc --noEmit -p .` nie sprawdza plików aplikacji) |

- Sterownik testowy: Ethernet `192.168.128.141`, hasło panelu w środowisku dev to domyślne
  `whisli_gogo` (zapytaj, jeśli przestanie działać — nie zgaduj i nie resetuj).
- Build uruchamia `scripts/embed_ui.py` (buduje React, gzipuje, generuje
  `src/app/embedded_ui.hpp`) i `scripts/generate_device_profiles.py` (z `data/device_profiles.json`
  generuje `include/whisli/app/device_profiles.generated.hpp`). Pliki generowane są w `.gitignore`
  — nie edytuj ich ręcznie.
- Nie ma osobnego obrazu systemu plików: panel jest w `firmware.bin`. Partycja „spiffs” (3,4 MB)
  służy jako LittleFS na konfigurację i notatki.
- Framework: Arduino 3.x na ESP-IDF (PlatformIO), C++17 (`-std=gnu++17`).
- Przy dużej zmianie zawsze: build → OTA → sprawdzenie `/api/v1/system` (wersja, `freeHeapKb`,
  `controlMaxLateUs`/`controlMaxBusyUs`) → test funkcji przez API → sprzątanie.

## 5. Mapa kodu

```text
src/main.cpp                 composition root: system → state lock → bootstrap → app; loop() = sprzątanie w tle
src/bootstrap/               sieć (Ethernet, Wi-Fi, AP „Whisli-Setup”), /setup, OTA, restart, trasy sieci/MQTT/kopii/notatek
src/net/http_server.cpp      esp_http_server: trasy, logowanie, StateLock na trasach, WebSocket /api/v1/ws, broadcast()
src/net/auth.cpp             jedno hasło (PBKDF2 + sól, NVS), sesje (cookie), HTTP Basic dla skryptów, reset BOOT 10 s
src/net/mqtt_settings.cpp    ustawienia MQTT w NVS
src/app/application.cpp      obiekty globalne, zadanie control, wszystkie trasy /api/v1 aplikacji, poll()
src/app/controller_io.cpp    8 DI/8 DO płytki, debounce, ButtonInput na wejście
src/app/button_input.cpp     rozpoznawanie gestów (klik, podwójny, potrójny, długi, przytrzymanie)
src/app/input_actions.cpp    zdarzenia (wejście+gest) i akcje (polecenie → cel), wykonanie
src/app/lights.cpp           logiczne światła: typy, poziomy, fade, krzywe, CCT/RGB(W), zachowanie, chaos
src/app/dmx_output.cpp       jedno uniwersum DMX512 na UART0/GPIO43, ciągłe ramki we własnym zadaniu
src/app/modbus_relays.cpp    moduły przekaźników Modbus RTU (UART1), kolejka poleceń, własne zadanie
src/app/zones.cpp            strefy (płaskie) i przypisania obiektów do stref
src/app/presets.cpp          nazwane barwy bieli (K) i kolory
src/app/http_hooks.cpp       webhooki: zapis, zmienne, wysyłka w tle (esp_http_client)
src/app/mqtt_bridge.cpp      Home Assistant: discovery, stany, polecenia, gesty jako device triggers
src/app/state_events.cpp     flagi zmian (tematy) i pierścień gestów dla panelu/MQTT
src/app/chaos_mode.cpp       test obciążeniowy całej instalacji
src/system/config_store.cpp  ConfigStore: konfiguracja w plikach LittleFS, migracja z NVS
src/system/config_backup.cpp kopia konfiguracji JSON (+ część szyfrowana), import, przeniesienie do plików
src/system/notes.cpp         notatka tekstowa instalacji (plik na LittleFS)
src/system/log.cpp           dziennik zdarzeń w RAM (bufor alokowany raz, PSRAM)
src/system/state_lock.cpp    jedna rekurencyjna blokada stanu aplikacji
include/whisli/…             interfejsy; system/task_priorities.hpp — kolejność zadań
data/device_profiles.json    katalog profili urządzeń (źródło generatora)
ui/                          panel React
doc/                         dokumentacja sprzętu, zrzuty ekranu do README
scripts/                     build, osadzanie panelu, generator profili
```

## 6. Architektura firmware

### Zadania i priorytety (`include/whisli/system/task_priorities.hpp`)

| Zadanie | Priorytet | Rola |
|---|---|---|
| `whisli-control` | 10 | wejścia → akcje → światła/wyjścia, fade; co 5 ms, rdzeń 1 |
| Modbus | 9 | polecenia do modułów przekaźników |
| DMX | 8 | ciągłe nadawanie ramek |
| HTTP | 5 | panel, API, upload firmware |
| MQTT, webhooki | 3 | Home Assistant, zapytania HTTP wychodzące |
| `loop()` | 1 | broadcast zmian, zapis stanu świateł, chaos, `mqttBridge.poll()` |

Rdzeń 0 należy do Wi-Fi/Ethernet/lwIP; zadania aplikacji na rdzeniu 1. Nowe zadanie — dopisz
priorytet do `task_priorities.hpp` z komentarzem, nigdy wyżej niż Modbus, jeśli nie jest
sterowaniem.

### Blokada stanu

- `whisli::system::StateLock` — jedna rekurencyjna blokada wokół stanu aplikacji (światła,
  urządzenia, akcje, strefy, presety, webhooki).
- Trasy HTTP domyślnie (`StateAccess::Locked`) wykonują się pod blokadą, z buforowaniem odpowiedzi
  (wysyłka do sieci po zwolnieniu). **Trzymaj je krótkie.** Trasa, która czeka (np. test webhooka,
  sieć, upload), musi mieć `StateAccess::None` i brać `StateLock` sama, na chwilę.
- Moduły z własnym zadaniem (Modbus, DMX) mają dodatkowo własne mutexy na swoje bufory.

### Powiadomienia o zmianach

- Moduł zmieniający stan woła `notifyChange(kTopicLights | …)` (tematy: devices, lights, actions,
  zones). `loop()` co ≥50 ms wysyła przez WebSocket `{"type":"changed","topics":[…]}`; heartbeat co
  20 s.
- Gesty: zadanie control zapisuje je do pierścienia (`recordInputEvent`), `loop()` wysyła
  `{"type":"input",…}` do panelu (animacja mapy) i do MQTT.
- Panel nie odpytuje w kółko — odświeża tylko dane z tematu, który przyszedł.

### Pamięć

- Wewnętrzny RAM jest rzadki (wolne ok. 85–100 KB po starcie). PSRAM 8 MB: `malloc` powyżej 4 KB
  trafia do PSRAM.
- Duże tabele **alokuj** (`new`), nie trzymaj jako statycznych tablic ani w obiektach globalnych.
  Wzorzec: `struct Tables` + referencje (`LightManager`), `*new T[1][N]()` (`ModbusRelayManager`),
  wskaźniki (`InputActionManager`).
- Nie trzymaj w RAM dużych tekstów per obiekt: MQTT przechowuje hashe FNV ostatnio wysłanych
  stanów, webhooki trzymają w pamięci tylko indeks (id, nazwa) i czytają resztę z pliku przy
  wysyłce.
- Stosy zadań: dobieraj realnie (HTTPS potrzebuje ~10 KB), sprawdzaj `freeHeapKb` po zmianie.
- Pętle po wielu obiektach w MQTT/broadcast — budżetuj (`kMessagesPerRound`), kontynuuj w
  następnym `poll()`.

### Limity (generowane z myślą o dużej instalacji)

| Obiekt | Limit | Gdzie |
|---|---|---|
| Światła | 128 | `kMaxLights` (128 RGBW = pełne uniwersum DMX) |
| Akcje / zdarzenia | 1024 / 640 | `kMaxInputActions`, `kMaxInputEvents` |
| Moduły przekaźników | 16 × do 32 kanałów | `kMaxRelayDevices`, `kMaxRelayChannels` |
| Strefy / przypisania | 64 / 1024 | `kMaxZones`, `kMaxZoneAssignments` |
| Presety | 64 | `kMaxPresets` |
| Webhooki | 128 (URL 1024, nagłówki 2048, treść 4096 B) | `http_hooks.hpp` |
| Notatka | 10 000 znaków | `notes.hpp` |
| Body formularza HTTP | 16 KB | `kMaxFormBody` w `http_server.cpp` |

Limity mają chronić przed niekontrolowanym wzrostem, nie ograniczać użytkownika. Podnosząc je,
sprawdź pamięć (PSRAM vs wewnętrzny RAM), format zapisu i MQTT.

### Przechowywanie konfiguracji

- **`ConfigStore`** (`config_store.hpp`) — API jak `Preferences`, ale pliki na LittleFS:
  `/cfg/<namespace>/<key>`, bajt typu (`nvs_type_t`) + wartość, zapis do `.new` i rename (zanik
  zasilania zostawia starą wartość), niezmienione wartości nie są przepisywane.
- Przestrzenie w plikach: `whisli-board`, `whisli-relay`, `whisli-lights`, `whisli-actions`,
  `whisli-zones`, `whisli-presets`, `whisli-hooks`, `whisli-hookauth` (sekretna).
- **W NVS zostają** tylko: `whisli-net` (sieć), `whisli-mqtt`, `whisli-auth` (hasło panelu).
- Migracja NVS → pliki: `backup::moveConfigToFiles()` przy starcie, raz na przestrzeń.
- Nowy moduł z konfiguracją: użyj `ConfigStore`, dopisz przestrzeń do `kNamespaces` w
  `config_backup.cpp` (inaczej nie trafi do kopii), sekrety do `kSecretFileNamespaces`.
- Stan świateł do przywrócenia po restarcie zapisuje `loop()` z opóźnieniem, poza blokadą.

### Kopia konfiguracji (`config_backup.cpp`)

- JSON: `{"application":"Whisli","format":1,"firmware":…,"entries":[[ns,key,type,value],…],
  "notes":{…},"secrets":{…}}`. Liczby dziesiętnie, tekst wprost, bloby base64.
- Sieć i MQTT: tylko jawne klucze, scalane z obecnymi. Sekrety w `secrets` (AES-256-GCM, klucz
  z hasła kopii przez PBKDF2).
- Import najpierw wszystko waliduje, potem zapisuje, potem restart. Musi czytać kopie ze starszych
  wersji firmware (np. akcje w formacie v3) — testuj przywracanie starej kopii po zmianie formatu.

### Akcje (`input_actions`)

- Zdarzenie = wejście (`controller.input-N`) + gest (`click`, `doubleClick`, `tripleClick`,
  `longClick`, `hold`). Akcja = zdarzenie + polecenie + cel (+ wartość).
- Polecenia (`ActionCommand`, nazwy w API): `toggle`, `on`, `off`, `stepUp`/`stepDown` (o n %),
  `setLevel` (n %), `preset` (id presetu), `run` (webhook). `Retired*` — usunięte, zostawione dla
  zgodności wartości.
- Cele: `light-N`, wyjście płytki/przekaźnik (`controller.output-N`, `relay-<id>.output-N`),
  `hook-N`. Walidacja w `fits()`: ściemnianie tylko DMX, preset tylko pasującego typu, `run` ⇔ cel
  `hook-*`.
- Nowe polecenie: dopisz na końcu enuma, nazwę w `commandName`/`parseCommand`, walidację w `fits`,
  wykonanie w `execute`, w panelu `commandLabels` + i18n + grupy w `actions-page.tsx`, ikonę na
  mapie.

### Webhooki (`http_hooks`)

- Nazwa w UI: **„Webhooki”**, polecenie **„Wywołaj webhook”**; w kodzie/API: `hook`, `run`.
- Zapis: `whisli-hooks/h<id>` (format 2: metoda, typ treści, flagi, nazwa, URL, szablon treści;
  format 1 jest nadal czytany), nagłówki osobno w `whisli-hookauth/h<id>` (sekret).
- Zmienne `{button}`, `{buttonId}`, `{gesture}`, `{zone}`, `{controller}`, `{ip}`, `{time}`
  (UTC, „Z”), `{uptime}` w URL, nagłówkach i treści; kodowane zależnie od miejsca (URL, JSON,
  formularz, tekst). Zmienna to tylko `{alfanumeryczne}` — klamry JSON zostają nietknięte.
- Akcja tylko wrzuca zadanie do kolejki (`trigger`), wysyła zadanie o priorytecie MQTT, timeout
  5 s, wynik w dzienniku (`HTTP "<nazwa>": 200`). `test` z panelu wysyła synchronicznie, poza
  blokadą.
- HTTPS: `esp_crt_bundle_attach`; opcja „Ignoruj błędy certyfikatu” wyłącza weryfikację
  (`MBEDTLS_SSL_VERIFY_NONE`).
- Do testów: https://webhook.site, https://postman-echo.com/post, https://self-signed.badssl.com/
  (certyfikat). Usługi publiczne — bez prawdziwych tokenów.

### MQTT / Home Assistant (`mqtt_bridge`)

- Tematy: `<base>/status` (LWT, retained), `<base>/light/<id>/state|set` (JSON schema),
  `<base>/input/<n>/action` (gest jako zwykły tekst). Discovery jako jedno urządzenie
  `whisli_<mac>`; jego nazwa w HA to `deviceName` z ustawień MQTT albo nazwa kontrolera.
- Przycisk w HA = encja `event` (czyta `action` przez `value_template`) + wyzwalacze urządzenia
  (`device_automation`) na każdy gest. Identyfikatory (`unique_id`) się nie zmieniają — nie ruszaj
  ich, bo HA straci przypisania encji.
- Zmiana tematu głównego albo brokera: `stop(true)` najpierw czyści stare retained (stany, status).
  Samo wyłączenie MQTT zostawia „offline” i nie usuwa encji.
- Test połączenia (`mqtt::test`, `POST /api/v1/network/mqtt/test`): osobny, jednorazowy klient z
  własnym id, bez auto-reconnect, limit 6 s — działające połączenie z HA nie jest przerywane.
- `loop()` tylko porównuje stany (hashe) i stosuje polecenia z kolejki; łączenie, restart klienta i
  wysyłka w workerze. Zmiana ustawień nigdy nie blokuje `loop()`.
- Zły broker = błąd na stronie MQTT, nic więcej. Sprawdzaj to przy każdej zmianie mostu.

### Sieć i dostęp

- Ethernet (DHCP) + opcjonalnie Wi-Fi; punkt dostępowy `Whisli-Setup` (hasło domyślne
  `whisli_gogo`, `192.168.4.1`), wraca sam po 60 s bez sieci.
- Jedno hasło panelu (domyślne `whisli_gogo`), sesja w cookie, HTTP Basic dla skryptów. BOOT 10 s =
  reset hasła.
- Panel jest po zwykłym HTTP — dostęp z zewnątrz tylko przez VPN (tak też piszemy w dokumentacji).
- Komunikaty API dla użytkownika są po polsku (`{"message":"…"}`); panel pokazuje je wprost.

## 7. Panel WWW (`ui/`)

- React 19 + TypeScript + Vite 6 + Tailwind 4, komponenty w stylu shadcn/ui (Base UI, Radix),
  ikony Lucide, mapa na `@xyflow/react` (ładowana leniwie).
- Routing przez hash (`lib/route.ts`, np. `#/webhooks`); nawigacja i podstrony w `App.tsx`
  (`navigation`).
- API: `lib/api.ts` — `getJson`, `post` (form-encoded, nigdy nie rzuca). Wszystkie trasy pod
  `/api/v1`.
- Odświeżanie na żywo: `hooks/use-live-refresh.ts` (jeden WebSocket, liczniki per temat).
- **i18n:** `src/i18n/pl.json` i `en.json` — każdy nowy tekst w obu plikach, ten sam klucz
  (`TranslationKey` bierze klucze z `pl.json`). Żadnych tekstów na sztywno w JSX.
- Wspólne elementy: `components/page.tsx` (`PageHeader`, `Section`, `Field`, `AutoSaveInput`),
  `components/ui/*` (button, combobox, dialog, message-box, checkbox, slider).

### Styl wizualny

- **„Pro”, monochromatycznie, jedynym akcentem jest niebieski** (`--status-ok`). Czerwony tylko dla
  błędów/usuwania. Bez kolorowych kafelków, gradientów, mocnych ramek.
- Kolory wyłącznie z tokenów w `src/index.css` (`--page`, `--card`, `--raised`, `--ink`,
  `--muted`, `--line`, `--status-*`) i klas `whisli-*`; motyw jasny i ciemny (`data-theme`).
  Każdą zmianę wyglądu sprawdź w obu motywach.
- Delikatne obramowania (`whisli-line`), dużo miejsca na treść, małe opisy `whisli-muted text-xs`.
- **Bez przycisków „Zapisz”**: pola zapisują się przy opuszczeniu (`AutoSaveInput`) albo od razu
  przy wyborze. Usuwanie zawsze z potwierdzeniem (`MessageBox`).
- Zrzuty ekranu do README: wersja polska, ciemny motyw, `doc/screenshots/`.

## 8. Czego nie robić

- Nie wywołuj sieci, flasha ani długich operacji pod `StateLock` lub w zadaniu control.
- Nie dodawaj dużych statycznych tablic ani `String` per obiekt w obiektach globalnych.
- Nie zmieniaj wartości zapisanych enumów ani układu zapisanych struktur bez migracji.
- Nie dodawaj sekretów do jawnej części kopii ani do dziennika zdarzeń. (Zalogowany panel dostaje
  nagłówki webhooków w `GET /api/v1/hooks`, bo je edytuje — to jedyny wyjątek.)
- Nie edytuj plików generowanych (`embedded_ui.hpp`, `*.generated.hpp`, `data/www/`).
- Nie wspominaj w README o DALI (nie jest obsługiwane).
- Nie commituj, nie pushuj, nie wgrywaj firmware i nie przełączaj lamp bez prośby.

## 9. Dokumentacja

- `README.md`: na górze wstęp dla ludzi (po polsku, bez opisywania ekranów), „Szybki start”, potem
  „Dokumentacja techniczna”. Zrzuty w `doc/screenshots/`.
- Gotowy firmware jest publikowany w GitHub Releases (`firmware.bin`, `firmware-full.bin`).
- Komentarze w kodzie po angielsku (część starszych po polsku — nie przepisuj ich bez potrzeby);
  komentarz mówi *dlaczego*, nie *co*. Gęstość i styl jak w okolicznym kodzie.
