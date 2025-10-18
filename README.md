# Real-Time Weather Monitoring System

## Overview
This project implements a real-time weather monitoring system using ESP32 and DHT11 sensor, with cloud storage and visualization on Firebase. The system is designed to be power-efficient, reliable, and capable of handling intermittent network connectivity.

## Features
-- Real-time temperature and humidity monitoring  
-- Firebase integration for cloud storage  
-- FreeRTOS-based task scheduling for sensor reads, Wi-Fi management, and data uploads  
-- Deep sleep scheduling to reduce power consumption  
-- Queue-based data buffering to handle network outages  
-- NTP-based time synchronization with validation to ensure accurate timestamps  

## Hardware Required
-- ESP32 Development Board  
-- DHT11 Temperature & Humidity Sensor  
-- USB cable for programming  
-- Wi-Fi connectivity  

## Software Requirements
-- Arduino IDE or PlatformIO  
-- ESP32 Board support for Arduino  
-- ArduinoJson library  
-- WiFi.h and HTTPClient.h libraries  
-- FreeRTOS (built-in with ESP32 Arduino core)  

## Project Structure
-- `main.ino` : Core firmware implementing sensor reads, FreeRTOS tasks, Wi-Fi connection, time sync, and Firebase uploads  
-- `freertos/queue.h` : Used for inter-task communication (data buffering)  
-- `ESP32Time.h` : RTC management and NTP synchronization  
-- Libraries required are included via Arduino IDE Library Manager  

## Usage
1. Connect the ESP32 to the computer via USB.  
2. Open `temperatureMonitor.ino` in Arduino IDE.  
3. Set Wi-Fi credentials and Firebase project details in the configuration section.  
4. Compile and upload the code to ESP32.  
5. Open Serial Monitor to view real-time logs and status.  
6. Access Firebase to monitor uploaded sensor readings.  

## Workflow
-- Upon boot, the ESP32 connects to Wi-Fi.  
-- NTP time synchronization is performed and verified for accuracy (year >= 2024).  
-- Sensor readings are captured hourly (or at shorter intervals in case of errors).  
-- Data is stored in a **queue** if network is unavailable.  
-- Network monitor task ensures queued data is sent when connection is restored.  
-- Deep sleep mechanism is used between readings to save power.  

## Future Improvements
-- Add web dashboard for real-time visualization of temperature and humidity trends  
-- Integrate additional sensors (air quality, pressure, light intensity)  
-- Implement adaptive upload intervals based on network stability and time of day  

## Author
Preetham  


## License
This project is licensed under MIT License.
