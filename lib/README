# 📡 ESP32 Smart Meter Gateway (SMGW.Lite)

## 🚀 Projektbeschreibung
Dieses Projekt ist eine Firmware für ein ESP32-basiertes Smart Meter Gateway. Es liest Verbrauchswerte aus, verarbeitet sie und sendet sie an ein Backend. Die Konfiguration erfolgt über eine Weboberfläche.

## 📦 Features
- 📡 **WLAN-Anbindung** mit Webinterface zur Konfiguration
- 🔄 **Datenverarbeitung** von Smart Meter Telegrammen
- 🔗 **Backend-Anbindung** zur Übermittlung der Verbrauchsdaten
- 🌡️ **Temperaturmessung** über Dallas DS18B20 Sensor
- 🔄 **Over-the-Air (OTA) Updates** möglich

---

## 🛠️ Installation & Einrichtung
### 1️⃣ Benötigte Hardware
- ESP32-Board (z. B. ESP32 DevKit v1)
- Optischer Lesekopf für moderne Messeinrichtung
- DS18B20 Temperatursensor (optional)


### 2️⃣ Abhängigkeiten installieren
Benötigte Bibliotheken in der **Arduino IDE** oder **PlatformIO**:
```cpp
#include <WiFi.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <IotWebConf.h>
```

### 3️⃣ Code hochladen
1. Projekt mit der Arduino IDE oder PlatformIO öffnen
2. **`main.cpp` anpassen**:
   - WLAN-SSID & Passwort setzen (oder aus SPIFFS laden)
   - Backend-URL konfigurieren
3. Auf den ESP32 hochladen und seriellen Monitor beobachten.

---

## 🌐 Webinterface
Nach dem Start öffnet der ESP32 ein eigenes WLAN (SSID: `SMGW.Lite`).
1. Mit diesem WLAN verbinden und `http://192.168.4.1` aufrufen.
2. WLAN-Daten & Backend eintragen.
3. Nach Neustart ist das Webinterface über die vergebene IP erreichbar.

---

## 🔌 API Endpunkte
| Route | Beschreibung |
|--------|--------------|
| `/` | Startseite mit Systemstatus |
| `/config` | Konfiguration aufrufen |
| `/showMeterValue` | Aktuellen Zählerstand anzeigen |
| `/showCert` | SSL-Zertifikat anzeigen |
| `/sendMeterValues_Task` | Messwerte an Backend senden |
| `/restart` | ESP32 neustarten |

---

## 📜 Lizenz
Dieses Projekt verwendet die **MIT-Lizenz**. Siehe `LICENSE` für weitere Details.

---

## 👨‍💻 Beitrag leisten
- Fehler melden & Vorschläge machen: [GitHub Issues]
- Code verbessern: Fork & Pull Request erstellen.
- Fragen? Schreibe eine Nachricht! 🚀

