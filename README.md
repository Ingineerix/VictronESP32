# VictronESP32

This is an Ardino ESP32 sketch that uses BLE to connect to Victron SmartSolar MPPTs and the Victron BMV (Battery Monitor) to provide a nice dynamically updating web page using webSockets.

It includes spiffy analog gauges for inportant metrics and a daily solar production graph.

![Screenshot](https://github.com/ingineerix/VictronESP32/blob/main/screenshot3.png?raw=true)

You will need to grab your Victron Bluetooth Encryption keys from the Victron phone app (iOS/Android) under "Product Info" and click the "Show" button under "Encryption Data", then add that hex data to the array starting at line 96.

My system has 3 SmartSolar MPPTs and one BMV, so if yours is different, you'll need to customize the code.

Also, you will need to set certain variables and defines at line 15 for your system.
