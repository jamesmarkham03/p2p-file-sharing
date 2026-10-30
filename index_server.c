#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

// PDU structure used for all communication
struct pdu {
    char type;
    char data[100];
};

// This struct represents ONE registration entry in the index server
struct content {
    char peerName[10];      // who registered
    char contentName[10];   // what file they host
    char address[20];       // their IP address
    char port[6];           // TCP port to download from
};

struct content contents[32];     // the “database” (max 32 entries)
int contentsSize = 0;            // how many entries in the index
int frequency[32];               // download count for least-used rule

// helper functions
uint32_t searchContent(char* content);
uint32_t searchName(char* name);
int getIndex(uint32_t bitArray);
int getLowestFrequencyIndex(uint32_t bitArray);

int main(int argc, char *argv[]) {

    struct sockaddr_in fsin;  // sender info
    char buf[100];
    int alen;
    struct sockaddr_in sin;
    int s;
    int port = 3000;
    int i;

    // ----------------------------------------------------------
    // Handle command line arguments (./index_server 3000)
    // ----------------------------------------------------------
    switch(argc){
        case 1: break;
        case 2: port = atoi(argv[1]); break;
        default:
            fprintf(stderr, "Usage: %s [port]\n", argv[0]);
            exit(1);
    }

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = INADDR_ANY;    // listen on ANY address
    sin.sin_port = htons(port);

    // ----------------------------------------------------------
    // INDEX SERVER'S UDP SOCKET
    // ----------------------------------------------------------
    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        fprintf(stderr, "can't create socket\n");

    if (bind(s, (struct sockaddr *)&sin, sizeof(sin)) < 0)
        fprintf(stderr, "can't bind to %d port\n", port);

    alen = sizeof(fsin);

    int search, index;
    struct pdu rpdu, spdu;
    char peerName[10], contentName[10];

    // ==========================================================
    // INDEX SERVER MAIN LOOP
    // ==========================================================
    while (1) {

        // Wait for PDU from any peer (register, search, list, deregister)
        if (recvfrom(s, &rpdu, sizeof(struct pdu), 0, (struct sockaddr *)&fsin, &alen) < 0)
            fprintf(stderr, "recvfrom error\n");

        // ======================================================
        // SWITCH ON PDU TYPE
        // ======================================================
        switch (rpdu.type) {


// =======================================================================
// ======================= PART A — REGISTRATION ('R') ====================
// =======================================================================
            case 'R': 
                // Build a new index entry from registration request
                struct content cont;

                // First 10 bytes = peer name
                strcpy(cont.peerName, rpdu.data);

                // Next 10 bytes = content name
                strcpy(cont.contentName, rpdu.data + 10);

                // Index server records the sender's IP address
                strcpy(cont.address, inet_ntoa(fsin.sin_addr));

                // TCP port (as string)
                strcpy(cont.port, rpdu.data + 20);

                // Duplicate? (same peer + same content)
                if (searchContent(cont.contentName) & searchName(cont.peerName)) {
                    spdu.type = 'E';
                    strcpy(spdu.data, "Content already registered with this name");
                    sendto(s, &spdu, sizeof(spdu), 0, (struct sockaddr*)&fsin, sizeof(fsin));
                    break;
                }

                // Add entry to index server
                contents[contentsSize] = cont;
                contentsSize++;

                // Acknowledge
                spdu.type = 'A';
                spdu.data[0] = '\0';
                sendto(s, &spdu, sizeof(spdu), 0, (struct sockaddr*)&fsin, sizeof(fsin));
                break;


// =======================================================================
// ===================== PART B — SEARCH / DOWNLOAD ('S') =================
// =======================================================================
            case 'S':
                // Peer is asking: “Who has this file?”
                struct content con;
                strcpy(con.peerName, rpdu.data);
                strcpy(con.contentName, rpdu.data + 10);

                // Find all peers with this content
                search = searchContent(con.contentName);

                if (!search) {
                    // No one has it
                    spdu.type = 'E';
                    strcpy(spdu.data, "Content does not exist");
                    sendto(s, &spdu, sizeof(spdu), 0, (struct sockaddr*)&fsin, sizeof(fsin));
                    break;
                }
                else if (searchName(con.peerName) & search) {
                    // Prevent someone from downloading their own content with same name
                    spdu.type = 'E';
                    strcpy(spdu.data, "Another peer with the name already registered the content");
                    sendto(s, &spdu, sizeof(spdu), 0, (struct sockaddr*)&fsin, sizeof(fsin));
                    break;
                }

                // ------------------------------
                // LEAST-USED PEER SELECTION RULE
                // ------------------------------
                index = getLowestFrequencyIndex(search);

                // Reply with IP + PORT of selected content server
                spdu.type = 'S';
                strcpy(spdu.data, contents[index].address);
                strcpy(spdu.data + 20, contents[index].port);

                sendto(s, &spdu, sizeof(spdu), 0, (struct sockaddr*)&fsin, sizeof(fsin));

                // Increment frequency to track load balancing
                frequency[index]++;
                break;


// =======================================================================
// ============================= PART C — LISTING =========================
// =======================================================================
            case 'O':
                // Send number of records first
                spdu.type = 'O';
                sprintf(spdu.data, "%d", contentsSize);
                sendto(s, &spdu, sizeof(spdu), 0, (struct sockaddr*)&fsin, sizeof(fsin));

                // Then send each entry
                for (i = 0; i < contentsSize; i++) {
                    sprintf(spdu.data, "name: %s\ncontent name: %s\naddress: %s:%s\n",
                            contents[i].peerName,
                            contents[i].contentName,
                            contents[i].address,
                            contents[i].port);

                    sendto(s, &spdu, sizeof(spdu), 0, (struct sockaddr*)&fsin, sizeof(fsin));
                }
                break;


// =======================================================================
// =================== PART D — DEREGISTRATION ('T') ======================
// =======================================================================
            case 'T':

                // Extract values from PDU
                strcpy(peerName, rpdu.data);
                strcpy(contentName, rpdu.data + 10);

                // Check if this exact peer/content exists
                search = searchName(peerName) & searchContent(contentName);

                if (search) {
                    // Remove it from index
                    index = getIndex(search);

                    memmove(contents + index,
                            contents + index + 1,
                            (--contentsSize - index) * sizeof(struct content));

                    // Reset frequency count for that entry
                    frequency[index] = 0;

                    // Acknowledge successful remove
                    spdu.type = 'A';
                    spdu.data[0] = '\0';
                    sendto(s, &spdu, sizeof(spdu), 0, (struct sockaddr*)&fsin, sizeof(fsin));
                }
                else {
                    spdu.type = 'E';
                    strcpy(spdu.data, "Data is not registered\n");
                    sendto(s, &spdu, sizeof(spdu), 0, (struct sockaddr*)&fsin, sizeof(fsin));
                }
                break;

            default:
                ;
        }
    }
}


// =======================================================================
// =========================== HELPER FUNCTIONS ===========================
// =======================================================================

// returns bitmask of indexes where peerName matches
uint32_t searchName(char* name){
    int i;
    unsigned int sum = 0;

    for (i = 0; i < contentsSize; i++){
        if (!strcmp(name, contents[i].peerName)){
            sum += 1 << i;
        }
    }
    return sum;
}

// returns bitmask of indexes where contentName matches
uint32_t searchContent(char* content){
    int i;
    unsigned int sum = 0;

    for (i = 0; i < contentsSize; i++){
        if (!strcmp(content, contents[i].contentName)){
            sum += 1 << i;
        }
    }
    return sum;
}

// Converts a bitmask with ONE 1-bit into its index
int getIndex(uint32_t bitArray){
    int count = 0;

    while(bitArray != 1){
        bitArray = bitArray >> 1;
        count++;
    }
    return count;
}

// For least-used rule: pick the index with the lowest frequency
int getLowestFrequencyIndex(uint32_t bitArray){
    int count = 0;
    int lowest = 100;
    int lowestIndex = 0;

    while(bitArray > 0){
        if (bitArray % 2 == 1){
            if(lowest > frequency[count]){
                lowestIndex = count;
                lowest = frequency[count];
            }
        }
        bitArray = bitArray >> 1;
        count++;
    }
    return lowestIndex;
}
