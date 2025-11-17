# SDIS_PROJECT
Implementation of MQTT

Authors: Cesar Junior and Frederico Ramos

The objective of this work is to develop a simMQTT Broker, applying the concepts studied in the Distributed Systems course. Following the theoretical approach, a practical implementation will be carried out, allowing for the exploration of the broker's operation in the pub/sub model and message management. 



The intended specifications for this work are:

•	TCP/IP communication between the Broker and MQTT Clients.

•	Quality of Service (QoS) 1 as a minimum.

•	Dynamic management of Clients and their respective topics (Pub/Sub).

•	Code written in C and C++.



The planned final implementation will be:
Broker → PC (Linux)
MQTT Clients → 2 PCs + 2 microcontrollers (Esp32 and Esp8266)

![image](https://github.com/user-attachments/assets/f0c42e92-a630-4fd0-a1b5-60be23d0af91)


Figure 1- Diagram of the desired MQTT broker at the end of the project with example topics



################################################################

QoS1 Working

# MQTT Broker

This project implements s MQTT Broker from scratch, with Quality of Service (QoS) 1. MQTT's oficial documentation was used https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718037.

most of the limitations to our simplified MQTT broker are from the limits imposed on the header file broker.h
