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
#include <errno.h>

#define BUFFER_SIZE 4096
#define BODY_BUFFER_SIZE 32768
#define METHOD_MAX_SIZE 5
#define FILENAME_MAX_SIZE 29
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
    //printf("%s", message->buffer);
    //char* body_check = strstr((char*)&message->buffer, "\r\n\r\n");
    //printf("%s\n", body_check);
    char* token = strtok((char*)&message->buffer, "\r\n");
    //printf("this is first token: %s\n", token);
    sscanf(token, "%s %s %s", message->method, message->filename, message->httpversion);
    //printf("http version: %s\n", message->httpversion);
    //printf("filename: %s\n", message->filename);
    //printf("%s %s\n", message->filename, message->httpversion);
    //printf("token: %s\n", token);
    if(strlen(message->filename) > 28){
        //printf("filename: %s\n", message->filename);
        message->status_code = 400;
        dprintf(client_sockd, "%s %d Bad Request\r\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, 0);
        return;
    }
    int len = strspn(message->filename ,"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_/");
    //printf("len: %d\n", len);
    if(strlen(message->filename) != len){       //check for valid chars in resource name
        message->status_code = 400;
        printf("bad ascii char\n");
        dprintf(client_sockd, "%s %d Bad Request\r\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, 0);
        return;
    }
    if(strcmp(message->httpversion,"HTTP/1.1") != 0){
        message->status_code = 400;
        printf("bad http char\n");
        dprintf(client_sockd, "%s %d Bad Request\r\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, 0);
    }
    memmove(message->filename, (message->filename)+1, strlen(message->filename));
    //printf("%s %s %s\n", message->method, message->filename, message->httpversion);
    
    //printf("0x%" PRIXPTR "\n", (uintptr_t) ((body_in_header)));
    //printf("this is strstr len: %d\n", strlen(body_check);
    /*if(strlen(body_check) > 4){        //check if request contains header and body (bad request)
        printf("strlen: %lu\n", strlen(body_check));
        //uint8_t responseBuffer[BUFFER_SIZE];
        message->status_code = 400;
        //printf("no body in header, good request\n");
        printf("send response 400 bad request and close socket\n");
        dprintf(client_sockd, "%s %d Bad Request\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, 0);
        
    }*/
        
    if(strlen(token) > BUFFER_SIZE){
        message->status_code = 400;
        printf("header larger than 4096\n");
        dprintf(client_sockd, "%s %d Bad Request\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, 0);
        return;
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
int if_exists (char *filename){
  struct stat statbuf;   
  return (stat (filename, &statbuf));
}

int get_file_size (char *filename){
    struct stat statbuf;
    stat(filename, &statbuf);
    return statbuf.st_size;
}

int is_regular_file(char *filename){    //check if object is actually a file, returns 0 if not a file, returns non-negative integer if a file
    struct stat statbuf;
    stat(filename, &statbuf);
    return S_ISREG(statbuf.st_mode);
}

//HANDLE PUT REQUEST
void process_request(ssize_t client_sockd, struct httpObject* message) {
    uint8_t body_buffer[BODY_BUFFER_SIZE];
    int file_exists = if_exists(message->filename);
    //int file_reg = is_regular_file(message->filename);

    if(strcmp(message->method, "PUT") == 0){
        if(file_exists == 0){
            message->status_code = 200;
        } else {
            message->status_code = 201;
        }
        struct stat statbuf;
        stat(message->filename, &statbuf);
        if((statbuf.st_mode & S_IWUSR) == 0){       //file not readable
            message->status_code = 403;
            //printf("in here bro\n");
            dprintf(client_sockd, "%s %d Forbidden\r\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, 0);
            return;
        }
        ssize_t putfd = open(message->filename, O_CREAT|O_WRONLY|O_TRUNC, 0644);       //creates a new file if it doesn't exist, overwrites it if it does
        //printf("putfd: %zd\n", putfd);
        if(putfd < 0){
            if(errno == EACCES){
                message->status_code = 403;
                dprintf(client_sockd, "%s %d Forbidden\r\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, 0);
            }
            return;
        }
        int cont_len = message->content_length; 
        if(cont_len == 0){

            if(message->status_code == 200){
                dprintf(client_sockd, "%s %d OK\r\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, 0);
            } else if(message->status_code == 201){
                dprintf(client_sockd, "%s %d Created\r\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, 0);
            }
        }
        ssize_t bytes_recv = recv(client_sockd, body_buffer, BODY_BUFFER_SIZE, 0);
        //printf("first bytes_recv: %zu\n", bytes_recv);
        if(bytes_recv <= BODY_BUFFER_SIZE){             //check if bytes_recv is less than buffer size    
            while(true){
                cont_len = cont_len - bytes_recv;       //decrement cont_len by number of bytes recv after each iteration to handle any sized files
                //printf("in loop\n");
                ssize_t bytes_written = write(putfd, body_buffer, bytes_recv);         //writes contents of body from request into the created file which is saved in the server
                if(bytes_written < 0){
                    message->status_code = 500;
                    dprintf(client_sockd, "%s %d Internal Server Error\r\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, 0);
                }
                if(cont_len == 0){
                    break;
                }
                bytes_recv = recv(client_sockd, body_buffer, BODY_BUFFER_SIZE, 0);
            }
            if(message->status_code == 200){
                dprintf(client_sockd, "%s %d OK\r\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, 0);
            } else if(message->status_code == 201){
                dprintf(client_sockd, "%s %d Created\r\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, 0);
            }
            close(putfd);
            return;
        } 
    }
    //HANDLE GET REQUEST
    else if(strcmp(message->method, "GET") == 0){
        int file_size = get_file_size(message->filename);
        int file_reg = is_regular_file(message->filename);
        int file_exists = if_exists(message->filename);
        if(file_exists != 0){
            message->status_code = 404;
            dprintf(client_sockd, "%s %d Not Found\r\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, 0);
            return;
        }
        //printf("%d\n",file_reg);
        if(file_reg == 0){      
            message->status_code = 403;
            dprintf(client_sockd, "%s %d Forbidden\r\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, 0);
            return;
        }
        struct stat statbuf;
        stat(message->filename, &statbuf);
        if((statbuf.st_mode & S_IRUSR) == 0){       //file not readable
            message->status_code = 403;
            dprintf(client_sockd, "%s %d Forbidden\r\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, 0);
            return;
        }
        //printf("%s\n", message->filename);
        //printf("content-length: %zd\n", message->content_length);
        ssize_t getfd = open(message->filename, O_RDONLY);
        //printf("getfd: %d\n", getfd);
        if(getfd < 0){                                  //if file doesn't exist, then 404 request (Not Found)
            message->status_code = 404;
            if(errno == EACCES){
                message->status_code = 403;
                dprintf(client_sockd, "%s %d Forbidden\r\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, 0);
            } else {
                dprintf(client_sockd, "%s %d Not Found\r\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, 0);
            }
            return;
        }
        ssize_t readfd = read(getfd, body_buffer, BODY_BUFFER_SIZE);
        if(readfd < 0){
            message->status_code = 500;
            dprintf(client_sockd, "%s %d Internal Server Error\r\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, 0);
            return;
        }
        message->status_code = 200;
        dprintf(client_sockd, "%s %d OK\r\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, file_size);
        int cont_len = file_size;     
        while(true){
            cont_len = cont_len - readfd;                      //decrement cont_len by number of bytes recv after each iteration to handle any sized files
            ssize_t bytes_written = write(client_sockd, body_buffer, readfd);         //writes contents of body from request into the created file which is saved in the server
            if(bytes_written < 0){
                message->status_code = 500;
                dprintf(client_sockd, "%s %d Internal Server Error\r\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, 0);
            }

            if(cont_len == 0){
                break;
            }
            readfd = read(getfd, body_buffer, BODY_BUFFER_SIZE);
            //printf("second bytes_recv: %zu\n", bytes_recv);
        }
        close(getfd);
    }
    //HANDLE HEAD REQUEST
    else if(strcmp(message->method, "HEAD") == 0){
    int file_size = get_file_size(message->filename);
    int file_exists = if_exists(message->filename);
    if(file_exists != 0){
        message->status_code = 404;
        dprintf(client_sockd, "%s %d Not Found\r\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, 0);
        return;
    }
    struct stat statbuf;
    stat(message->filename, &statbuf);
    if((statbuf.st_mode & S_IRUSR) == 0){       //file not readable
        message->status_code = 403;
        //printf("in here bro\n");
        dprintf(client_sockd, "%s %d Forbidden\r\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, 0);
        return;
    }
    ssize_t headfd = open(message->filename, O_RDONLY);
        //printf("getfd: %d\n", getfd);
        if(headfd < 0){                                  //if file doesn't exist, then 404 request (Not Found)
            if(errno == EACCES){
                message->status_code = 403;
                dprintf(client_sockd, "%s %d Forbidden\r\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, 0);
            }
            return;
        }
        ssize_t readfd = read(headfd, body_buffer, BODY_BUFFER_SIZE);
        if(readfd < 0){
            printf("error reading file in GET\n");
            return;
        }
        message->content_length = file_size;
        message->status_code = 200;
        dprintf(client_sockd, "%s %d OK\r\nContent-Length: %zd\r\n\r\n", message->httpversion, message->status_code, message->content_length);
        close(headfd);
    }
    return;
}

int main(int argc, char** argv) {
    /*
        Create sockaddr_in with server information
    */
    char* port = argv[1];
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
    socklen_t client_addrlen = sizeof(client_addr);

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
        process_request(client_sockd, &message);
        //clear statically allocated memory (reset it)
        memset(message.buffer, '\0', BUFFER_SIZE);
        memset(message.filename, '\0', BUFFER_SIZE);
        memset(message.method, '\0', BUFFER_SIZE);
        memset(message.httpversion, '\0', BUFFER_SIZE);

        close(client_sockd);
    }

    return EXIT_SUCCESS;
}
