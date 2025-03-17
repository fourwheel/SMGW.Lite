# 📡 SMGW.Lite - Why?

By **2030**, all electricity meters in German households must be replaced with **digital meters** (*moderne Messeinrichtung*). These meters typically store consumption data and are only read **once per year**.  

However, only households with an **EV charger**, **heat pump**, **PV power plant**, or an annual consumption of **more than 6,000 kWh** receive a **Smart Meter Gateway (SMGW)**. Together with the digital meter, this forms a **Smart Meter** (according to the German definition).  

A Smart Meter does more than just provide **15-minute interval readings** which are transmitted day after — it also establishes a **secure communication channel**, allowing grid operators to **remotely manage** connected devices like heat pumps and EV chargers.  

In my opinion, **every electricity consumer** should have access to **15-minute interval meter readings**, even if they don’t own the devices mentioned above. This is **key to actively participating in the energy transition** by shifting the consumption.
Luckily, digital meters periodically transmit **valuable data**, including meter readings, via an **optical interface**. In my free time, I developed a project to **extract** and **process** this data at **high frequency**.  
🔥 **I call it *Smart Meter Gateway Lite*.**

---

## 🚀 Project Description

This project consists of **firmware for an ESP32 microcontroller**.  

Together with an **optical dongle**, it:
- **Receives and decrypts** telegrams from the meter 🔄  
- **Transmits** the data to a backend for further **processing and visualization** 📡  
- **Includes basic backend code** to store data in a database 🗄️  

Although the implementation is **not** a complete German Smart Meter Gateway, it does imitate certain aspects such as:  
- **TAF** (*Tarifanwendungsfall*)  
- **Wirk-PKI** (TLS-secured backend communication)  

---

## 🛠️ Prerequisites  

To use **SMGW.Lite**, you will need:  

✔️ **A digital meter** (*moderner Messeinrichtung*) with an **optical interface**  
✔️ **A PIN code** to unlock data flow on your meter (request from your metering point operator)  
✔️ **ESP32 board** (e.g., *ESP32 DevKit v1*)  
✔️ **Optical dongle** ([example](https://www.ebay.de/itm/313460034498))  
✔️ **DS18B20 temperature sensor** (optional) 🌡️  
✔️ **VSCode with PlatformIO** 🖥️  

---

## 🏗️ Core Code Blocks

- 🔗 **`Log_`** → Provides detailed logging of system events  
- 📝 **`Telegram_`** → Handles incoming telegrams from the meter  
- 🔢 **`MeterValue_`** → Stores and processes meter readings  
- ⚙️ **`Param_`** → Manages system parameters  
- ⏳ **`Time_`** → Adds time-related functionalities  
- 📡 **`Webclient_`** → Sends collected data to the backend  
- 🌍 **`Webserver_`** → Provides a web interface for device management  

---


## 🌐 Getting Started
1. Clone repo, flash device
2. Connect dongle, boot device
3. Connect to WiFi named `SMGW.Lite`
4. Browse to `http://192.168.4.1`. You might be asked for the password: `password`
5. Configure parameter to match your needs.
6. Reboot the device, disconnect from WiFi.
7. Find the new IP address via your router's admin panel or serial monitor


---

## 📜 `Licence`
This project is licensed under **MIT-License**. See `LICENSE` for more details.

---

## 🤝 Contribution
Want to contribute? Awesome! 🎉 Follow these steps:
- Fork the repository 🍴
- Create a new branch (feature/your-feature-name) 🌿
- Commit your changes and push to your fork 📤
- Open a Pull Request for review 🔍
