#include <sys/socket.h>
#include <sys/stat.h>
#include <stdio.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <fcntl.h>
#include <unistd.h> // write
#include <string.h> // memset
#include <stdlib.h> // atoi
#include <stdbool.h> // true, false
#include <inttypes.h>

#define BUFFER_SIZE 4096
#define BODY_BUFFER_SIZE 32768
#define METHOD_MAX_SIZE 5
#define FILENAME_MAX_SIZE 28
#define HTTPSIZE 9

struct httpObject {
    /*
        Create some object 'struct' to keep track of all
        the components related to a HTTP message
        NOTE: There may be more member variables you would want to add
    */
    char method[METHOD_MAX_SIZE];         // PUT, HEAD, GET
    char filename[FILENAME_MAX_SIZE];      // what is the file we are worried about
    char httpversion[HTTPSIZE];    // HTTP/1.1
    ssize_t content_length; // example: 13
    int status_code;
    uint8_t buffer[BUFFER_SIZE];
};

/*
    \brief 1. Want to read in the HTTP message/ data coming in from socket
    \param client_sockd - socket file descriptor
    \param message - object we want to 'fill in' as we read in the HTTP message
    \parses the HTTP message to grab method, filename, and content_length
*/
void read_http_request(ssize_t client_sockd, struct httpObject* message) {
    //printf("Client_sockd = %zu\n", client_sockd);
    /*
     *
     * Must check for valid ASCII characters, length of method <= 28, 
     * Must check if buffer includes header and body or just header (\r\n\r\n)
     * Must check if header length is <= 4 KiB, body can be any length
     */
    recv(client_sockd, message->buffer, sizeof(message->buffer), 0);
    char* token = strtok((char*)message->buffer, "\r\n");
    sscanf(token, "%s %s %s", message->method, message->filename, message->httpversion);
    //printf("%s %s %s\n", message->method, message->filename, message->httpversion);
    if(strlen(token) > BUFFER_SIZE){
        printf("need to throw error where header length > 4096 bytes");
        return;
    }
    //char* file_name = strtok(&(message->filename), "/");
    //printf("file_name = %s\n", file_name);
    //printf("token = %s\n", token);
    //printf("token len = %lu\n", strlen(token));
    //ssize_t cont_len = 0;
    while(token != NULL){
        //printf("%s", token);
        if(strstr(token, "Content-Length") != NULL){
            //printf("%s\n", token);
            break;
        }
        token = strtok(NULL, "\r\n");
    }
    //printf("outside of loop: %s\n", token);
    sscanf(token, "%*s %zu", &(message->content_length));
    //printf("%s %s %s\n Content-Length: %zu\n", message->method, message->filename, message->httpversion, message->content_length);

    /*
     * Start constructing HTTP request based off data from socket
     */
    //Right now, working on parsing method, filename, httpversion

    return;
}

/*
    \brief 2. Want to process the message we just received
*/
void process_request(ssize_t client_sockd, struct httpObject* message) {
    //printf("%" PRIu8 "\n", message->buffer[0]);
    //printf("process recv: %zd\n", bytes);
    uint8_t body_buffer[BODY_BUFFER_SIZE];
    if(strcmp(message->method, "PUT") == 0){
        memmove(message->filename, (message->filename)+1, strlen(message->filename));   //removes '/' from beginning of filename
        ssize_t bytes_recv = recv(client_sockd, body_buffer, sizeof(body_buffer), 0);
        ssize_t filefd = open(message->filename, O_CREAT|O_WRONLY|O_TRUNC, 0644);       //creates a new file if it doesn't exist, overwrites it if it does
        write(filefd, body_buffer, bytes_recv);                                     //writes contents of body from request into the created file which is saved in the server
    }
    else if(strcmp(message->method, "GET") == 0){
        
    }
    return;
}

/*
    \brief 3. Construct some response based on the HTTP request you recieved
*/
void construct_http_response() {
   //printf("Constructing Response\n");

    return;
}


int main(int argc, char** argv) {
    /*
        Create sockaddr_in with server information
    */
    char* port = "8080";
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(atoi(port));
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    socklen_t addrlen = sizeof(server_addr);

    /*
        Create server socket
    */
    int server_sockd = socket(AF_INET, SOCK_STREAM, 0);

    // Need to check if server_sockd < 0, meaning an error
    if (server_sockd < 0) {
        perror("socket");
    }

    /*
        Configure server socket
    */
    int enable = 1;

    /*
        This allows you to avoid: 'Bind: Address Already in Use' error
    */
    int ret = setsockopt(server_sockd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

    /*
        Bind server address to socket that is open
    */
    ret = bind(server_sockd, (struct sockaddr *) &server_addr, addrlen);

    /*
        Listen for incoming connections
    */
    ret = listen(server_sockd, 5); // 5 should be enough, if not use SOMAXCONN

    if (ret < 0) {
        return EXIT_FAILURE;
    }

    /*
        Connecting with a client
    */
    struct sockaddr client_addr;
    socklen_t client_addrlen;

    struct httpObject message;

    while (true) {
        printf("[+] server is waiting...\n");

        /*
         * 1. Accept Connection
         */
        int client_sockd = accept(server_sockd, &client_addr, &client_addrlen);
        // Remember errors happen

        /*
         * 2. Read HTTP Message
         */
        read_http_request(client_sockd, &message);

        /*
         * 3. Process Request
         */
        process_request(client_sockd, &message);

        /*
         * 4. Construct Response
         */
        construct_http_response();

        /*
         * 5. Send Response
         */
        //printf("Response Sent\n");

        /*
         Sample Example which wrote to STDOUT once.
         
        uint8_t buff[BUFFER_SIZE + 1];
        ssize_t bytes = recv(client_sockd, buff, BUFFER_SIZE, 0);
        buff[bytes] = 0; // null terminate
        printf("[+] received %ld bytes from client\n[+] response: \n", bytes);
        write(STDOUT_FILENO, buff, bytes);
        */
       close(client_sockd);
    }

    return EXIT_SUCCESS;
}
