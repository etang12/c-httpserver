#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdio.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h> 
#include <stdlib.h> 
#include <stdbool.h> 
#include <inttypes.h>
#include <err.h>
#include <errno.h>
#include <pthread.h>
#include <ctype.h>

#define BUFFER_SIZE 4096
#define BODY_BUFFER_SIZE 10000
#define METHOD_MAX_SIZE 5
#define FILENAME_MAX_SIZE 29
#define HTTPSIZE 9

int entries = 0;
pthread_cond_t dispatcher_cond = PTHREAD_COND_INITIALIZER;
pthread_cond_t worker_cond = PTHREAD_COND_INITIALIZER;

typedef struct httpObject {
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
    int body_tracker;       //flag to check if parts of body passed alongside header
    char* body_string;      //string that contains parts of body passed alongside header
    uint8_t buffer[BUFFER_SIZE];

} httpObject;

typedef struct circular_buff_t{
    int number_threads;
    int thread_id;
    int* clientfd_queue;
    int q_size;
    int max_size;
    int head;
    int tail;
    pthread_mutex_t *mut;
    httpObject msg;
} circleBuffer;

typedef struct threadarg_t{
    struct httpObject msg;
    circleBuffer *cb;
} threadArg;

int cb_dequeue(circleBuffer *cb){
    int client_fd;
    //printf("process: %d\n", pindex);
    while(cb->q_size == 0){
        pthread_cond_wait(&worker_cond, cb->mut);
    }
    client_fd = cb->clientfd_queue[cb->head];
    
    if(cb->head == cb->tail){
        cb->head = -1;
        cb->tail = -1;
    } else {
        cb->head = (cb->head + 1) % cb->max_size;
    }
    cb->q_size--;
    printf("dequeued: %d\n", client_fd);
    return client_fd;
}

void cb_enqueue(circleBuffer *cb, int client){
    //handle case if cb->pqsize is full
    while(cb->q_size == cb->max_size){
        pthread_cond_wait(&dispatcher_cond, cb->mut);
    }
    if(cb->head == -1){
        cb->head = 0;
    }
    cb->tail = (cb->tail + 1) % cb->max_size;
    cb->clientfd_queue[cb->tail] = client;
    printf("enqueued: %d\n", cb->clientfd_queue[cb->tail]);
    cb->q_size++;
}



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
    
    ssize_t total_bytes_recv = 0;
    //ssize_t bytes_recv = recv(client_sockd, message->buffer, sizeof(message->buffer), 0);
    
    while(total_bytes_recv < BUFFER_SIZE){
        ssize_t ret_bytes = recv(client_sockd, message->buffer + total_bytes_recv, BUFFER_SIZE - total_bytes_recv, 0);
        message->buffer[ret_bytes] = '\0';
        char* body_check = strstr((char*)message->buffer, "\r\n\r\n");
        //printf("%s\n", message->buffer);
        //printf("strlen: %lu\n", strlen(body_check));
        if(ret_bytes < 0){
            message->status_code = 500;
            dprintf(client_sockd, "%s %d Internal Server Error\r\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, 0);
            return;
        }
        else if (body_check != NULL) {
            //printf("%s\n", body_check);
            if(strlen(body_check) != 4){
                //printf("body check: %lu\n", strlen(body_check));
                message->body_string = (body_check + 4*sizeof(char));
                message->body_tracker = 1;
            }
            break;
        } else {
            total_bytes_recv += ret_bytes;
        }
    }
    char* ptr;
    char* token = strtok_r((char*)&message->buffer, "\r\n", &ptr);
    //printf("this is first token: %s\n", token);
    sscanf(token, "%s %s %s", message->method, message->filename, message->httpversion);

    if(strlen(message->filename) > 28){
        //printf("filename: %s\n", message->filename);
        message->status_code = 400;
        dprintf(client_sockd, "%s %d Bad Request\r\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, 0);
        return;
    }
    unsigned long len = strspn(message->filename ,"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_/");
    //printf("len: %d\n", len);
    if(strlen(message->filename) != len){       //check for valid chars in resource name
        message->status_code = 400;
        //printf("bad ascii char\n");
        dprintf(client_sockd, "%s %d Bad Request\r\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, 0);
        return;
    }
    if(strcmp(message->httpversion,"HTTP/1.1") != 0){
        message->status_code = 400;
        //printf("bad http char\n");
        dprintf(client_sockd, "%s %d Bad Request\r\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, 0);
        return;
    }
    if(strcmp(message->method, "PUT") != 0 && strcmp(message->method, "GET") != 0 && strcmp(message->method, "HEAD") != 0){
        printf("we in here big bruh\n");
        message->status_code = 400;
        dprintf(client_sockd, "%s %d Bad Request\r\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, 0);
        return;
    }
    memmove(message->filename, (message->filename)+1, strlen(message->filename));
        
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
        token = strtok_r(NULL, "\r\n", &ptr);
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
    int file_size = get_file_size(message->filename);
    int file_reg = is_regular_file(message->filename);
    if(strcmp(message->method, "PUT") == 0){
        printf("in PUT: %s\n", message->filename);
        if(file_exists == 0){
            struct stat statbuf;
            stat(message->filename, &statbuf);
            if((statbuf.st_mode & S_IWUSR) == 0){       //file not writeable
                message->status_code = 403;
                dprintf(client_sockd, "%s %d Forbidden\r\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, 0);
                return;
            }
            message->status_code = 200;
        } else {
            message->status_code = 201;
        }
        ssize_t putfd = open(message->filename, O_CREAT|O_WRONLY|O_TRUNC, 0644);       //creates a new file if it doesn't exist, overwrites it if it does
        printf("open file: %s\n", message->filename);
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
            return;
        }

        if(message->body_tracker == 1){
            write(putfd, message->body_string, strlen(message->body_string));
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
            memset(body_buffer, '\0', BODY_BUFFER_SIZE);
            printf("done: %s\n", message->filename);
            return;
        } 
    }
    //HANDLE GET REQUEST
    else if(strcmp(message->method, "GET") == 0){
        printf("in GET: %s\n", message->filename);
        printf("in GET client sockd: %zd\n", client_sockd);
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
        //printf("%s\n", message->filename);
        //printf("content-length: %zd\n", message->content_length);
        ssize_t getfd = open(message->filename, O_RDONLY);
        if(getfd < 0){                                  
            message->status_code = 404;
            if((statbuf.st_mode & S_IRUSR) == 0){       //file not readable
                message->status_code = 403;
                dprintf(client_sockd, "%s %d Forbidden\r\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, 0);
                return;
            } 
        }
        ssize_t readfd = read(getfd, body_buffer, BODY_BUFFER_SIZE);
        if(readfd < 0){
            printf("in read\n");
            message->status_code = 500;
            dprintf(client_sockd, "%s %d Internal Server Error\r\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, 0);
            return;
        }
        message->content_length = file_size;
        message->status_code = 200;
        dprintf(client_sockd, "%s %d OK\r\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, file_size);
        int cont_len = file_size;     
        while(true){
            cont_len = cont_len - readfd;                      //decrement cont_len by number of bytes recv after each iteration to handle any sized files
            ssize_t bytes_written = write(client_sockd, body_buffer, readfd);         //writes contents of body from request into the created file which is saved in the server
            if(bytes_written < 0){
                printf("in write\n");
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
        memset(body_buffer, '\0', BODY_BUFFER_SIZE);
        return;
    }
    //HANDLE HEAD REQUEST
    else if(strcmp(message->method, "HEAD") == 0){
        if(file_exists != 0){
            message->status_code = 404;
            dprintf(client_sockd, "%s %d Not Found\r\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, 0);
            return;
        }
        struct stat statbuf;
        stat(message->filename, &statbuf);
        if((statbuf.st_mode & S_IRUSR) == 0){       //file not readable
            message->status_code = 403;
            dprintf(client_sockd, "%s %d Forbidden\r\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, 0);
            return;
        }
        ssize_t headfd = open(message->filename, O_RDONLY);
            //printf("getfd: %d\n", getfd);
            if(headfd < 0){                                  //if file doesn't exist, then 404 request (Not Found)
                if((statbuf.st_mode & S_IRUSR) == 0){       //file not readable
                    message->status_code = 403;
                    dprintf(client_sockd, "%s %d Forbidden\r\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, 0);
                    return;
                }
            }
        ssize_t readfd = read(headfd, body_buffer, BODY_BUFFER_SIZE);
        if(readfd < 0){
            message->status_code = 500;
            dprintf(client_sockd, "%s %d Internal Server Error\r\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, 0);
        }
        message->content_length = file_size;
        message->status_code = 200;
        dprintf(client_sockd, "%s %d OK\r\nContent-Length: %zd\r\n\r\n", message->httpversion, message->status_code, message->content_length);
        close(headfd);
    }
    memset(body_buffer, '\0', BODY_BUFFER_SIZE);
    return;
}

void* thread_func(void* arg){   //dequeue from buffer
    threadArg *parg = (threadArg*) arg;
    //httpObject *pmsg = &parg->cb->msg;
        while(true){
            printf("--------------------------------------------\n");
            pthread_mutex_lock(parg->cb->mut);
            int c_fd = cb_dequeue(parg->cb);
            pthread_mutex_unlock(parg->cb->mut);
            pthread_cond_signal(&dispatcher_cond);
            read_http_request(c_fd, &parg->msg);
            //printf("c_fd: %d\n", c_fd);
            process_request(c_fd, &parg->msg);
            printf("method: %s\n", parg->msg.method);
            printf("filename: /%s\n", parg->msg.filename);
            printf("length: %zd\n", parg->msg.content_length);
            memset(&parg->msg.buffer, '\0', BUFFER_SIZE);
            memset(&parg->msg.method, '\0', METHOD_MAX_SIZE);
            memset(&parg->msg.filename, '\0', FILENAME_MAX_SIZE);
            memset(&parg->msg.httpversion, '\0', HTTPSIZE);
            //printf("fd: %d\n", c_fd);
            close(c_fd);
        }
    return NULL;
}

int main(int argc, char** argv) {
    char* port;
    char* threads = NULL;
    char* log_name = NULL;
    int NUM_THREADS = 0;
    int c;
    int i = 0;
    opterr = 0;
    while((c = getopt(argc, argv, "N:l:")) != -1){
        switch(c){
            case 'N':
                threads = optarg;
                NUM_THREADS = atoi(threads);
                break;
            case 'l':
                log_name = optarg;
                break;
            case '?':
                if(optopt == 'c'){
                    fprintf(stderr, "Option -%c requires an argument.\n", optopt);
                } else if (isprint(optopt)){
                    fprintf(stderr, "Unknown option -%c.\n", optopt);
                } else {
                    fprintf(stderr, "Unknown option character \\x%x'.\n", optopt);
                }
                return EXIT_FAILURE;
        }
    }
    if(NUM_THREADS == 0){
        NUM_THREADS = 4;
    }
    /*SETTING UP THE THREAD POOL, CIRCULAR BUFFER (QUEUE OF CLIENT FD'S), DISPATCHER THREAD */
    //printf("num_threads: %d\n", num_threads);
    //printf("log_name: %s\n", log_name);
    port = argv[optind];
    //printf("port: %s\n", port);
    pthread_t tid[NUM_THREADS];
    threadArg args[NUM_THREADS];
    int c_queue[NUM_THREADS * 2];
    circleBuffer circleBuff;
    struct httpObject message;
    pthread_mutex_t(mutex) = PTHREAD_MUTEX_INITIALIZER;
    circleBuff.number_threads = NUM_THREADS;
    circleBuff.clientfd_queue = c_queue;
    circleBuff.q_size = 0;
    circleBuff.max_size = NUM_THREADS * 2;
    circleBuff.head = -1;
    circleBuff.tail = -1;
    circleBuff.mut = &mutex;
    
    while(i < NUM_THREADS){
        args[i].cb = &circleBuff;
        args[i].msg = message;
        pthread_create(&tid[i], NULL, thread_func, &args[i]);
        i++;
        //printf("thread: %d\n", i);
    }

    /*
        Create sockaddr_in with server information
    */
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



    //dispatcher thread
    while (true) {
        //printf("[+] server is waiting...\n");

        /*
         * 1. Accept Connection
         */
        int client_sockd = accept(server_sockd, &client_addr, &client_addrlen);
        pthread_mutex_lock(circleBuff.mut);
        cb_enqueue(&circleBuff, client_sockd);
        pthread_mutex_unlock(circleBuff.mut);
        pthread_cond_signal(&worker_cond);
        /*for(int x = 0; x < circleBuff.pq_size; x++){
            printf("full buffer: %d\n", circleBuff.clientfd_queue[x]);
        }*/
        
        // Remember errors happen
        //place client_sockd into a "queue" to be used by worker threads
        /*
         * 2. Read HTTP Message
         */
        //read_http_request(client_sockd, &message);
        //process_request(client_sockd, &message);
        //clear statically allocated memory (reset it)


        //close(client_sockd);
    }

    return EXIT_SUCCESS;
}
