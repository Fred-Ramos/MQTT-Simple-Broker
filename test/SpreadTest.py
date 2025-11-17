import paho.mqtt.client as mqtt
import time
import threading
import argparse

# Configure command line arguments
parser = argparse.ArgumentParser(description='Round trip test with MQTT.')
parser.add_argument('ip', type=str, help='MQTT broker IP address')
parser.add_argument('port', type=int, help='MQTT broker port')
parser.add_argument('QoS', type=int, choices=[0, 1, 2], help='MQTT Quality of Service level: 0, 1, or 2')
parser.add_argument('N', type=int, help='Number of clients -> max 250')
parser.add_argument('num_tests', type=int, help='Number of tests to perform')
args = parser.parse_args()

broker = args.ip
port = args.port
N = args.N
num_tests = args.num_tests
qos = args.QoS

# Global variable to count received messages
received_count = 0
received_lock = threading.Lock()

# Function to create a client
def create_client(client_id, is_publisher=False):
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)

    def on_connect(client, userdata, flags, rc, properties=None):
        print(f"{client_id} connected with result code {rc}")
        if not is_publisher:
            client.subscribe("fire", qos)

    def on_message(client, userdata, msg):
        global received_count
        if not is_publisher and msg.topic == "fire":
            with received_lock:
                if received_count < N:
                    received_count += 1
                    #print(f"{client_id} received fire message. Total received: {received_count}")

    client.on_connect = on_connect
    client.on_message = on_message

    client.connect(broker, port, keepalive=60)
    return client

# Create clients
clients = []
for i in range(N):
    client_id = f"client{i+1}"
    client = create_client(client_id)
    clients.append(client)

# Create publisher client
publisher_client = create_client("client0", is_publisher=True)
clients.append(publisher_client)

# Function to start the loop for each client
def start_client(client):
    client.loop_start()

# Start all clients
for client in clients:
    threading.Thread(target=start_client, args=(client,)).start()

# Function to perform a test
def perform_test():
    global start_time, test_count, received_count
    if test_count < num_tests:
        start_time = time.time()
        with received_lock:
            received_count = 0
        publisher_client.publish("fire", "1", qos)
        #print(f"Test {test_count + 1} started")
        test_count += 1
        #print(f"perform_test: test_count={test_count}, received_count={received_count}")
    

# Thread to monitor the received count and print the elapsed time
def monitor_received_count():
    global received_count, test_count
    while test_count <= num_tests:
        with received_lock:
            if received_count == N:
                end_time = time.time()
                elapsed_time = end_time - start_time
                print(f"Total round trip time for test {test_count}: {elapsed_time} seconds")
                #print(f"monitor_received_count (end of test): test_count={test_count}, received_count={received_count}")
                if test_count < num_tests:
                    # Release the lock before calling perform_test
                    received_lock.release()
                    perform_test()
                    # Reacquire the lock after calling perform_test
                    received_lock.acquire()
                else:
                    print(f"All {test_count} tests completed")
                    break  # Exit the loop after the last test
# Initialize test count
test_count = 0

# Delay the initial message by 2 seconds and start the test
threading.Timer(2, perform_test).start()

# Start the monitor thread
monitor_thread = threading.Thread(target=monitor_received_count)
monitor_thread.start()

# Keep the script running
try:
    while True:
        time.sleep(1)
except KeyboardInterrupt:
    print("Exiting...")
    for client in clients:
        client.loop_stop()
        client.disconnect()