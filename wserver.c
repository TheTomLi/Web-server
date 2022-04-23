#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>    /* Internet domain header */

#include "wrapsock.h"
#include "ws_helpers.h"

#define MAXCLIENTS 10

int handleClient(struct clientstate *cs, char *line);

// You may want to use this function for initial testing
//void write_page(int fd);

int main() {


    unsigned short port = (unsigned short)atoi("50185");
    int listenfd;
    struct clientstate client[MAXCLIENTS];


    // Set up the socket to which the clients will connect
    listenfd = setupServerSocket(port);

    initClients(client, MAXCLIENTS);



    // TODO: complete this function
    // set up fd_set for select
    fd_set allset;
    fd_set rset;
    struct sockaddr_in q;
    struct timeval tv;
    socklen_t len;
    // put listenfd into the set
    FD_ZERO(&allset);
    FD_SET(listenfd, &allset);
    int maxfd = listenfd;

    // 5 minutes for time out
    tv.tv_sec = 300;
    tv.tv_usec = 0;

    int num_connections = 0;
    while(num_connections < 10) {
        rset = allset;

        int timval = Select(maxfd + 1, &rset, NULL, NULL, &tv);
        if (timval == 0){
            printf("Server timed out\n");
            return(0);
        }

        // if listenfd is in the set, make a new connection
        if (FD_ISSET(listenfd, &rset)) {
            printf("a new client is connecting\n");
            len = sizeof(q);
            int clientfd = Accept(listenfd, (struct sockaddr *) &q, &len);
            FD_SET(clientfd, &allset);
            client[num_connections].sock = clientfd;
            num_connections += 1;

            // increase the maxfd by comparing it with the new clientfd
            if (clientfd > maxfd) {
                maxfd = clientfd;
            }
            printf("client %d connected\n", clientfd);
        }
        // iterate through the list and find which fd is ready to be read from
        for (int i = 0; i < num_connections; i++){
            struct clientstate *cs = &client[i];

            // if it is a socket to be read next
            if(FD_ISSET(cs->sock, &rset)){
                char buf[MAXLINE];
                int read_len = read(cs->sock, buf, sizeof(buf) - 1);
                if (read_len < 0){
                    perror("read");
                    printServerError(cs->sock);
                }else if (read_len == 0){
                    if (cs->fd[0] > -1){
                        Close(cs->fd[0]);
                        FD_CLR(cs->fd[0], &allset);
                    }
                    Close(cs->sock);
                    FD_CLR(cs->sock, &allset);
                    resetClient(cs);
                }else{
                    // null terminate buf and pass it to handleclient
                    buf[read_len] = '\0';

                    int result = handleClient(cs, buf);
                    if (result == -1){
                        Close(cs->sock);
                        FD_CLR(cs->sock, &allset);
                        resetClient(cs);
                    }else if (result == 1){
                        // if handleclient returns 1, move to next stage.
                        // call cgi program
                        int pipe_fd = processRequest(cs);
                        if (pipe_fd == -1){
                            Close(cs->sock);
                            FD_CLR(cs->sock, &allset);
                            resetClient(cs);
                        }
                        //update maxfd
                        if (pipe_fd > maxfd){
                            maxfd = pipe_fd;
                        }
                        // let select to keep track of pipe_fd as well
                        FD_SET(pipe_fd, &allset);
                    }
                }
                // if it is a child process to be read from.
            } else if (FD_ISSET(cs->fd[0], &rset) && cs->fd[0] > -1){
                if (cs->output == NULL){
                    cs->output = malloc(sizeof(char)*MAXPAGE);
                    cs->optr = cs->output;
                }
                //read from pipr
                int read_num = read(cs->fd[0], cs->optr, MAXPAGE - (cs->optr - cs->output));
                // read reach the end
                if (read_num == 0){
                    // check the exit status of child process
                    int status;
                    wait(&status);

                    if(status == 0){
                        Close(cs->fd[0]);
                        printOK(cs->sock, cs->output, strlen(cs->output));
                        Close(cs->sock);
                        resetClient(cs);
                        FD_CLR(cs->sock, &allset);
                        FD_CLR(cs->fd[0], &allset);
                    }else if (status == 100){
                        printNotFound(cs->sock);
                        Close(cs->sock);
                        resetClient(cs);
                        FD_CLR(cs->sock, &allset);
                        FD_CLR(cs->fd[0], &allset);
                    } else {
                        printServerError(cs->sock);
                        FD_CLR(cs->sock, &allset);
                        FD_CLR(cs->fd[0], &allset);
                        Close(cs->fd[0]);
                        resetClient(cs);
                    }
                } else if (read_num > 0){
                    // not finished reading yet. move the pointer forward for next read call
                    cs->optr += read_num;
                } else {
                    printServerError(cs->sock);
                    perror("read");
                    exit(1);
                }

            }

        }
    }
    return 0;
}


int completeRequest(char *line){
    if (strlen(line) < 4){
	    return 0;
    }
    char *comparison = strstr(line, "\r\n\r\n");
    if (comparison != NULL){
        printf("completeRequest returns 1\n");
	    return 1;
    }else{
        printf("completeRequest return 0\n");
	    return 0;
    }
}

int getRequest(char *line){
    if (strncmp(line, "GET", 3) == 0){
	    return 1;
    }else{
	    return 0;
    }
}

/* Update the client state cs with the request input in line.
 * Intializes cs->request if this is the first read call from the socket.
 * Note that line must be null-terminated string.
 *
 * Return 0 if the get request message is not complete and we need to wait for
 *     more data
 * Return -1 if there is an error and the socket should be closed
 *     - Request is not a GET request
 *     - The first line of the GET request is poorly formatted (getPath, getQuery)
 *
 * Return 1 if the get request message is complete and ready for processing
 *     cs->request will hold the complete request
 *     cs->path will hold the executable path for the CGI program
 *     cs->query will hold the query string
 *     cs->output will be allocated to hold the output of the CGI program
 *     cs->optr will point to the beginning of cs->output
 */
int handleClient(struct clientstate *cs, char *line) {


    // TODO: Complete this function
    // initialize cs->request in dynamic memory
    if (cs->request == NULL) {
        cs->request = malloc(sizeof(char)*MAXLINE);
    }
    strcat(cs->request, line);
    // check complete request and get request
    if (completeRequest(cs->request) == 0){
        return 0;
    }
    if (getRequest(cs->request) == 0){
        return 0;
    }

    // create path and query string
    // print not found if they are null
    cs->path = getPath(cs->request);
    if (cs->path == NULL) {
        printNotFound(cs->sock);
        return -1;
    }
    cs->query_string = getQuery(cs->request);
    if (cs->query_string == NULL) {
        printNotFound(cs->sock);
        return -1;
    }

    // If the resource is favicon.ico we will ignore the request
    if(strcmp("favicon.ico", cs->path) == 0){
        // A suggestion for debugging output
        fprintf(stderr, "Client: sock = %d\n", cs->sock);
        fprintf(stderr, "        path = %s (ignoring)\n", cs->path);
        printNotFound(cs->sock);
        return -1;
    }

    // A suggestion for printing some information about each client.
    // You are welcome to modify or remove these print statements
    fprintf(stderr, "Client: sock = %d\n", cs->sock);
    fprintf(stderr, "        path = %s\n", cs->path);
    fprintf(stderr, "        query_string = %s\n", cs->query_string);

    return 1;
}

