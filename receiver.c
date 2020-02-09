#include <stdbool.h>
#include <stdio.h>    // for printf() and file i/o
#include <stdlib.h>   // for exit()
#include <string.h>   // for memset()
#include <unistd.h>   // for close()
#include <arpa/inet.h>
#include <sys/socket.h>

#define HEADER_SIZE 2
#define DATA_SIZE 500
#define PACKET_SIZE HEADER_SIZE + DATA_SIZE
#define ACKPACKET_SIZE 1

#define MAX_START_SEQ 100
#define WINDOW_SIZE 10

// --- HELPER FUNCTIONS ---
void freeResources(int sock, FILE *file) {
    if (sock != -1)
        close(sock);
    
    if (file != NULL)
        fclose(file);
}

void die(char *error, int sock, FILE *file) {
    perror(error);
    freeResources(sock, file);
    exit(-1);
}

void printProgress(long bytes) {
    printf("\r%*c", 50, ' ');
    printf("\rReceived: %ld bytes", bytes);
    fflush(stdout);
}

// --- CORE FUNCTIONS ---
// Opens a UDP socket and returns the ID
int openSocket(struct sockaddr_in *sockIn, int sockInLen, int port) {
    int sock;
    if ((sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
        die("Error: could not open socket.\n", sock, NULL);

    memset((char*)sockIn, 0, sockInLen);

    sockIn->sin_family = AF_INET;
    sockIn->sin_port = htons(port);
    sockIn->sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr*)sockIn, sockInLen) == -1)
        die("Error: could not bind server.\n", sock, NULL);

    return sock;
}

void initPacketsReceived(bool *packetsReceived) {
    for (int i = 0; i < WINDOW_SIZE; i++)
        packetsReceived[i] = false;
}

// Receives packets for this window and sends ack for every packet
// received. Returns the total data in bytes received
int recvPackets(char *bufOut, bool *eofOut, int sock, FILE *file,
    struct sockaddr_in *sockInOut, int *sockInOutLen, int seq) {
    
    int totalPacketsExpected = WINDOW_SIZE;
    bool packetsReceived[WINDOW_SIZE];
    initPacketsReceived(packetsReceived);
    int totalPacketsReceived = 0;
    int bytesReceived = 0;

    while (totalPacketsReceived != totalPacketsExpected) {
        char packet[PACKET_SIZE];
        int received;
        if ((received = recvfrom(sock, packet, PACKET_SIZE, 0,
            (struct sockaddr*)sockInOut, sockInOutLen)) == -1)
            die("Error: recvfrom() failed.\n", sock, file);
        
        const int seqNo = packet[0];
        bool isPrevWindowAck = true;
        const int seqEnd = seq + WINDOW_SIZE - 1;
        if (seqNo >= seq && seqNo <= seqEnd)
            isPrevWindowAck = false;
        
        const int packetNumber = seqNo % WINDOW_SIZE;
        // Discard old (previous window) and duplicate packets,
        // but do send their acks
        if (!isPrevWindowAck && !packetsReceived[packetNumber]) {
            bytesReceived += received - HEADER_SIZE;
            totalPacketsReceived++;

            // If this is the last window's last packet
            if (packet[1]) {
                *eofOut = true;
                // it isn't necessary that the last window's number of
                // packets would be = WINDOW_SIZE
                totalPacketsExpected = packetNumber + 1;
            }

            // Order packet in appropriate position
            memcpy(bufOut + (packetNumber * DATA_SIZE), packet + HEADER_SIZE,
                received - HEADER_SIZE);
        }
        
        // Send ack
        char ackPacket[] = {seqNo};
        if (sendto(sock, ackPacket, ACKPACKET_SIZE, 0,
            (struct sockaddr*)sockInOut, *sockInOutLen) == -1)
            die("Error: failed to send ack.\n", sock, file);
    }
    return bytesReceived;
}

void receiveFile(const char* filename, int port) {
    // Open socket
    struct sockaddr_in sockInOut;
    int sockInOutLen = sizeof(sockInOut);
    int sock = openSocket(&sockInOut, sizeof(sockInOut), port);

    // Create file
    FILE *file = fopen(filename, "wb");
    if (file == NULL)
        die("Error: could not create file.\n", sock, file);
    
    // Receive and write
    bool eof = false;
    int seq = 0;
    while (!eof) {
        char buf[DATA_SIZE * WINDOW_SIZE];
        int bufSize = recvPackets(buf, &eof, sock, file, &sockInOut,
            &sockInOutLen, seq);
        fwrite(buf, sizeof(char), bufSize, file);
        printProgress(ftell(file));

        if (seq == MAX_START_SEQ)
            seq = 0;
        else
            seq += WINDOW_SIZE;
    }
    freeResources(sock, file);
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Usage: receiver [filename] [port]\n");
        exit(-1);
    }

    printProgress(0);
    receiveFile(argv[1], atoi(argv[2]));
    printf("\nSuccess.\n");
    return 0;
}
