CC = gcc
CFLAGS = -Wall

SRC_DIR = src
OBJ = main.o broker.o

# Targets
all: mqtt_broker

# Clean up build artifacts
clean:
	rm -f *.o mqtt_broker

# Pattern rule for compiling .c files into .o files
%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# Final executable target
mqtt_broker: $(OBJ)
	$(CC) -o mqtt_broker $(OBJ)

