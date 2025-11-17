#include "broker.h"

//function creates server at local ip and given port
int create_tcpserver(int *server_fd, struct sockaddr_in *address, int *addrlen) {
    //create socket
    if ((*server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) { //IPv4, stream-oriented (TCP)
        perror("Socket creation failed");
        return -1;
    }

    int opt = 1;
    //set SO_REUSEADDR to allow reusing the address to avoid init errors
    if (setsockopt(*server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt failed");
        close(*server_fd);
        return -1;
    }

    //setup address
    address->sin_family = AF_INET;            //address family to IPv4
    address->sin_addr.s_addr = INADDR_ANY;    //accepting connectons on any available interface
    address->sin_port = htons(BROKER_PORT);   //set port in network byte order

    //bind the socket to the specified address and port
    if (bind(*server_fd, (struct sockaddr *)address, *addrlen) < 0) { //cast to simple struct sockaddr
        perror("Socket binding failed");
        close(*server_fd); //clean up the socket before exiting
        return -1;
    }

    //listen for incoming connections
    if (listen(*server_fd, MAX_CLIENTS) < 0){ 
        perror("Listening failed\n");
        close(*server_fd); //clean up the socket before exiting
        return -1;
    }
    printf("Server created sucessfully and listening\n");
    return 0;
}

//main loop function, for each thread
void *client_handler(void *arg) {
    printf("Thread created\n");
    thread_data *t_data = (thread_data *)arg; //cast to thread data type again
    int conn_fd = t_data->conn_fd;
    session *running_sessions = t_data->running_sessions;
    free(t_data); //no longer needed, free

    uint8_t buffer[BUFFER_SIZE] = {0};

    // Handle client connection
    while (1) {
        ssize_t valread = read(conn_fd, buffer, BUFFER_SIZE);
        if (valread <= 0) {
            printf("Client disconnected: conn_fd: %d | forcing connection close\n", conn_fd);
            //find the running session with matching conn_fd
            session *current_session = NULL;
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (running_sessions[i].conn_fd == conn_fd) {
                    current_session = &running_sessions[i];
                    break;
                }
            }
            if (current_session == NULL) {
                printf("Session not found || couldn't reset conn_fd\n");
            }
            close(conn_fd);
            current_session->conn_fd = 0;
            
            pthread_exit(NULL);
        }

        mqtt_pck received_pck = {0};
        received_pck.conn_fd = conn_fd;

        //Process MQTT packet
        if (mqtt_process_pck(buffer, received_pck, running_sessions) < 0) {
            printf("MQTT Process Error\n");
        }
        printf("|||||||||||||||||||||||\n");
        usleep(10); //to not overload CPU
    }
    return NULL;
}

//function to decode the remaining length
int decode_remaining_length(uint8_t *buffer, uint8_t *remaining_length, int *offset) {
    int multiplier = 1;
    uint8_t encoded_byte;
    *remaining_length = 0;
    *offset = 1;
    for (int i = 0; i < 4; i++) { //remaining Length can take up to 4 bytes
        encoded_byte = buffer[*offset];
        *remaining_length += (encoded_byte & 127) * multiplier;
        multiplier *= 128;

        if (multiplier > 128 * 128 * 128) { //malformed packet check
            printf("Malformed Remaining Length\n");
            return -1;
        }

        (*offset)++;

        if ((encoded_byte & 128) == 0) { //MSB = 0 indicates end of length encoding
            break;
        }
    }
    return 0; 
}

//function to encode the remaining length
int encode_remaining_length(uint8_t *buffer, size_t remaining_len) {
    int bytes_written = 0;

    do {
        uint8_t encoded_byte = remaining_len % 128;
        remaining_len /= 128;
        //if there is more data to encode, set the continuation bit (MSB = 1)
        if (remaining_len > 0) {
            encoded_byte |= 128;
        }
        buffer[bytes_written++] = encoded_byte;
    } while (remaining_len > 0 && bytes_written < 4);

    //return number of bytes used for the Remaining Length
    return bytes_written;
}

//function to easily made packet(only from a filled variable of type structure mqtt_pck)
int send_pck(mqtt_pck *packet) {
    //buffer to encode the Remaining Length (max 4 bytes)
    uint8_t remaining_length_encoded[4];
    int remaining_length_size = encode_remaining_length(remaining_length_encoded, packet->remaining_len);
    if (remaining_length_size < 1 || remaining_length_size > 4) {
        printf("Failed to encode Remaining Length\n");
        return -1;
    }

    //calculate total size of the packet
    size_t total_size = 1 + remaining_length_size; //fixed Header (1 byte for pck_type + flags) + Remaining Length
    if (packet->variable_header) {
        total_size += packet->variable_len; //variable Header size
    }
    if (packet->payload) {
        total_size += packet->payload_len; //payload size
    }

    // Allocate a buffer for the serialized packet
    uint8_t *buffer = malloc(total_size);
    if (!buffer) {
        perror("Failed to allocate buffer for packet serialization");
        return -1;
    }

    //get offset
    size_t offset = 0;
    buffer[offset++] = (packet->pck_type << 4) | (packet->flag & 0x0F); //packet Type and flags

    memcpy(&buffer[offset], remaining_length_encoded, remaining_length_size);
    offset += remaining_length_size;

    //serialize the Variable Header
    if (packet->variable_header) {
        memcpy(&buffer[offset], packet->variable_header, packet->variable_len);
        offset += packet->variable_len;
    }

    //serialize the Payload
    if (packet->payload) {
        memcpy(&buffer[offset], packet->payload, packet->payload_len);
        offset += packet->payload_len;
    }

    //send the serialized packet
    ssize_t bytes_sent = send(packet->conn_fd, buffer, offset, 0);
    if (bytes_sent < 0) {
        perror("Failed to send packet");
        free(buffer);
        return -1;
    }
    packet->time_sent = clock(); //only truly used in the case of PUBLISH packet, where it might need to retransmit
    //clean up
    free(buffer);
    printf("Packet sent to conn_fd %d\n", packet->conn_fd);
    return 0;
}

//============================================================================================================================//
//============================================================================================================================//
//============================================================================================================================//
//============================================================================================================================//
//determine type of packet and process
int mqtt_process_pck(uint8_t *buffer, mqtt_pck received_pck, session* running_sessions){
    //======================Analise Fixed Header 1st byte=======================================//
    received_pck.flag = buffer[0] & 0x0F;             //0->4 flag
    received_pck.pck_type = (buffer[0] >> 4) & 0x0F;  //4->7 control packet type
    
    //==============================Decode remaining packet length=============================//
    int offset = 1;
    uint8_t remaining_length;
    if (decode_remaining_length(buffer, &remaining_length, &offset) < 0) {
        return -1; //error decoding Remaining Length
    }
    received_pck.remaining_len = remaining_length;
    printf("Packet Received || conn_fd: %d || ", received_pck.conn_fd);
    // printf("Flag: %d || packet Type: %d || Remaining Length: %ld || ", received_pck.flag, received_pck.pck_type, received_pck.remaining_len);

    //=============Determine packet type received from a Client=================//
    printf("Packet Type: ");
    switch (received_pck.pck_type)
    {
    case 1: //CONNECT
        printf("CONNECT\n");
        if (received_pck.flag != 0){ //flag must be 0 for CONNECT
            printf("Invalid flag for CONNECT\n");
            return -1;
        }
        //fill variable header
        received_pck.variable_len = 10;
        received_pck.variable_header = malloc(received_pck.variable_len); //allocate 10 bytes (CONNECT variable header)
        if (received_pck.variable_header == NULL) {
            perror("Failed to allocate memory for variable header");
            exit(EXIT_FAILURE);
        }
        memcpy(received_pck.variable_header, buffer + offset, received_pck.variable_len); //copy 10 bytes from buffer starting at offset
        
        //compute payload len
        received_pck.payload_len = received_pck.remaining_len - received_pck.variable_len;

        //fill payload
        received_pck.payload = malloc(received_pck.payload_len); //allocate 10 bytes (CONNECT variable header)
        if (received_pck.payload == NULL) {
            perror("Failed to allocate memory for payload");
            exit(EXIT_FAILURE);
        }
        memcpy(received_pck.payload, buffer + offset + received_pck.variable_len, received_pck.payload_len); //copy X bytes from buffer starting after variable header

        return connect_handler(&received_pck, running_sessions); //interpret connect command
    
    case 3: //PUBLISH
        printf("PUBLISH\n");
        //fill variable header
        received_pck.topic_len = (buffer[offset] << 8) | buffer[offset + 1];
        received_pck.variable_len = received_pck.topic_len + 4; //+2 for length MSB and LSB and +2 for Packet ID MSB and LSB

        received_pck.variable_header = malloc(received_pck.variable_len); //allocate bytes (PUBLISH variable header)
        if (received_pck.variable_header == NULL) {
            perror("Failed to allocate memory for variable header");
            exit(EXIT_FAILURE);
        }
        memcpy(received_pck.variable_header, buffer + offset, received_pck.variable_len); //copy bytes from buffer starting at offset
        
        //compute payload len
        received_pck.payload_len = received_pck.remaining_len - received_pck.variable_len;

        //fill payload
        received_pck.payload = malloc(received_pck.payload_len); //allocate bytes (PUBLISH variable header)
        if (received_pck.payload == NULL) {
            perror("Failed to allocate memory for payload");
            exit(EXIT_FAILURE);
        }
        memcpy(received_pck.payload, buffer + offset + received_pck.variable_len, received_pck.payload_len); //copy X bytes from buffer starting after variable header

        return publish_handler(&received_pck, running_sessions); //interpret publish command
    
    case 4: //PUBLISH ACKNOWLEDGE
        printf("PUBACK\n");
        //fill variable header
        received_pck.variable_len = 2; //variable header only has packet ID MSB and LSB

        received_pck.variable_header = malloc(received_pck.variable_len); //allocate bytes (PUBLISH variable header)
        if (received_pck.variable_header == NULL) {
            perror("Failed to allocate memory for variable header");
            exit(EXIT_FAILURE);
        }
        memcpy(received_pck.variable_header, buffer + offset, received_pck.variable_len); //copy bytes from buffer starting at offset
        
        //compute payload len
        received_pck.payload_len = received_pck.remaining_len - received_pck.variable_len;

        //fill payload
        received_pck.payload_len = 0;
        received_pck.payload = NULL; //no payload on PUBACL

        return puback_handler(&received_pck, running_sessions);

    case 8: //SUBSCRIBE
        printf("SUBSCRIBE\n");
        if (received_pck.flag != 2){ //flag must be 0b1000 for SUBSCRIBE
            printf("Invalid flag for SUBSCRIBE\n");
            return -1;
        }
        //size of variable header for this packet
        received_pck.variable_len = 2;
        //fill variable header
        received_pck.variable_header = malloc(received_pck.variable_len); // Allocate var_head bytes (SUBSCRIBE variable header, CONTAINS PACKET ID!!!)
        if (received_pck.variable_header == NULL) {
            perror("Failed to allocate memory for variable header");
            exit(EXIT_FAILURE);
        }
        memcpy(received_pck.variable_header, buffer + offset, received_pck.variable_len); //copy var_head bytes from buffer starting at offset
        
        // Extract Packet ID
        received_pck.pck_id = (buffer[offset] << 8) | buffer[offset + 1]; //copy packet id bytes to mqtt_pck

        //compute payload len
        received_pck.payload_len = received_pck.remaining_len - received_pck.variable_len;

        //fill payload
        received_pck.payload = malloc(received_pck.payload_len); // Allocate var_head bytes (CONNECT variable header)
        if (received_pck.payload == NULL) {
            perror("Failed to allocate memory for payload");
            exit(EXIT_FAILURE);
        }
        memcpy(received_pck.payload, buffer + offset + received_pck.variable_len, received_pck.payload_len); //copy X bytes from buffer starting after variable header

        return subscribe_handler(&received_pck, running_sessions);

    case 12:
        printf("PING Request\n");
        return send_pingresp(&received_pck);

    case 14:
        printf("DISCONNECT\n");
        return disconnect_handler(&received_pck, running_sessions);
    default:
        return -1;
    }
    return 0;
}
//============================================================================================================================//
//============================================================================================================================//
//============================================================================================================================//
//============================================================================================================================//

//disconnects client properly
int disconnect_handler(mqtt_pck *received_pck, session* running_sessions){
    //find the running session with matching conn_fd
    session *current_session = NULL;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (running_sessions[i].conn_fd == received_pck->conn_fd) {
            current_session = &running_sessions[i];
            break;
        }
    }
    if (current_session == NULL) {
        printf("Session not found for conn_fd: %d\n", received_pck->conn_fd);
        return -1;
    }

    printf("DISCONNECTION || conn_fd: %d || Client_ID: '%s'\n", current_session->conn_fd, current_session->client_id);
    current_session->last_pck_received_id = 0; //reset last packet id
    close(current_session->conn_fd);
    current_session->conn_fd = 0;
    pthread_exit(NULL);
}

//handle(interprets) CONNECT packet
int connect_handler(mqtt_pck *received_pck, session* running_sessions){
    int return_code = 0; 
    int session_present = 0;

    //check variable header
    uint8_t expected_protocol[8] = {0x00, 0x04, 0x4D, 0x51, 0x54, 0x54, 0x04, 0x02};
    if (memcmp(received_pck->variable_header, expected_protocol, 8) != 0){
        printf("Invalid protocol\n");
        return_code = 1;
    }
    int keepalive = received_pck->variable_header[9];
    
    //Check payload
    int id_len = (received_pck->payload[0] << 8)  | received_pck->payload[1];

    char* client_id = malloc(id_len);
    if (client_id == NULL) {
        perror("Failed to allocate memory for client id");
        exit(EXIT_FAILURE);
    }
    memcpy(client_id, received_pck->payload + 2, id_len);

    //check if client_id exists in any session
    int session_idx = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (running_sessions[i].client_id != NULL) {
            //compare existing session client_id with the received client_id
            if (strcmp(running_sessions[i].client_id, client_id) == 0) {
                printf("Ongoing session found for Client_ID: %s || conn_fd: %d || index %d\n", running_sessions[i].client_id, running_sessions[i].conn_fd, i);
                session_idx = i;
                session_present = 1; // Mark session as present
                break;
            }
        } 
        else if (session_idx == -1) {
            //save first available slot for a new session
            session_idx = i;
        }
    }
    //associate client info with session
    running_sessions[session_idx].client_id = client_id;
    running_sessions[session_idx].conn_fd = received_pck->conn_fd;
    running_sessions[session_idx].keepalive = keepalive;

    printf("Valid Protocol || Keepalive: %d || Client_ID: %s || SessionIdx: %d\n", keepalive, client_id, session_idx);

    //assign the new connection to the corresponding session
    return send_connack(&running_sessions[session_idx], return_code, session_present);
}

//Prepares and sends connack packet
int send_connack(session* current_session, int return_code, int session_present) {
    mqtt_pck connack_packet;

    //fixed Header
    connack_packet.flag = 0;
    connack_packet.pck_type = 2;
    connack_packet.remaining_len = 2;

    //variable Header
    connack_packet.variable_len = 2;
    connack_packet.variable_header = malloc(connack_packet.variable_len); //allocate 7 bytes (PUBLISH variable header)
    if (!connack_packet.variable_header) {
        perror("Failed to allocate memory for CONNACK variable header");
        return -1;
    }
    connack_packet.variable_header[0] = session_present & 0x01; // Reserved(0000) || SessionPresent(which is 1 or 0)
    connack_packet.variable_header[1] = return_code; //Connect Return Code (only 0x00 or 0x01)

    //payload
    connack_packet.payload_len = 0; //n payload
    connack_packet.payload = NULL;  //set pointer to NULL

    //conn_fd
    connack_packet.conn_fd = current_session->conn_fd;
    if (send_pck(&connack_packet) < 0){
        printf("Failed to send CONNACK\n");
        free(connack_packet.variable_header);
        return -1;
    }
    printf("CONNACK sent successfully\n");
    return 0;
}


//Sends PingResp packet(no need for handler before)
int send_pingresp(mqtt_pck *received_pck) {
    mqtt_pck pingresp_packet;

    //fixed Header
    pingresp_packet.flag = 0;
    pingresp_packet.pck_type = 13; //PINGRESP packet type
    pingresp_packet.remaining_len = 0; //remaining length is 0

    //no Variable Header
    pingresp_packet.variable_header = NULL;

    //no Payload
    pingresp_packet.payload = NULL;
    pingresp_packet.payload_len = 0;

    //connection file descriptor
    pingresp_packet.conn_fd = received_pck->conn_fd;

    if (send_pck(&pingresp_packet) < 0) {
        perror("Failed to send PING packet");
        return -1;
    }

    printf("PINGRESP sent successfully\n");
    return 0;
}

//handle SUBSCRIBE packet
int subscribe_handler(mqtt_pck *received_pck, session *running_sessions) {
    //find the running session with matching conn_fd
    session *current_session = NULL;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (running_sessions[i].conn_fd == received_pck->conn_fd) {
            current_session = &running_sessions[i];
            break;
        }
    }
    if (current_session == NULL) {
        printf("Session not found for conn_fd: %d\n", received_pck->conn_fd);
        return -1;
    }

    //print received packet ID
    printf("Packet ID: %d\n", received_pck->pck_id);

    //process the payload
    int offset = 0;
    int num_topics = 0;

    while (offset < received_pck->payload_len) {
        //check topic length from the first two bytes of the payload
        uint16_t topic_len = (received_pck->payload[offset] << 8) | received_pck->payload[offset + 1];
        offset += 2;

        //ensure topic length is within valid range
        if (topic_len <= 0 || topic_len > received_pck->payload_len - offset) {
            printf("Invalid topic length: %d\n", topic_len);
            return -1;
        }

        //extract topic name
        char topic[topic_len + 1];  //+1 for null-terminator
        memcpy(topic, received_pck->payload + offset, topic_len);
        topic[topic_len] = '\0'; //properly terminate the topic string
        offset += topic_len;

        //check if the QoS is valid
        uint8_t qos = received_pck->payload[offset];
        if (qos > 1) { //only QoS 1 or 0
            printf("Ignoring topic '%s' with unsupported QoS level: %d\n", topic, qos);
            offset += 1;
            continue;
        }
        offset++; //move past the QoS byte

        //check if the topic already exists in the client's session
        bool topic_exists = false;
        for (int i = 0; i < MAX_TOPICS; i++) {
            if (strcmp(current_session->topic[i], topic) == 0) {
                topic_exists = true;
                printf("Topic '%s' already exists in the session with conn_fd: %d in topic_id: %d\n", topic, current_session->conn_fd, i);
                break;
            }
        }

        //store the topic if it's new
        if (!topic_exists) {
            for (int i = 0; i < MAX_TOPICS; i++) {
                if (current_session->topic[i][0] == '\0') { //empty topic slot found
                    strncpy(current_session->topic[i], topic, sizeof(current_session->topic[i]) - 1);
                    current_session->topic[i][sizeof(current_session->topic[i]) - 1] = '\0';  //ensure null termination
                    printf("Stored new topic: '%s' in the session with conn_fd: %d in topic_id: %d\n", topic, current_session->conn_fd, i);
                    break;
                }
            }
        }

        num_topics++;
    }

    //send a SUBACK packet back to the client after processing all topics
    return send_suback(current_session, received_pck->pck_id, num_topics); //not entire received_pck necessary for acknowledgment, only packet id and number of subscripted topics in this message
}

//send SUBACK
int send_suback(session *current_session, int pck_id, int num_topics) {
    mqtt_pck suback_packet;

    //fixed Header
    suback_packet.flag = 0;
    suback_packet.pck_type = 9; // SUBACK control packet type
    suback_packet.remaining_len = 2 + num_topics; // Packet Identifier (2 bytes) + Payload

    //variable Header (Packet Identifier)
    suback_packet.variable_len = 2;
    suback_packet.variable_header = malloc(suback_packet.variable_len); 
    if (!suback_packet.variable_header) {
        perror("Failed to allocate memory for SUBACK variable header");
        return -1;
    }
    suback_packet.variable_header[0] = (pck_id >> 8) & 0xFF; // MSB of pck_id
    suback_packet.variable_header[1] = pck_id & 0xFF;        // LSB of pck_id

    //payload (QoS Levels for each topic)
    suback_packet.payload_len = num_topics;
    suback_packet.payload = malloc(num_topics);
    if (!suback_packet.payload) {
        perror("Failed to allocate memory for SUBACK payload");
        free(suback_packet.variable_header);
        return -1;
    }
    
    //setting QoS level for each topic (set to QoS 1 for all)
    for (int i = 0; i < num_topics; i++) {
        suback_packet.payload[i] = 0x01;  // QoS 1 for each topic
    }

    //assign the connection file descriptor
    suback_packet.conn_fd = current_session->conn_fd;

    //Send the SUBACK packet using send_pck
    if (send_pck(&suback_packet) < 0) {
        printf("Failed to send SUBACK\n");
        free(suback_packet.variable_header);
        free(suback_packet.payload);
        return -1;
    }

    //Clean up allocated memory
    free(suback_packet.variable_header);
    free(suback_packet.payload);

    printf("SUBACK sent successfully for Packet_ID: %d\n", pck_id);
    return 0;
}

//handle(interprets) PUBISH packet
int publish_handler(mqtt_pck *received_pck, session* running_sessions) {
    //find the running session with matching conn_fd
    session *current_session = NULL;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (running_sessions[i].conn_fd == received_pck->conn_fd) {
            current_session = &running_sessions[i];
            break;
        }
    }
    if (current_session == NULL) {
        printf("Session not found for conn_fd: %d\n", received_pck->conn_fd);
        return -1;
    }

    int Retain = received_pck->flag & 0x01;
    if (Retain != 0) {
        printf("Invalid Retain\n");
    }
    int QOS_lvl = (received_pck->flag >> 1) & 0x03;
    if (QOS_lvl != 1) {
        printf("Invalid QOS level\n");
    }

    //check if its first time the client sent the message
    int DUP = (received_pck->flag >> 3) & 0x01;

    char topic[received_pck->topic_len + 1];

    memcpy(topic, received_pck->variable_header + 2, received_pck->topic_len);
    topic[received_pck->topic_len] = '\0';

    printf("Topic: %s\n", topic);

    int pck_id_offset = 2 + received_pck->topic_len; //where the pck_id starts, duo to variable topic length
    received_pck->pck_id = (received_pck->variable_header[pck_id_offset] << 8) |
                 received_pck->variable_header[pck_id_offset + 1];
    
    printf("DUP: %d || Topic: '%s' || pck_id: %d\n", DUP, topic, received_pck->pck_id);

    // verify it wasn't received before
    if (received_pck->pck_id != current_session->last_pck_received_id) {
        printf("New message to publish\n");
        current_session->last_pck_received_id = received_pck->pck_id;

        //Find clients that are subscribed and save message to queue
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (running_sessions[i].client_id != NULL) {
                for (int j = 0; j < MAX_TOPICS; j++) {
                    if (strcmp(running_sessions[i].topic[j], topic) == 0) { //comparing topics
                        printf("Queuing message to Client_ID '%s' || conn_fd %d || Subscribed to topic '%s' || ", running_sessions[i].client_id, running_sessions[i].conn_fd, topic);
                        queue_publish(received_pck, &running_sessions[i]);
                    }
                }
            }
        }
    } 
    else {
        printf("Duplicated message\n");
        return 0;
    }
    return send_puback(current_session, received_pck->pck_id); //not entire received_pck necessary for acknowledgment, only packet id
}

int send_puback(session* current_session, int pck_id){
    printf("Sending PUBACK to conn_fd %d\n", current_session->conn_fd);
    mqtt_pck puback_packet;

    //fixed Header
    puback_packet.flag = 0;
    puback_packet.pck_type = 4;
    puback_packet.remaining_len = 2;

    //variable Header
    puback_packet.variable_len = 2;
    puback_packet.variable_header = malloc(puback_packet.variable_len); //allocate 7 bytes (PUBLISH variable header)
    if (!puback_packet.variable_header) {
        perror("Failed to allocate memory for PUBACK variable header");
        return -1;
    }
    puback_packet.variable_header[0] = (pck_id >> 8); // MSB of pck_id(shift right to eliminate LSB) 
    puback_packet.variable_header[1] = pck_id & 0xFF; // LSB of pck_id

    //payload
    puback_packet.payload_len = 0; //n payload
    puback_packet.payload = NULL;  //set pointer to NULL

    //conn_fd
    puback_packet.conn_fd = current_session->conn_fd;
    if (send_pck(&puback_packet) < 0){
        printf("Failed to send PUBACK\n");
        free(puback_packet.variable_header);
        free(puback_packet.payload);
        return -1;
    }

    //Clean up allocated memory
    free(puback_packet.variable_header);
    free(puback_packet.payload);
    printf("PUBACK sent successfully\n");
    return 0;
}

int puback_handler(mqtt_pck *received_pck, session* running_sessions){
    //find the running session with matching conn_fd
    session *current_session = NULL;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (running_sessions[i].conn_fd == received_pck->conn_fd) {
            current_session = &running_sessions[i];
            break;
        }
    }
    if (current_session == NULL) {
        printf("Session not found for conn_fd: %d\n", received_pck->conn_fd);
        return -1;
    }

    //extract packet id, to find which publish message is this acknowledge refering to
    int puback_pck_id = (received_pck->variable_header[0] << 8) | received_pck->variable_header[1]; //MSB (shift left) and LSB convertion

    for (int i=0; i < MAX_PUB_QUEUE_SIZE; i++){ //for each queue slot   
        if (current_session->pck_to_send[i].pck_type == 0){ //if slot empty, continue
            continue;
        }
        else if (current_session->pck_to_send[i].pck_id == puback_pck_id){ //slot has message and pck_id equal to the acknowledge
            printf("Clearing Queue Slot: %d\n", i);
            memset(&current_session->pck_to_send[i], 0, sizeof(mqtt_pck)); //clear slot
            return 0;
        }
    }
    printf("ERROR-NO QUEUE FOUND\n");
    return -1;
}


int queue_publish(mqtt_pck *received_pck, session* running_session) {
    //find an available slot in the publish queue
    for (int i = 0; i < MAX_PUB_QUEUE_SIZE; i++) {
        if (running_session->pck_to_send[i].pck_type == 0) {  //if slot is empty save the publish into the queue
            running_session->pck_to_send[i] = *received_pck; //associate pending message with destination client's session
            running_session->pck_to_send[i].conn_fd = running_session->conn_fd; //destination of packet associated with found subscribed client's session

            printf("Queue Slot: %d\n", i);

            printf("FOWARDING PUBLISH to Client\n");
            if (send_pck(&running_session->pck_to_send[i]) < 0) {  //first attempt to send the message; queue thread will resend if not sucessfull
                printf("FOWARD FAILURE\n");
            }
            running_session->pck_to_send[i].first_forward = 1;
            return 0;
        }
    }
    //if Queue is full
    printf("Queue ERROR-FULL\n");
    return -1;
}

//queue loop function, 1 for all threads, responsible for fowarding PUBLISH messages
void *queue_handler(void *arg) {
    printf("Queue Handling Thread created\n");
    thread_data *t_data = (thread_data *)arg;             //cast to thread data type again
    session *running_sessions = t_data->running_sessions; //only running_sessions data is required
    free(t_data); //no longer needed, free

    double elapsed_time; //to know time between retransmissions
    clock_t time_now;
    //Search for unsent queues
    while (1) {
        time_now = clock();
        for (int i=0; i < MAX_CLIENTS; i++){ //for each possible session
            for (int j=0; j < MAX_PUB_QUEUE_SIZE; j++){ //for each queue slot
                if (running_sessions[i].pck_to_send[j].first_forward == 0 ){ //if message hasn't been sent first
                    continue;
                }
                else{ //slot has message
                    if (running_sessions[i].conn_fd != 0){ //if client is connected
                        running_sessions[i].pck_to_send[j].conn_fd = running_sessions[i].conn_fd; //in case the reconection got a diferent conn_fd, make sure packet has correct new conn_fd

                        elapsed_time = (double)(time_now - running_sessions[i].pck_to_send[j].time_sent) / CLOCKS_PER_SEC;
                        if (elapsed_time > TIME_TO_RETRANSMIT){ //if retransmition time has passed
                            printf("RETRANSMISSIONING->PUBLISH to Client_ID: '%s' || conn_fd: %d || Queue Slot: %d\n", running_sessions[i].client_id, running_sessions[i].conn_fd, j);
                            if (send_pck(&running_sessions[i].pck_to_send[j]) < 0) {  //send the message
                                printf("RETRANSMISSIONING FAILURE\n");
                                continue;
                            }
                            running_sessions[i].pck_to_send[j].time_sent = time_now;
                        }
                    }
                }
            }
        }
        usleep(10); ////to not overload CPU
    }
    return NULL;
}