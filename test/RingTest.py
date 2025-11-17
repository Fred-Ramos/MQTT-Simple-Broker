import paho.mqtt.client as mqtt
import time
import threading
import argparse


# Configure command line arguments
parser = argparse.ArgumentParser(description='Round trip test with MQTT.')
parser.add_argument('ip', type=str, help='MQTT broker IP address')
parser.add_argument('port', type=int, help='MQTT broker port')
parser.add_argument('QoS', type=int, choices=[0, 1, 2], help='MQTT Quality of Service level: 0, 1, or 2')
parser.add_argument('N', type=int, help='Number of clients')
parser.add_argument('num_tests', type=int, help='Number of tests to perform')
args = parser.parse_args()

broker = args.ip
port = args.port
N = args.N
num_tests = args.num_tests
qos = args.QoS

# Function to create a client
def create_client(client_id, subscribe_topic, publish_topic):
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,client_id)

    def on_connect(client, userdata, flags, rc, properties=None):
        print(f"{client_id} connected with result code {rc}")
        client.subscribe(subscribe_topic, qos)
    
    def on_message(client, userdata, msg):
        if client_id == "client1" and msg.topic == "end":
            end_time = time.time()
            elapsed_time = end_time - start_time
            print(f"Round trip time {elapsed_time} seconds")
            perform_test()
        else:
            client.publish(publish_topic, msg.payload.decode(),qos)
    
    client.on_connect = on_connect
    client.on_message = on_message
    
    client.connect(broker, port, keepalive = 60) 
    return client

# Create clients and assign topics
clients = []
for i in range(1, N+1):
    if i == 1:
        subscribe_topic = "end"
        publish_topic = "topic1"
    elif i == N:
        subscribe_topic = f"topic{i-1}"
        publish_topic = "end"
    else:
        subscribe_topic = f"topic{i-1}"
        publish_topic = f"topic{i}"
    
    client = create_client(f"client{i}", subscribe_topic, publish_topic)
    clients.append(client)

# Function to start the loop for each client
def start_client(client):
    client.loop_start()

# Start all clients
for client in clients:
    threading.Thread(target=start_client, args=(client,)).start()

# Function to perform a test
def perform_test():
    global start_time, test_count
    if test_count < num_tests:
        start_time = time.time()
        clients[0].publish("topic1", "1",qos)
        test_count += 1
    else:
        print(f"All {test_count} tests completed")
        for client in clients:
            client.loop_stop()
            client.disconnect()

# Initialize test count
test_count = 0

# Delay the initial message by 2 seconds
threading.Timer(2, perform_test).start()

# Keep the script running
try:
    while True:
        time.sleep(1)
except KeyboardInterrupt:
    print("Exiting...")
    for client in clients:
        client.loop_stop()
        client.disconnect()