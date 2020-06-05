#include <err.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdbool.h>
#include <pthread.h>
#include <ctype.h>

pthread_cond_t dispatcher_cond = PTHREAD_COND_INITIALIZER;
pthread_cond_t worker_cond = PTHREAD_COND_INITIALIZER;

typedef struct server_info_t {
    int port;
    int entries;
    int errors;
    int status_code;
    int healthcheck_len;
    int serverfd;
    int success_rate;
    int min_index;
    char* status_msg;
    bool alive; //set to false initially
} ServerInfo;

typedef struct servers_t {
    int num_servers;
    ServerInfo *servers;
    pthread_mutex_t mut;
} Servers;

typedef struct circular_buff_t {
    int* clientfd_queue;
    int q_size;
    int max_size;
    int head;
    int tail;
    pthread_mutex_t *cmut;
} circleBuffer;

typedef struct threadarg_t {
    int serverfd;
    int c_port;
    circleBuffer *cb;
} threadArg;
 
Servers servers;


int cb_dequeue(circleBuffer *cb){
    int client_fd;
    //printf("process: %d\n", pindex);
    while(cb->q_size == 0){
        pthread_cond_wait(&worker_cond, cb->cmut);
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
        pthread_cond_wait(&dispatcher_cond, cb->cmut);
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
 * client_connect takes a port number and establishes a connection as a client.
 * connectport: port number of server to connect to
 * TODO: be able to establish multiple client connections AS the loadbalancer
 * returns: valid socket if successful, -1 otherwise
 */
int client_connect(uint16_t connectport) {
    int connfd;
    struct sockaddr_in servaddr;

    connfd=socket(AF_INET,SOCK_STREAM,0);
    if (connfd < 0)
        return -1;
    memset(&servaddr, 0, sizeof servaddr);

    servaddr.sin_family=AF_INET;
    servaddr.sin_port=htons(connectport);

    /* For this assignment the IP address can be fixed */
    inet_pton(AF_INET,"127.0.0.1",&(servaddr.sin_addr));

    if(connect(connfd,(struct sockaddr *)&servaddr,sizeof(servaddr)) < 0)
        return -1;
    return connfd;
}

/*
 * server_listen takes a port number and creates a socket to listen on 
 * that port.
 * port: the port number to receive connections
 * returns: valid socket if successful, -1 otherwise
 */
int server_listen(int port) {
    int listenfd;
    int enable = 1;
    struct sockaddr_in servaddr;

    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0)
        return -1;
    memset(&servaddr, 0, sizeof servaddr);
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(port);

    if(setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) < 0)
        return -1;
    if (bind(listenfd, (struct sockaddr*) &servaddr, sizeof servaddr) < 0)
        return -1;
    if (listen(listenfd, 500) < 0)
        return -1;
    return listenfd;
}

/*
 * bridge_connections send up to 100 bytes from fromfd to tofd
 * fromfd, tofd: valid sockets
 * returns: number of bytes sent, 0 if connection closed, -1 on error
 */
int bridge_connections(int fromfd, int tofd) {
    char recvline[4096];
    int n = recv(fromfd, recvline, 4096, 0);
    if (n < 0) {
        //printf("connection error receiving\n");
        return -1;
    } else if (n == 0) {
        //printf("receiving connection ended\n");
        return 0;
    }
    recvline[n] = '\0';
    //printf("%s", recvline);
    //sleep(1);
    n = send(tofd, recvline, n, 0);
    if (n < 0) {
        //printf("connection error sending\n");
        return -1;
    } else if (n == 0) {
        //printf("sending connection ended\n");
        return 0;
    }
    memset(recvline, '\0', sizeof(recvline));
    return n;
}

/*
 * bridge_loop forwards all messages between both sockets until the connection
 * is interrupted. It also prints a message if both channels are idle.
 * sockfd1, sockfd2: valid sockets
 */
void bridge_loop(int sockfd1, int sockfd2) {
    fd_set set;
    struct timeval timeout;
    int fromfd, tofd;
    while(1) {
        // set for select usage must be initialized before each select call
        // set manages which file descriptors are being watched
        FD_ZERO (&set);
        FD_SET (sockfd1, &set);
        FD_SET (sockfd2, &set);

        // same for timeout
        // max time waiting, 5 seconds, 0 microseconds
        timeout.tv_sec = 5;
        timeout.tv_usec = 0;

        // select return the number of file descriptors ready for reading in set
        switch (select(FD_SETSIZE, &set, NULL, NULL, &timeout)) {
            case -1:
                //printf("error during select, exiting\n");
                return;
            case 0:
                //printf("both channels are idle, waiting again\n");
                continue;
            default:
                if (FD_ISSET(sockfd1, &set)) {
                    fromfd = sockfd1;
                    tofd = sockfd2;
                } else if (FD_ISSET(sockfd2, &set)) {
                    fromfd = sockfd2;
                    tofd = sockfd1;
                } else {
                    //printf("this should be unreachable\n");
                    return;
                }
        }
        //printf("fromfd in bridge loop: %d\n", fromfd);
        //printf("tofd in bridge loop: %d\n", tofd);
        if (bridge_connections(fromfd, tofd) <= 0){
            return;
        }
    }
}

void healthcheck_func() {              //health check thread, sends health check to all servers
    int conn_fd;
    char healthheader[1000];
    char healthbody[1000];
    //sends health check to servers and saves server response inside server struct
    for(int idx = 0; idx < servers.num_servers; idx++) {
        //health check to servers.servers[i].port
        //update servers.servers[idx]
        //printf("number of servers: %d\n", servers.num_servers);
        //printf("server port: %d\n", servers.servers[idx].port);
        if ((conn_fd = client_connect(servers.servers[idx].port)) < 0)
            err(1, "failed connecting");
        //printf("connfd inside healthcheck: %d\n", conn_fd);
        dprintf(conn_fd, "GET /healthcheck HTTP/1.1\r\n\r\n");
        int recv_bytes = recv(conn_fd, healthheader, sizeof(healthheader), 0);
        sscanf(healthheader, "HTTP/1.1 %d %s\r\nContent-Length: %d\r\n\r\n", &servers.servers[idx].status_code, servers.servers[idx].status_msg, &servers.servers[idx].healthcheck_len);
        recv_bytes = recv(conn_fd, healthbody, servers.servers[idx].healthcheck_len, 0);
        sscanf(healthbody, "%d\n%d", &servers.servers[idx].errors, &servers.servers[idx].entries);
        //printf("%s\n", healthheader);
        printf("status: %d\nerrors: %d\nentries: %d\n\n", servers.servers[idx].status_code, servers.servers[idx].errors, servers.servers[idx].entries);
        if(servers.servers[idx].status_code == 200 || servers.servers[idx].status_code == 201) {
            servers.servers[idx].alive = true;
            if(servers.servers[idx].entries != 0)
                servers.servers[idx].success_rate = (1 - (servers.servers[idx].errors / servers.servers[idx].entries));
        }
        close(conn_fd);
    }
    //compares servers with each other, saves minimum entries, if same entries then compares success rate, determines which server port should be sent connections first
    int min = servers.servers[0].entries;
    int minidx = 0;
    for(int idx = 1; idx < servers.num_servers; idx++) {
        if(servers.servers[idx].entries < min){     //server at current idx has less entries than previous minimum
            min = servers.servers[idx].entries;
            minidx = idx;
        } else if(servers.servers[idx].entries == min) {        //server at current idx has same entries as previous minimum
            if(servers.servers[idx].success_rate > servers.servers[minidx].success_rate){
                min = servers.servers[idx].entries;
                minidx = idx;
            } else {
                min = servers.servers[idx].entries;
                minidx = idx;
            }
        }
    }
    servers.servers->min_index = minidx;
}

int init_servers(int argc, char** argv, int start_of_ports) {
    servers.num_servers = argc - start_of_ports;
    if(servers.num_servers <= 0) {
        printf("Error: specify server httpserver ports");
        return -1;
    }
    servers.servers = malloc(servers.num_servers * sizeof(ServerInfo));
    pthread_mutex_init(&servers.mut, NULL);
    for(int idx = start_of_ports; idx < argc; idx++) {
        int sidx = idx - start_of_ports;
        servers.servers[sidx].port = atoi(argv[idx]);
        servers.servers[sidx].alive = false;
        //printf("server index: %d\n", sidx);
        //printf("server port: %d\n", servers.servers[sidx].port);
    }
    pthread_mutex_lock(&servers.mut);
    healthcheck_func();
    pthread_mutex_unlock(&servers.mut);
    return 0;
}

void* thread_func(void* arg) {
    int conn_fd;
    threadArg *parg = (threadArg*) arg;
    while(1){
        if ((conn_fd = client_connect(servers.servers[servers.servers->min_index].port)) < 0)
            err(1, "failed connecting");
        //printf("port number: %d\n", servers.servers[servers.servers->min_index].port);
        //printf("server fd: %d\n", conn_fd);
        pthread_mutex_lock(parg->cb->cmut);
        int c_fd = cb_dequeue(parg->cb);
        //printf("c_fd: %d\n", c_fd);
        pthread_mutex_unlock(parg->cb->cmut);
        pthread_cond_signal(&dispatcher_cond);
        bridge_loop(c_fd, conn_fd);
        close(c_fd);
        close(conn_fd);
    }
}

int main(int argc,char **argv) {
    int connfd, listenfd, acceptfd;
    uint16_t connectport, listenport;
    int c, i, x, num_requests, num_connections = 0;
    
    if (argc < 3) {
        //printf("missing arguments: usage %s port_to_connect port_to_listen", argv[0]);
        return 1;
    }

    // Remember to validate return values
    // You can fail tests for not validating
    while((c = getopt(argc, argv, "R:N:")) != -1){
        switch(c){
            case 'R':
                num_requests = atoi(optarg);
                break;
            case 'N':
                num_connections = atoi(optarg);
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
    if(num_connections == 0) {  //number of client connections that can be handled concurrently
        num_connections = 4;
    }
    if(num_requests == 0) {     //number of requests that are handled before a health check is requested again
        num_requests = 5;
    }
    int c_queue[num_connections];
    connectport = atoi(argv[optind + 1]);    //port of server that client connects to
    listenport = atoi(argv[optind]);      //port that listens for incoming client connections
    init_servers(argc, argv, optind + 1);
    //printf("connectport: %d\n", connectport);
    pthread_t tid[num_connections];
    threadArg args[num_connections];
    pthread_mutex_t(cmutex) = PTHREAD_MUTEX_INITIALIZER;
    circleBuffer circleBuff;
    circleBuff.clientfd_queue = c_queue;
    circleBuff.q_size = 0;
    circleBuff.max_size = num_connections;
    circleBuff.head = -1;
    circleBuff.tail = -1;
    circleBuff.cmut = &cmutex;
    //printf("num connections: %d\n", num_connections);

    while(i < num_connections) {
        args[i].cb = &circleBuff;
        //args[i].c_port = connectport;
        //if ((connfd = client_connect(connectport)) < 0)
        //    err(1, "failed connecting");
        //printf("connfd: %d\n", connfd);
        //args[i].serverfd = connfd;
        pthread_create(&tid[i], NULL, thread_func, &args[i]);
        i++;
    }

    if ((listenfd = server_listen(listenport)) < 0)
        err(1, "failed listening");

    while(1){
        if ((acceptfd = accept(listenfd, NULL, NULL)) < 0)
            err(1, "failed accepting");
        //printf("acceptfd: %d\n", acceptfd);
        pthread_mutex_lock(circleBuff.cmut);
        cb_enqueue(&circleBuff, acceptfd);
        pthread_mutex_unlock(circleBuff.cmut);
        pthread_cond_signal(&worker_cond);
    }
}
