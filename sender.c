#include <errno.h>
#include <stdbool.h>
#include <stdio.h>    // for printf() and file i/o
#include <stdlib.h>   // for exit()
#include <string.h>   // for memset()
#include <unistd.h>   // for close()
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>

#define SERVER_ADDR "127.0.0.1"

#define HEADER_SIZE 2
#define DATA_SIZE 500
#define PACKET_SIZE HEADER_SIZE + DATA_SIZE
#define ACKPACKET_SIZE 1

#define WINDOW_SIZE 10

// 100 ms
#define TIMEOUT_US 100000
// or max wait time = 100 * 100 ms = 10 seconds
#define MAX_TIMEOUTS 100
#define MAX_START_SEQ MAX_TIMEOUTS

// --- HELPER FUNCTIONS ---
void freeResources(int sock, FILE *file) {
    if (sock != -1)
        close(sock);
    
    if (file != NULL)
        fclose(file);
}

void die(char *error, int sock, FILE *file) {
    printf("\n");
    perror(error);
    freeResources(sock, file);
    exit(-1);
}

void printProgress(long bytes) {
    printf("\r%*c", 50, ' ');
    printf("\rSent: %ld bytes", bytes);
    fflush(stdout);
}

// --- CORE FUNCTIONS ---
// Opens a UDP socket and returns the ID
int openSocket(struct sockaddr_in *sockInOut, int sockInOutLen, int port) {
    int sock;
    if ((sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
        die("Error: could not open socket.\n", sock, NULL);

    memset((char*)sockInOut, 0, sockInOutLen);

    sockInOut->sin_family = AF_INET;
    sockInOut->sin_port = htons(port);

    if (inet_aton(SERVER_ADDR, &sockInOut->sin_addr) == 0)
        die("Error: could not set up the server address in the socket.\n",
            sock, NULL);
    
    // Set timeout for ack recv
    struct timeval tval;
    tval.tv_sec = 0;
    tval.tv_usec = TIMEOUT_US;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tval, sizeof(tval)) == -1)
        die("Error: could not set timeout.\n", sock, NULL);

    return sock;
}

// All packets should be of PACKET_SIZE bytes, except the final packet
// which might not, unless the file size is perfectly divisible by DATA_SIZE.
// This function calculates and returns packet size, accomadating that case.
int calculatePacketSize(int bufSize, bool isLastPacket) {
    int packetSize;
    if (isLastPacket) {
        const int remainder = bufSize % DATA_SIZE;
        // If last packet is utilizing complete buffer size
        if (remainder == 0)
            packetSize = PACKET_SIZE;
        else
            packetSize = remainder + HEADER_SIZE;
    }
    else {
        packetSize = PACKET_SIZE;
    }
    
    return packetSize;
}

void initAcks(bool *acks) {
    for (int i = 0; i < WINDOW_SIZE; i++)
        acks[i] = false;
}

// Receives acks and returns total acks received after timeout
int recvAcks(struct sockaddr_in *sockInOut, int *sockInOutLen,
    int sock, int seq, bool *acks, FILE *file) {
    
    char buf[ACKPACKET_SIZE];
    int totalAcks = 0;
    static int totalTimeouts = 0;
    while (true) {
        if (recvfrom(sock, buf, ACKPACKET_SIZE, 0,
            (struct sockaddr*)sockInOut, sockInOutLen) == -1) {
            
            // Ack timeout
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                totalTimeouts++;
                if (totalTimeouts > MAX_TIMEOUTS)
                    die("Receiver not responding.\n", sock, file);
                
                break;
            }
            else {
                die("Error: recvfrom() failed.\n", sock, file);
            }
        }

        // If we have made this far, it means we got at least one ACK,
        // so reset total timeouts
        totalTimeouts = 0;

        bool isPrevWindowAck = true;
        const int seqEnd = seq + WINDOW_SIZE - 1;
        if (buf[0] >= seq && buf[0] <= seqEnd)
            isPrevWindowAck = false;
        
        int packetNumber = buf[0] % WINDOW_SIZE;
        // Discard old (previous window) and duplicate ACKs
        if (!isPrevWindowAck && !acks[packetNumber]) {
            acks[packetNumber] = true;
            totalAcks++;
        }
    }
    return totalAcks;
}

// Reliably sends packets for the next window
void sendPackets(const char *buf, int bufSize, int seq, int sock,
    struct sockaddr_in *sockInOut, int *sockInOutLen, FILE *file) {
    
    int numberOfPackets = bufSize / DATA_SIZE;
    if (bufSize % DATA_SIZE != 0)
        numberOfPackets += 1;
    
    bool acks[WINDOW_SIZE];
    initAcks(acks);
    int totalAcks = 0;

    while (totalAcks != numberOfPackets) {
        int bufIndex = 0;
        for (int i = 0; i < numberOfPackets; i++) {
            bool isLastPacket = (i + 1 == numberOfPackets);
            const int packetSize = calculatePacketSize(bufSize, isLastPacket);
            // Skip already acked packets by incrementing index appropriately
            if (acks[i]) {
                bufIndex += packetSize - HEADER_SIZE;
                continue;
            }
            
            // Make a packet
            char packet[PACKET_SIZE];
            memcpy(packet + HEADER_SIZE, buf + bufIndex, packetSize);
            packet[0] = seq + i;
            // Last packet of last window
            if (isLastPacket && feof(file))
                packet[1] = true;
            else
                packet[1] = false;
            
            // Send the packet
            if (sendto(sock, packet, packetSize, 0,
                (struct sockaddr*)sockInOut, *sockInOutLen) == -1)
                die("Error: sendto() failed.\n", sock, file);
            
            bufIndex += packetSize - HEADER_SIZE;
        }
        totalAcks += recvAcks(sockInOut, sockInOutLen, sock, seq, acks,
            file);
    }
}

void sendFile(const char *filename, int port) {
    // Open socket
    struct sockaddr_in sockInOut;
    int sockInOutLen = sizeof(sockInOut);
    int sock = openSocket(&sockInOut, sizeof(sockInOut), port);

    // Open file
    FILE *file = fopen(filename, "rb");
    if (file == NULL)
        die("Error: could not open given file.\n", sock, file);
    
    int seq = 0;
    // Read and send
    while (true) {
        const int bufSize =  DATA_SIZE * WINDOW_SIZE;
        char buf[bufSize];
        int bytesRead = fread(buf, sizeof(char), bufSize, file);
        // Terminate when no more bytes in file
        if (bytesRead == 0)
            break;
        
        sendPackets(buf, bytesRead, seq, sock, &sockInOut, &sockInOutLen, file);
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
        printf("Usage: sender [filename] [port]\n");
        exit(-1);
    }

    printProgress(0);
    sendFile(argv[1], atoi(argv[2]));
    printf("\nSuccess.\n");
    return 0;
}
