# MQTT Broker (QoS 1) — Simple C Implementation

This project implements a **custom MQTT Broker** supporting **QoS 1 (at least once delivery)**, following the MQTT v3.1.1 specification.  
The design is intentionally lightweight and focused on connection management, subscriptions, message forwarding, and retransmission.

Most of the limitations come from the configuration constants in **broker.h**.

## Features

- Full handling of core MQTT Control Packets:
  - **CONNECT / CONNACK**
  - **PUBLISH / PUBACK**
  - **SUBSCRIBE / SUBACK**
  - **PINGREQ / PINGRESP**
  - **DISCONNECT**
- **QoS 1 reliability**
  - Message stored in per-client queues  
  - Retransmitted until PUBACK is received
- **Session persistence**
  - Reconnecting with the same Client ID restores the previous session state
- **Multi-threaded server**
  - One thread per client  
  - One global queue-handling thread  
- TCP server running on port **1883**

## Configuration (Static)

At the moment, configuration is done through constants in `broker.h`:

```
#define BROKER_PORT 1883
#define MAX_CLIENTS 10
#define MAX_TOPICS 5
#define MAX_PUB_QUEUE_SIZE 10
#define TIME_TO_RETRANSMIT 5.0
#define BUFFER_SIZE 1024
```

## Build Instructions

Only **gcc** and **make** are required.

Build using:

```
make
```

Then run the generated executable:

```
./mqtt_broker
```

The broker immediately opens a TCP server on port **1883** and waits for client connections.

## Test Benches

Python tests included (`/test`) evaluate:

- **Ring Test** — end-to-end propagation delay  
- **Spread Test** — fan-out to N subscribers and queue performance  

## Limitations

- No authentication  
- No retained messages  
- No wildcard topic support  
- Single-thread-per-client model  
- Only supports QoS 1  

## Reference

MQTT v3.1.1 specification: https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html
