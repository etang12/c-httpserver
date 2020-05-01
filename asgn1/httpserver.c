#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdio.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <fcntl.h>
#include <unistd.h> // write
#include <string.h> // memset
#include <stdlib.h> // atoi
#include <stdbool.h> // true, false
#include <inttypes.h>
#include <err.h>

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
    char* body; //contents of body
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
    //printf("%s", message->buffer);
    char* body_check = strstr((char*)&message->buffer, "\r\n\r\n");
    char* token = strtok((char*)&message->buffer, "\r\n");
    //printf("this is first token: %s\n", token);
    sscanf(token, "%s %s %s", message->method, message->filename, message->httpversion);
    memmove(message->filename, (message->filename)+1, strlen(message->filename));
    //printf("%s %s %s\n", message->method, message->filename, message->httpversion);
    
    //printf("0x%" PRIXPTR "\n", (uintptr_t) ((body_in_header)));
    //printf("this is strstr len: %d\n", strlen(body_check);
    if(strlen(body_check) >= 4){        //check if request contains header and body (bad request)
        //uint8_t responseBuffer[BUFFER_SIZE];
        message->status_code = 400;
        //printf("no body in header, good request\n");
        //send(client_sockd, "HELLO CLIENT\n" , sizeof("HELLO CLIENT"), 0);
        //send response 400 bad request and close socket
    }
        
    if(strlen(token) > BUFFER_SIZE){
        //printf("need to throw error where header length > 4096 bytes");
        message->status_code = 400;
        dprintf(client_sockd, "%s %d Bad Request\n", message->httpversion, message->status_code);
        close(client_sockd);
        exit(EXIT_FAILURE);
    }
    while(token != NULL){
        //printf("token1: %s\n", token);
        if(strstr(token, "Content-Length") != NULL){
            //printf("token2: %s\n", token);
            sscanf(token, "%*s %zu", &(message->content_length));
            break;
        }else if(strcmp(message->method, "GET") == 0 || strcmp(message->method, "HEAD") == 0){
            //printf("method name : %s\n", message->method);
            message->content_length = 0; //place holder for content-length when GET or HEAD command
            //printf("inside TOKEN LOOP, GET or HEAD request\n");
            break;
        }
        token = strtok(NULL, "\r\n");
    }
    //printf("%s %s %s\n Content-Length: %zu\n", message->method, message->filename, message->httpversion, message->content_length);
    /*if(message->content_length > BODY_BUFFER_SIZE){     //checks if content length is larger than 32 KiB, is so throw error
        printf("400 Bad Request, body exceeds 32KiB\n");
        message->status_code = 400;
        dprintf(client_sockd, "%s %d Bad Request\nContent-Length: %zd\n", message->httpversion, message->status_code, message->content_length);
        close(client_sockd);
        exit(EXIT_FAILURE);
    }*/
    //printf("outside of loop: %s\n", token);
    
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
//function created to check if a file exists to determine status code (200 or 201 during a PUT request)
bool file_exists (char *filename) {
  struct stat statbuf;   
  return (stat (filename, &statbuf) == 0);
}

void process_request(ssize_t client_sockd, struct httpObject* message) {
    //printf("%" PRIu8 "\n", message->buffer[0]);
    //printf("process recv: %zd\n", bytes);
    //printf("inside process_request\n");
    uint8_t body_buffer[BODY_BUFFER_SIZE];
    if(strcmp(message->method, "PUT") == 0){
        if(file_exists(message->filename)){
            message->status_code = 200;
            //printf("file exists\n");
        } else {
            message->status_code = 201;
            //printf("file doesn't exist\n");
        }
        ssize_t putfd = open(message->filename, O_CREAT|O_WRONLY|O_TRUNC, 0644);       //creates a new file if it doesn't exist, overwrites it if it does
        if(putfd < 0){
            warn("%s", message->filename);
            exit(EXIT_FAILURE);
        }
        ssize_t bytes_recv = recv(client_sockd, body_buffer, BODY_BUFFER_SIZE, 0);
        //printf("first bytes_recv: %zu\n", bytes_recv);
        if(bytes_recv == 0){
            write(putfd, body_buffer, bytes_recv);
            //printf("inside == 0\n");
            dprintf(client_sockd, "%s %d OK\r\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, 0);
            close(putfd);
            //printf("end of this\n");
            return;
        } else if(bytes_recv <= BODY_BUFFER_SIZE){       //check if bytes_recv is less than buffer size
            int temp_bytes_recv = bytes_recv;            //temp variable created to keep track of bytes recv after different iterations of loop 
            while(true){
                //printf("in loop\n");
                write(putfd, body_buffer, bytes_recv);         //writes contents of body from request into the created file which is saved in the server
                if(bytes_recv == message->content_length){
                    //printf("breaking out loop\n");
                    break;
                }
                if(temp_bytes_recv == message->content_length - bytes_recv){       //temp bytes == different iteration loop recv value, 
                    //printf("bytes_recv subtraction: %zd\n", bytes_recv);
                    //printf("break loop\n");
                    break;
                }
                bytes_recv = recv(client_sockd, body_buffer, BODY_BUFFER_SIZE, 0);
                //printf("count: %d bytes_recv: %zu\n", bytes_recv);
            }
            if(message->status_code == 200){
                dprintf(client_sockd, "%s %d OK\r\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, 0);
            } else if(message->status_code == 201){
                dprintf(client_sockd, "%s %d Created\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, 0);
            }
            close(putfd);
            return;
        } 
    }
    else if(strcmp(message->method, "GET") == 0){
        //printf("%s\n", message->filename);
        ssize_t getfd = open(message->filename, O_RDONLY);
        //printf("getfd: %d\n", getfd);
        if(getfd < 0){                                  //if file doesn't exist, then 404 request (Not Found)
            message->status_code = 404;
            dprintf(client_sockd, "%s %d Not Found\r\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, 0);
            //printf("error opening file in GET\n");
            return;
        }
        //printf("opened in GET successfully, FD = %zu\n", getfd);
        ssize_t readfd = read(getfd, body_buffer, BODY_BUFFER_SIZE);
        if(readfd < 0){
            printf("error reading file in GET\n");
            return;
        }
        message->status_code = 200;

        dprintf(client_sockd, "%s %d OK\r\nContent-Length: %zd\r\n\r\n", message->httpversion, message->status_code, readfd);
        send(client_sockd, body_buffer, readfd, 0);
        close(getfd);
    }
    else if(strcmp(message->method, "HEAD") == 0){
    ssize_t headfd = open(message->filename, O_RDONLY);
        //printf("getfd: %d\n", getfd);
        if(headfd < 0){                                  //if file doesn't exist, then 404 request (Not Found)
            message->status_code = 404;
            dprintf(client_sockd, "%s %d Not Found\r\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, 0);
            //printf("error opening file in GET\n");
            return;
        }
        //printf("opened in GET successfully, FD = %zu\n", getfd);
        ssize_t readfd = read(headfd, body_buffer, BODY_BUFFER_SIZE);
        if(readfd < 0){
            printf("error reading file in GET\n");
            return;
        }
        message->content_length = readfd;
        message->status_code = 200;
        dprintf(client_sockd, "%s %d OK\r\nContent-Length: %zd\r\n\r\n", message->httpversion, message->status_code, message->content_length);
        close(headfd);
    }
    return;
}

/*
    \brief 3. Construct some response based on the HTTP request you recieved
*/
void construct_http_response(ssize_t client_sockd, struct httpObject* message) {
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
        //printf("hello");

        /*
         * 4. Construct Response
         */
        construct_http_response(client_sockd, &message);

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
