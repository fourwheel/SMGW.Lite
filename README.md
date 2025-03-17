# 📡 SMGW.Lite - Why?
Until 2030, all meters in German households must be replaced. They receive digital meters (*moderner Messeinrichtung*) and the meter value is taken once a year. Only house holds with an EV charger, heatpumpt, PV-powerplant or annual consumption > 6.000 kWh receive a Smart Meter Gateway: Together with the digital meter it forms a Smart Meter (according to the German definition). It comes not only with 15 minute meter readings, but also brings a secured communication channel - allowing grid operators to talk to your heatpump, EV charger, ... .
In my humble opinion, every electricity consumer should be able to have meter values every 15 minutes, even when they don't have the above mentioned devices. This is key to a participation in the energy transition.  
Luckily, the digital meter transmits valuable information including the meter values periodically on an optical interface. In my freetime, I developed a project to read out such meters high-frequently.  
I call it "smart meter gateway light".

## 🚀 Project Descripiton
This project contains of a firmware for ESP32. Together with a dongle, it receives and decrypts the telegrams coming from the meter. It then transmits it to a backend, where the data can be processed and dispay. The bear minimum code for the backend to receive the data and store it in a database is also included in this repository.
Some mimic of German metering have found there way into my implementation (though I'm not supporting all of them!). For example te TAF (*Tarifanwendungsfall*) and Wirk-PKI (for TLS secured backend communication) are imitated.

## 🛠️ Prerequisits
- digital meter / "moderner Messeinrichtung" with optical interface
- Pin code to unlock data flow on your meter - can be obtained by your metering point operators
- ESP32-Board (for example ESP32 DevKit v1)
- optical dongle ([example](https://www.ebay.de/itm/313460034498))
- DS18B20 temperature sensor (optional)
- VSCode with Platformio

## 🏗️ Core Code Blocks
- 🔗 **Log_** offers a detailed logging of what has happened
- 📝 **Telegram_** brings in function to handle incoming telegrams
- 🔗 **MeterValue_** stores the meter values
- ⚙️ **Param_** allows you to store your parameters
- 🕰️  **Time_** adds timing information
- 📡 **webclient_** contains function to talk to your backend
- 🌍 **webserver_** offers features for handling the SMGW.Lite
---



## 🌐 Getting Started
1. Clone repo, flash device
2. Connect sensors, boot device
3. Connect to WiFi (SSID: `SMGW.Lite`)
4. Browse to `http://192.168.4.1`. You might be asked for the password: `password`
2. Configure parameter according to your needs.
3. Reboot the device, disconnect from WiFi.


---

## 📜 `Licence`
This project is licensed under **MIT-Lizenz**. See `LICENSE` for more details.

---

## 🤝 Contribution
Want to contribute? Awesome! 🎉 Follow these steps:
- Fork the repository 🍴
- Create a new branch (feature/your-feature-name) 🌿
- Commit your changes and push to your fork 📤
- Open a Pull Request for review 🔍
