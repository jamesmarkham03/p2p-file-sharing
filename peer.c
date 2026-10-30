#include <sys/types.h>

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h> 
#include <sys/signal.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <strings.h>
#include <fcntl.h>

#define BUFSIZE 64
#define MSG "Any Message \n"

// PDU struct used for ALL messages in this assignment
struct pdu {
    char type;
    char data[100];
};

// Local record of what THIS peer is currently hosting
struct registered {
    char name[10];
    char contentName[10];
};

struct registered registeredContent[32];
int regSize = 0;

// local lookup helpers
uint32_t searchName(char* name);
uint32_t searchContent(char* content);
int getIndex(uint32_t bitArray);

// main registration function that sends R-PDU + forks server
void registerContent(int s, char *pname, char *cname);

int main(int argc, char **argv) {

    // -------------------------------
    // DEFAULT index server info
    // -------------------------------
    char *host = "localhost";
    int port = 3000;
    char str[100];

    struct hostent *phe;
    struct sockaddr_in sin;
    int s, n;
    int sd;

    // -------------------------------
    // ARGUMENT HANDLING
    // -------------------------------
    switch (argc) {
        case 1: break;
        case 2: host = argv[1]; break;
        case 3: host = argv[1]; port = atoi(argv[2]); break;
        default:
            fprintf(stderr, "usage: client [host [port]]\n");
            exit(1);
    }

    // fill socket structure for index server
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;                                                        
    sin.sin_port = htons(port);

    // Convert hostname/IP
    if (phe = gethostbyname(host)) {
        memcpy(&sin.sin_addr, phe->h_addr, phe->h_length);
    }
    else if ((sin.sin_addr.s_addr = inet_addr(host)) == INADDR_NONE)
        fprintf(stderr, "Can't get host entry\n");

    // -------------------------------
    // UDP SOCKET TO TALK TO INDEX SERVER
    // -------------------------------
    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        fprintf(stderr, "Can't create UDP socket\n");

    if (connect(s, (struct sockaddr *)&sin, sizeof(sin)) < 0)
        fprintf(stderr, "Can't connect to %s\n", host);

    write(s, MSG, strlen(MSG)); // initial throwaway message

    // -------------------------------
    // MENU VARIABLES
    // -------------------------------
    int select;
    int i, listSize;
    int search, index;

    char downloadPort[6];
    struct sockaddr_in server;
    struct hostent *hp;

    struct pdu spdu, rpdu;
    char pname[10], cname[10], address[20];

    // ============================================================
    // MAIN LOOP
    // ============================================================
    while(1){

        printf("1. Register content\n2. Download content\n3. List content\n4. Deregister content\n5. Quit\n");
        read(0, str, 10);
        select = atoi(str);

        switch(select){

// ====================================================================
// ===================== 1) CONTENT REGISTRATION =======================
// ====================================================================
            case 1:
                printf("Name:\n");
                n = read(0, pname, 10);
                pname[n - 1] = '\0';

                printf("Content Name:\n");
                n = read(0, cname, 10);
                cname[n - 1] = '\0';

                // This function:
                //  - Opens file
                //  - Creates TCP listener for it
                //  - Sends R-PDU to index server
                //  - Forks and serves file
                registerContent(s, pname, cname);
                break;


// ====================================================================
// ======================= 2) CONTENT DOWNLOAD ========================
// ====================================================================
            case 2:
                // basic user input
                printf("Name:\n");
                n = read(0, pname, 10);
                pname[n - 1] = '\0';

                printf("Content Name:\n");
                n = read(0, cname, 10);
                cname[n - 1] = '\0';

                // ----------- SEND S-TYPE TO INDEX SERVER -----------
                // S-type asks: "Who can give me this file?"
                spdu.type = 'S';
                strcpy(spdu.data, pname);
                strcpy(spdu.data + 10, cname);
                write(s, &spdu, sizeof(struct pdu));

                // read response from index (either S or E)
                read(s, (struct pdu*)&rpdu, sizeof(struct pdu));

                if (rpdu.type == 'S') {

                    // rpdu.data contains IP + port of least-used server
                    printf("Index Server selected content server: %s:%s\n",
                           rpdu.data, rpdu.data+20);

                    strcpy(address, rpdu.data);
                    strcpy(downloadPort, rpdu.data+20);

                    port = atoi(downloadPort);

                    // ---------- TCP SOCKET TO DOWNLOAD FROM PEER ----------
                    sd = socket(AF_INET, SOCK_STREAM, 0);

                    bzero((char*)&server, sizeof(struct sockaddr_in));
                    server.sin_family = AF_INET;
                    server.sin_port = htons(port);

                    // build server IP
                    if (hp = gethostbyname(address)) {
                        bcopy(hp->h_addr, (char*)&server.sin_addr, hp->h_length);
                    } else if (inet_aton(address, (struct in_addr*) &server.sin_addr)) {
                        fprintf(stderr, "can't get server address\n");
                    }

                    if(connect(sd, (struct sockaddr*)&server, sizeof(server)) == -1)
                        perror("cant connect\n");

                    // send D-PDU telling content server which file we want
                    spdu.type = 'D';
                    strcpy(spdu.data, cname);
                    write(sd, &spdu, sizeof(struct pdu));

                    // create file locally
                    int contentFile = open(cname, O_RDWR | O_CREAT | O_TRUNC, 0777);

                    // receive C-PDUs until final A-PDU
                    read(sd, (struct pdu*)&rpdu, sizeof(struct pdu));
                    while(rpdu.type != 'A') {
                        
                        write(contentFile, rpdu.data, strlen(rpdu.data));
                        read(sd, (struct pdu*)&rpdu, sizeof(struct pdu));
                    }

                    // --------- AUTO-REGISTER AFTER DOWNLOAD -----------
                    // After downloading, this peer becomes another content server
                    registerContent(s, pname, cname);
                }
                else if (rpdu.type == 'E') {
                    printf("%s\n", rpdu.data);
                }

                break;


// ====================================================================
// ======================== 3) LIST ALL CONTENT =======================
// ====================================================================
            case 3:
                spdu.type = 'O';
                spdu.data[0] = '\0';
                write(s, &spdu, sizeof(struct pdu));

                read(s, (struct pdu*)&rpdu, sizeof(struct pdu));
                if (rpdu.type != 'O') {
                    printf("Error\n");
                    break;
                }

                listSize = atoi(rpdu.data);

                for (i = 0; i < listSize; i++) {
                    read(s, (struct pdu*)&rpdu, sizeof(struct pdu));
                    printf("%s\n", rpdu.data);
                }
                break;


// ====================================================================
// ===================== 4) CONTENT DEREGISTRATION ====================
// ====================================================================
            case 4:
                printf("Name:\n");
                n = read(0, pname, 10);
                pname[n - 1] = '\0';

                printf("Content Name:\n");
                n = read(0, cname, 10);
                cname[n - 1] = '\0';

                // check if we actually host this
                search = searchName(pname) & searchContent(cname);
                if (!search) {
                    printf("Content not registered under this user\n");
                    break;
                }

                // build T-type PDU
                spdu.type = 'T';
                strcpy(spdu.data, pname);
                strcpy(spdu.data+10, cname);
                write(s, &spdu, sizeof(struct pdu));

                // reply from index
                read(s, (struct pdu*)&rpdu, sizeof(struct pdu));

                if (rpdu.type == 'A') {
                    index = getIndex(search);
                    memmove(registeredContent+index,
                            registeredContent+index+1,
                            (--regSize - index) *sizeof(struct registered));
                }
                else if (rpdu.type == 'E') {
                    printf("%s\n", rpdu.data);
                }

                break;


// ====================================================================
// ============================ 5) QUIT ================================
// ====================================================================
            case 5:
                // send T-PDU for every file we host
                spdu.type = 'T';
                for (i = 0; i < regSize; i++) {
                    strcpy(spdu.data, registeredContent[i].name);
                    strcpy(spdu.data+10, registeredContent[i].contentName);
                    write(s, &spdu, sizeof(struct pdu));
                }
                exit(0);

            default:
                printf("Invalid input\n");
        }
    }

    exit(0);
}

// ====================================================================
// ========================= HELPER FUNCTIONS ==========================
// ====================================================================
uint32_t searchName(char* name){
    int i;
    uint32_t sum = 0;

    for (i = 0; i < regSize; i++){
        if (!strcmp(name, registeredContent[i].name)){
            sum += 1 << i;
        }
    }
    return sum;
}

uint32_t searchContent(char* content){
    int i;
    uint32_t sum = 0;

    for (i = 0; i < regSize; i++){
        if (!strcmp(content, registeredContent[i].contentName)){
            sum += 1 << i;
        }
    }
    return sum;
}

int getIndex(uint32_t bitArray){
    int count = 0;

    while(bitArray != 1){
        bitArray /= 2;
        count++;
    }
    return count;
}

// ====================================================================
// ========================= REGISTER CONTENT ==========================
// ====================================================================
void registerContent(int s, char *pname, char *cname){
    int i;

    int alen;
    struct sockaddr_in server;
    struct hostent *hp;

    struct pdu spdu, rpdu;

    // try opening the file we want to register
    int contentFile = open(cname, O_RDONLY);
    if (contentFile == -1){
        printf("File does not exist\n");
        return;
    }

    // TCP server socket to serve this file later
    int sd = socket(AF_INET, SOCK_STREAM, 0);

    bzero((char*)&server, sizeof(struct sockaddr_in));
    server.sin_family = AF_INET;
    server.sin_port = htons(0);   // port 0 → OS picks free port
    server.sin_addr.s_addr = htonl(INADDR_ANY);

    if(bind(sd, (struct sockaddr*)&server, sizeof(server)) == -1){
        fprintf(stderr, "Can't bind name to socket");
    }

    // get OS-assigned port
    alen = sizeof(struct sockaddr_in);
    getsockname(sd, (struct sockaddr*) &server, &alen);

    // -------- BUILD R-PDU --------
    spdu.type = 'R';
    strcpy(spdu.data, pname);
    strcpy(spdu.data+10, cname);
    sprintf(spdu.data+20, "%d", htons(server.sin_port));

    // send registration request to index
    write(s, &spdu, sizeof(struct pdu));

    // wait for index reply
    read(s, (struct pdu*)&rpdu, sizeof(struct pdu));

    if (rpdu.type == 'A') {

        // store locally
        struct registered reg;
        strcpy(reg.name, pname);
        strcpy(reg.contentName, cname);
        registeredContent[regSize] = reg;
        regSize++;

        // --------- FORK A CONTENT SERVER ----------
        switch (fork()){
            case 0:
                listen(sd, 5);
                (void) signal(SIGCHLD, SIG_IGN);

                struct sockaddr_in client;
                int new_sd;
                int client_length = sizeof(client);

                while(1){
                    new_sd = accept(sd, (struct sockaddr*)&client, &client_length);

                    switch(fork()){
                        case 0:
                            close(sd);

                            read(new_sd, &rpdu, sizeof(struct pdu));

                            // C-PDUs sent here
                            spdu.type = 'C';
                            bzero((char*)&spdu.data, sizeof(spdu.data));

                            while((i = read(contentFile, spdu.data, 100)) > 0){
                                write(new_sd, &spdu, sizeof(struct pdu));
                                bzero((char*)&spdu.data, sizeof(spdu.data));
                            }

                            spdu.type = 'A';
                            write(new_sd, &spdu, sizeof(struct pdu));

                            close(new_sd);
                            exit(0);

                        default:
                            close(new_sd);
                    }
                }
                break;

            default:
                printf("Listening server created\n");
                break;

            case -1:
                fprintf(stderr, "fork error\n");
        }
    }
    else if (rpdu.type == 'E'){
        printf("%s\n", rpdu.data);
    }
}
