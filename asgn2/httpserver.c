#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdio.h>
#include <string.h> 
#include <stdlib.h> 
#include <netinet/in.h>
#include <netinet/ip.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdbool.h> 
#include <inttypes.h>
#include <err.h>
#include <errno.h>
#include <pthread.h>
#include <ctype.h>
#include <math.h>

#define BUFFER_SIZE 4096
#define BODY_BUFFER_SIZE 8000
#define LOG_SIZE 4000
#define METHOD_MAX_SIZE 100
#define FILENAME_MAX_SIZE 256
#define HTTPSIZE 100
#define HEALTH_BODY 1000

int entries = 0;
int errors = 0;
int global_offset = 0;
pthread_cond_t dispatcher_cond = PTHREAD_COND_INITIALIZER;
pthread_cond_t worker_cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t offset_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct httpObject {
    /*
        Create some object 'struct' to keep track of all
        the components related to a HTTP message
        NOTE: There may be more member variables you would want to add
    */
    char method[METHOD_MAX_SIZE];         // PUT, HEAD, GET
    char filename[FILENAME_MAX_SIZE];      // what is the file we are worried about
    char httpversion[HTTPSIZE];    // HTTP/1.1
    size_t content_length; // example: 13
    int status_code;
    int body_tracker;       //flag to check if parts of body passed alongside header
    char* body_string;      //string that contains parts of body passed alongside header
    uint8_t buffer[BUFFER_SIZE];
    char copyBuffer[FILENAME_MAX_SIZE];
    int logfd;
    int healthcheck_size;

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
    int logfd;
} threadArg;

int cb_dequeue(circleBuffer *cb){
    int client_fd;
    //printf("process: %d\n", pindex);
    while(cb->q_size == 0){
        pthread_cond_wait(&worker_cond, cb->mut);
    }
    client_fd = cb->clientfd_queue[cb->head];
    cb->clientfd_queue[cb->head] = -1;
    if(cb->head == cb->tail){
        cb->head = -1;
        cb->tail = -1;
    } else if(cb->head == cb->max_size - 1){
        cb->head = 0;
    } else{
        cb->head++;
    }
    cb->q_size--;
    //printf("dequeued: %d\n", client_fd);
    return client_fd;
}

void cb_enqueue(circleBuffer *cb, int client){
    //handle case if cb->pqsize is full
    while(cb->q_size == cb->max_size){
        pthread_cond_wait(&dispatcher_cond, cb->mut);
    }
    if(cb->head == -1){
        cb->head = cb->tail = 0;
        cb->clientfd_queue[cb->tail] = client;
    } else if(cb->tail == cb->max_size - 1 && cb->head != 0){
        cb->tail = 0;
        cb->clientfd_queue[cb->tail] = client;
    } else{
        cb->tail++;
        cb->clientfd_queue[cb->tail] = client;
    }
    //printf("enqueued: %d\n", cb->clientfd_queue[cb->tail]);
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
    message->body_tracker = 0;
    ssize_t total_bytes_recv = 0;
    //ssize_t bytes_recv = recv(client_sockd, message->buffer, sizeof(message->buffer), 0);
    
    while(total_bytes_recv < BUFFER_SIZE){
        ssize_t ret_bytes = recv(client_sockd, message->buffer + total_bytes_recv, BUFFER_SIZE - total_bytes_recv, 0);
        message->buffer[ret_bytes] = '\0';  //maybe add +1 to buffer to add null terminator at end after body
        char* body_check = strstr((char*)message->buffer, "\r\n\r\n");
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
    //char* buf_copy;
    //strcpy(buf_copy, (char*)message->buffer);
    char* token = strtok_r((char*)&message->buffer, "\r\n", &ptr);
    strcpy(message->copyBuffer, token);
    //printf("copy: %swhat it do\n", message->copyBuffer);
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
        //printf("we in here big bruh\n");
        message->status_code = 400;
        dprintf(client_sockd, "%s %d Bad Request\r\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, 0);
        return;
    }
    memmove(message->filename, (message->filename)+1, strlen(message->filename));
        
    if(strlen(token) > BUFFER_SIZE){
        message->status_code = 400;
        //printf("header larger than 4096\n");
        dprintf(client_sockd, "%s %d Bad Request\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, 0);
        return;
    }
    while(token != NULL){
        //printf("token1: %s\n", token);
        if(strstr(token, "Content-Length") != NULL){
            //printf("token2: %s\n", token);
            sscanf(token, "%*s %lu", &(message->content_length));
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

void process_request(ssize_t client_sockd, struct httpObject* message, int logfd) {
    uint8_t body_buffer[BODY_BUFFER_SIZE];
    int file_exists = if_exists(message->filename);
    int file_size = get_file_size(message->filename);
    int file_reg = is_regular_file(message->filename);
    if(strcmp(message->method, "PUT") == 0){
        //printf("in PUT: %s\n", message->filename);
        if(strcmp(message->filename, "healthcheck") == 0){
            message->status_code = 403;
            dprintf(client_sockd, "%s %d Forbidden\r\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, 0);
            return;
        }
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
        //printf("open file: %s\n", message->filename);
        //printf("putfd: %zd\n", putfd);
        if(putfd < 0){
            if(errno == EACCES){
                message->status_code = 403;
                dprintf(client_sockd, "%s %d Forbidden\r\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, 0);
            }
            return;
        }
        //handles 0 byte files
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
        memset(body_buffer, '\0', BODY_BUFFER_SIZE);
        ssize_t bytes_recv = recv(client_sockd, body_buffer, BODY_BUFFER_SIZE, 0);
        //printf("first bytes_recv: %zu\n", bytes_recv);
        if(bytes_recv <= BODY_BUFFER_SIZE){             //check if bytes_recv is less than buffer size    
            while(true){
                cont_len = cont_len - bytes_recv;       //decrement cont_len by number of bytes recv after each iteration to handle any sized files
                //printf("in loop\n");
                ssize_t bytes_written = write(putfd, body_buffer, bytes_recv);         //writes contents of body from request into the created file which is saved in the server
                if(bytes_written < 0){
                    printf("PUT: write 500\n");
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
            printf("done: %s\n", message->filename);
            return;
        }
    }
    //HANDLE GET REQUEST
    else if(strcmp(message->method, "GET") == 0){
        //printf("in GET: %s\n", message->filename);
        //printf("in GET client sockd: %zd\n", client_sockd);
        printf("logfd: %d\n", message->logfd);
        if(strcmp(message->filename, "healthcheck") == 0 && logfd > 0){
            message->status_code = 200;
            char log_errors[100];
            char log_entries[100];
            sprintf(log_errors, "%d", errors);
            sprintf(log_entries, "%d", entries);
            int length = strlen(log_entries) + strlen(log_errors) + 1;
            message->healthcheck_size = length;
            //dprintf(client_sockd, "%s %d OK\r\nContent-Length: ", message->httpversion, message->status_code);
            dprintf(client_sockd, "%s %d OK\r\nContent-Length: %d\r\n\r\n%d\n%d", message->httpversion, message->status_code, length, errors, entries);
            //printf("%s %d OK\r\nContent-Length: %d\r\n\r\n%d\n%d\n", message->httpversion, message->status_code, length, errors, entries);
            return;
        }
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
        if(strcmp(message->filename, "healthcheck") == 0){
            message->status_code = 403;
            dprintf(client_sockd, "%s %d Forbidden\r\nContent-Length: %d\r\n\r\n", message->httpversion, message->status_code, 0);
            return;
        }
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

//calculates the amount of bytes of the hex converted lines in a file
size_t calc_hex_bytes(size_t file_size){
    size_t hex_bytes = 0;
    size_t extra_bytes = 0;
    size_t extra_chars = 0;
    size_t ret = 0;
    hex_bytes = floor(file_size / 20);
    if(file_size % 20 != 0){
        extra_chars = file_size % 20;
        extra_bytes = 8 + extra_chars * 3 + 1;
        ret = (hex_bytes * 69) + extra_bytes;
    } else {
        ret = hex_bytes * 69;
    }
    return ret;
}
void log_func(int logfd, httpObject* msg){
    uint8_t logBuffer[LOG_SIZE];
    uint8_t fileBuffer[LOG_SIZE];
    char filelength[30];
    int file_exists = if_exists(msg->filename);
    size_t file_size = get_file_size(msg->filename);
    if(file_exists != 0){
        file_size = 0;
    }
    size_t filefd = open(msg->filename, O_RDONLY);
    //printf("file fd: %ld\n", filefd);
    size_t read_bytes = read(filefd, fileBuffer, LOG_SIZE);
    printf("read_bytes: %ld\n", read_bytes);
    printf("file size: %ld\n", file_size);
    size_t hex_bytes = calc_hex_bytes(file_size);
    printf("returned bytes: %ld\n", hex_bytes);
    printf("healthcheck_Size: %d\n", msg->healthcheck_size);
    //size_t file_len = strlen(filelength);
    //int namelen = strlen(msg->filename + 1);
    //int methodlen = strlen(msg->method);
    int local_offset = 0;
    sprintf(filelength, "%ld", file_size);
    int cont_len = file_size;
    //printf("initial cont_len: %d\n", cont_len);
    //printf("initial read_bytes: %ld\n", read_bytes);
    size_t lead_zero = 0;
    size_t bytes_written = 0;
    /*
    * If the request fails
    */
    if((msg->status_code == 400) | (msg->status_code == 404) | (msg->status_code == 403) | (msg->status_code == 500)){
        memset(logBuffer, '\0', LOG_SIZE);
        bytes_written = snprintf((char*)logBuffer, LOG_SIZE, "FAIL: %s --- response %d\n========\n", msg->copyBuffer, msg->status_code);
        //printf("file name: %s\n", msg->filename);
        pthread_mutex_lock(&offset_lock);
        local_offset = global_offset;
        global_offset += bytes_written;
        entries++;
        errors++;
        pthread_mutex_unlock(&offset_lock);
        pwrite(logfd, logBuffer, bytes_written, local_offset);
        close(filefd);
        close(read_bytes);
    /*
    * If HEAD request
    */
    } else if(strcmp(msg->method, "HEAD") == 0){
        memset(logBuffer, '\0', LOG_SIZE);
        bytes_written = snprintf((char*)logBuffer, LOG_SIZE, "%s /%s length %zd\n========\n", msg->method, msg->filename, file_size);
        //printf("bytes written: %ld\n", bytes_written);
        pthread_mutex_lock(&offset_lock);
        local_offset = global_offset;
        global_offset += bytes_written;
        entries++;
        pthread_mutex_unlock(&offset_lock);
        pwrite(logfd, logBuffer, bytes_written, local_offset);
        close(filefd);
        close(read_bytes);
    /*
    * If PUT/GET request
    */
    } else {
        printf("-------------------------------------------------------------------\n");
        int footer_line = 9;
        if(strcmp(msg->filename, "healthcheck") == 0){
            char log_err[100];
            char log_ent[100];
            sprintf(log_err, "%d", errors);
            sprintf(log_ent, "%d", entries);
            int health_line = snprintf((char*)logBuffer, LOG_SIZE, "%s /%s length %d\n", msg->method, msg->filename, msg->healthcheck_size);
            size_t health_bytes = calc_hex_bytes(msg->healthcheck_size);
            int health_calc = health_bytes + health_line + footer_line;
            pthread_mutex_lock(&offset_lock);
            local_offset = global_offset;
            global_offset += health_calc;
            pthread_mutex_unlock(&offset_lock);
            pwrite(logfd, logBuffer, health_line, local_offset);
            local_offset += health_line;
            memset(logBuffer, '\0', LOG_SIZE);
            int count = 0;
            /*for(int idx = 0; idx < msg->healthcheck_size; idx++){
                if(idx % 20 == 0){
                    if(idx != 0){
                        bytes_written = snprintf((char*)logBuffer + count, 2, "\n");
                        count += bytes_written;
                    }
                    bytes_written = snprintf((char*)logBuffer + count, LOG_SIZE, "%08ld", lead_zero);
                    lead_zero += 20;
                    count += bytes_written;
                }
                bytes_written = snprintf((char*)logBuffer + count, LOG_SIZE, " %02x", health_body[idx]);
                count += bytes_written;
            }*/
            bytes_written = snprintf((char*)logBuffer + count, LOG_SIZE, "%08ld", lead_zero);
            lead_zero += 20;
            count += bytes_written;
            for(int index = 0; index < 1; index++){
                bytes_written = snprintf((char*)logBuffer + count, LOG_SIZE, " %02x", log_err[index]);
                count += bytes_written;
                bytes_written = snprintf((char*)logBuffer + count, LOG_SIZE, " %02x", '\n');
                count += bytes_written;
                bytes_written = snprintf((char*)logBuffer + count, LOG_SIZE, " %02x", log_ent[index]);
                count += bytes_written;
            }
            
            pwrite(logfd, logBuffer, count, local_offset);
            local_offset += count;
            memset(logBuffer, '\0', LOG_SIZE);
            bytes_written = 0;
            bytes_written += snprintf((char*)logBuffer + bytes_written, LOG_SIZE, "\n========\n");       //footers
            pwrite(logfd, logBuffer, bytes_written, local_offset);
            local_offset += bytes_written;
            pthread_mutex_lock(&offset_lock);
            entries++;
            pthread_mutex_unlock(&offset_lock);
        } else {
            int header_line = snprintf((char*)logBuffer, LOG_SIZE, "%s /%s length %zd\n", msg->method, msg->filename, file_size);
            int full_calc = hex_bytes + header_line + footer_line;
            pthread_mutex_lock(&offset_lock);
            local_offset = global_offset;
            global_offset += full_calc;
            entries++;
            pthread_mutex_unlock(&offset_lock);
            //int header_line = snprintf((char*)logBuffer, LOG_SIZE, "%s /%s length %zd\n", msg->method, msg->filename, file_size);      //header
            pwrite(logfd, logBuffer, header_line, local_offset);
            local_offset += header_line;
            memset(logBuffer, '\0', LOG_SIZE);
            //printf("header: %d\n", header_line);
            //printf("after header local offset: %d\n", local_offset);
            //bytes_written = 0;
            while(true){
                cont_len = cont_len - read_bytes;
                int count = 0;
                for(size_t idx = 0; idx < read_bytes; idx++){
                    //printf("count: %d\n", count);
                    /*if(count > LOG_SIZE + 69){
                        printf("for loop count: %d\n", count);
                        pwrite(logfd, logBuffer, count, local_offset);
                        local_offset += count;
                        printf("for loop local offset: %d\n", local_offset);
                        //memset(logBuffer, '\0', LOG_SIZE);
                        count = 0;
                    }*/
                    if(idx % 20 == 0){
                        if(idx != 0){
                            bytes_written = snprintf((char*)logBuffer + count, 2, "\n");
                            count += bytes_written;
                        }
                        if(count == 69){
                            pwrite(logfd, logBuffer, count, local_offset);
                            local_offset += count;
                            count = 0;
                        }
                        /*
                        * converts chars to hex
                        */
                        bytes_written = snprintf((char*)logBuffer + count, LOG_SIZE, "%08ld", lead_zero);
                        lead_zero += 20;
                        count += bytes_written;
                    }
                    bytes_written = snprintf((char*)logBuffer + count, LOG_SIZE, " %02x", fileBuffer[idx]);
                    count += bytes_written;
                    //printf("%s\n", logBuffer);
                    if(idx == LOG_SIZE - 1){    //writes last line of current read_bytes
                        pwrite(logfd, logBuffer, count, local_offset);
                        local_offset += count;
                        pwrite(logfd, "\n", 1, local_offset);
                        local_offset += 1;
                    }
                }
                if(cont_len == 0){
                    //printf("after for loop local offset: %d\n", local_offset);
                    //printf("outside count: %d\n", count);
                    pwrite(logfd, logBuffer, count, local_offset);
                    local_offset += count;
                    memset(logBuffer, '\0', LOG_SIZE);
                    bytes_written = 0;
                    //printf("after writing body local offset: %d\n", local_offset);
                    bytes_written += snprintf((char*)logBuffer + bytes_written, LOG_SIZE, "\n========\n");       //footers
                    pwrite(logfd, logBuffer, bytes_written, local_offset);
                    local_offset += bytes_written;
                    //printf("after writing footer local offset: %d\n", local_offset);
                    break;
                }
                read_bytes = read(filefd, fileBuffer, LOG_SIZE);
                //printf("read\n");
                //printf("after read cont len: %d\n",cont_len);  
            }
        }
        
    }
    //printf("global_offset: %d\n", global_offset);
    close(filefd);
    close(read_bytes);
}

void* thread_func(void* arg){   //dequeue from buffer
    threadArg *parg = (threadArg*) arg;
    httpObject *pmsg = &parg->cb->msg;
        while(true){
            //printf("--------------------------------------------\n");
            pthread_mutex_lock(parg->cb->mut);
            int c_fd = cb_dequeue(parg->cb);
            pthread_mutex_unlock(parg->cb->mut);
            pthread_cond_signal(&dispatcher_cond);
            //printf("logfd in thread func: %d\n", pmsg->logfd);
            read_http_request(c_fd, &parg->msg);
            process_request(c_fd, &parg->msg, pmsg->logfd);
            if(parg->logfd > 0){
                log_func(parg->logfd, &parg->msg);
            }
            memset(&parg->msg.buffer, '\0', BUFFER_SIZE);
            memset(&parg->msg.method, '\0', METHOD_MAX_SIZE);
            memset(&parg->msg.filename, '\0', FILENAME_MAX_SIZE);
            memset(&parg->msg.httpversion, '\0', HTTPSIZE);
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
    if(argc < 2){
        fprintf(stderr, "No arguments entered\n");
        return EXIT_FAILURE;
    }
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
    if(argc != optind + 1){
        fprintf(stderr, "No port number entered\n");
        return EXIT_FAILURE;
    }
    if(NUM_THREADS == 0){
        NUM_THREADS = 4;
    }
    int logfd = -1;
    if(log_name != NULL){
        logfd = open(log_name, O_CREAT| O_WRONLY |O_TRUNC, 0644);
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
    circleBuff.msg.logfd = logfd;
    //printf("logfd in main: %d\n", circleBuff.msg.logfd);
    
    while(i < NUM_THREADS){
        args[i].cb = &circleBuff;
        args[i].msg = message;
        args[i].logfd = logfd;
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
    ret = listen(server_sockd, SOMAXCONN); // 5 should be enough, if not use SOMAXCONN

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
        int client_sockd = accept(server_sockd, &client_addr, &client_addrlen);
        pthread_mutex_lock(circleBuff.mut);
        cb_enqueue(&circleBuff, client_sockd);
        pthread_mutex_unlock(circleBuff.mut);
        pthread_cond_signal(&worker_cond);
    }
    return EXIT_SUCCESS;
}
