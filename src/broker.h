#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <errno.h>

#include <fcntl.h>
#include <pthread.h>
#include <time.h>

#ifndef _ARPA_INET_H_
#define _ARPA_INET_H_

#endif
//=============================================================//
//most of the limitations to our simplified MQTT broker are from the limits here, which could be dynamic but would take more work and memory allocation
#define BROKER_PORT 1883
#define MAX_CLIENTS 10
#define MAX_TOPICS 5
#define MAX_PUB_QUEUE_SIZE 10 
#define TIME_TO_RETRANSMIT 5.0   //time in seconds before retransmission is tried, in case PUBLISH doesnt receive PUBACK
#define QOS 1

#define BUFFER_SIZE 1024

//packet structure
typedef struct {
    //fixed header
    uint8_t pck_type;
    uint8_t flag;
    ssize_t remaining_len;

    //variable Header (depends on the packet type)
    ssize_t topic_len;
    ssize_t variable_len;
    uint8_t *variable_header;

    //payload
    ssize_t payload_len;
    uint8_t *payload;

    int conn_fd;                   //connection file descriptor

    int pck_id;       //if present, represents the packet id

    int first_forward; //used to know if the message was tried to send once before
    clock_t time_sent; //used to know when to resend publish packet

} mqtt_pck;

//session required arguments to save
typedef struct {
    int conn_fd;                   //connection file descriptor
    int keepalive;                //time between finishing 1 packet and next packet, in seconds
    char topic[MAX_TOPICS][256];  //Client's subscripted topics (at maximum all topics)

    char* client_id;
    int last_pck_received_id;     //pck id of last received message from this session's client
    mqtt_pck pck_to_send[MAX_PUB_QUEUE_SIZE]; //queue of publish messages to send to this client
} session;

//for each thread
typedef struct {
    int conn_fd;
    session *running_sessions;
} thread_data;

#ifndef MQTT_RETURN_CODES_H
#define MQTT_RETURN_CODES_H

//MQTT Connect Return Code Responses
#define MQTT_CONN_ACCEPTED                  0x00  // Connection accepted
#define MQTT_CONN_REFUSED_ID_REJECTED       0x02  // Connection Refused, identifier rejected

#endif // MQTT_RETURN_CODES_H

//function creates server at local ip and given port
int create_tcpserver(int *server_fd, struct sockaddr_in *address, int *addrlen);
//main loop function, for each thread
void *client_handler(void *arg);
//queue loop function, 1 for all threads, responsible for fowarding PUBLISH messages
void *queue_handler(void *arg);
//function to decode the remaining length
int decode_remaining_length(uint8_t *buffer, uint8_t *remaining_length, int *offset);
//function to encode the remaining length
int encode_remaining_length(uint8_t *buffer, size_t remaining_len);
//function to easily made packet(only fill a variable of type structure mqtt_pck)
int send_pck(mqtt_pck *packet);
//determine type of packet and process
int mqtt_process_pck(uint8_t *buffer, mqtt_pck received_pck, session* running_sessions);
//disconnects client properly
int disconnect_handler(mqtt_pck *received_pck, session* running_sessions);
//handle(interprets) CONNECT packet
int connect_handler(mqtt_pck *received_pck, session* running_sessions);
//Prepares and sends connack packet
int send_connack(session* current_session, int return_code, int session_present);
//Sends PingResp packet(no need for handler before)
int send_pingresp(mqtt_pck *received_pck);
//handle(interprets) PUBISH packet
int publish_handler(mqtt_pck *received_pck, session* running_sessions);
//send puback
int send_puback(session* current_session, int pck_id);
//handle PUBACK response
int puback_handler(mqtt_pck *received_pck, session* running_sessions);
//queue publish
int queue_publish(mqtt_pck *received_pck, session* running_session);
//handle SUBSCRIBE packet
int subscribe_handler(mqtt_pck *received_pck, session* running_sessions);
//send SUBACK response
int send_suback(session *current_session, int pck_id, int num_topics);