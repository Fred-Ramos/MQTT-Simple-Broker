Required to run Test Bench:

```
pip install paho-mqtt --break-system-packages
```

Run code with necessary arguments:


```
python3 RingTest.py <ip> <port> <QoS> <N> <num_tests>
```
```
python3 SpreadTest.py <ip> <port> <QoS> <N> <num_tests>
```


Use command line below to have access to all parameters and test info:

```
python3 RingTest.py -h

```
```
python3 SpreadTest.py -h
```