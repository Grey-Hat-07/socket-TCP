#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <sys/types.h>
#include <signal.h>

#define MAX_CLIENTS 100
#define BUFFER_SZ 2048
#define PORT 9090

static _Atomic unsigned int cli_count = 0;
static int uid = 10;

typedef struct
{
    struct sockaddr_in address;
    int sockfd;
    int uid;
    char name[32];
} client_t;
client_t *clients[MAX_CLIENTS];
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

void print_ip_addr(struct sockaddr_in addr)
{
    printf("%d.%d.%d.%d\n",
           addr.sin_addr.s_addr & 0xFF,
           (addr.sin_addr.s_addr & 0xFF00) >> 8,
           (addr.sin_addr.s_addr & 0xFF0000) >> 16,
           (addr.sin_addr.s_addr & 0xFF000000) >> 24);
}
void str_overwrite_stdout()
{
    printf("\r%s", ">");
    fflush(stdout);
}
void str_trim_lf(char *a, int length)
{
    int i;
    for (i = 0; i < length; i++)
    {
        if (a[i] == '\n')
        {
            a[i] = '\0';
            break;
        }
    }
}

void queue_add(client_t *cl)
{
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (!clients[i])
        {
            clients[i] = cl;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}
void queue_remove(int uid)
{
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i])
        {
            if (clients[i]->uid == uid)
            {
                clients[i] = NULL;
                break;
            }
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}
void send_message(char *s, int uid)
{
    pthread_mutex_lock(&clients_mutex);

    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i])
        {
            if (clients[i]->uid != uid)
            {
                if (write(clients[i]->sockfd, s, strlen(s)) < 0)
                {
                    printf("ERROR: write to descriptor failed\n");
                    break;
                }
            }
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}
void send_online_clients(int sockfd)
{
    pthread_mutex_lock(&clients_mutex);
    char online_clients[BUFFER_SZ];
    memset(online_clients, 0, sizeof(online_clients));
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i])
        {
            strcat(online_clients, clients[i]->name);
            strcat(online_clients, "\n");
        }
    }
    write(sockfd, online_clients, sizeof(online_clients));
    pthread_mutex_unlock(&clients_mutex);
}
void send_private_message(char *s, int sender_uid, char *recipient_name)
{
    char *substring = strtok(recipient_name, " ");
    pthread_mutex_lock(&clients_mutex);
    // printf("%s",recipient_name);

    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i])
        {
            if (strcmp(clients[i]->name, recipient_name) == 0)
            {
                if (write(clients[i]->sockfd, s, strlen(s)) < 0)
                {
                    printf("ERROR: write to descriptor failed\n");
                    break;
                }
                break;
            }
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

void *handle_client(void *arg)
{
    char buffer[BUFFER_SZ];
    char name[32];
    int leave_flag = 0;
    cli_count++;
    client_t *cli = (client_t *)arg;
    if (recv(cli->sockfd, name, 32, 0) <= 0 || strlen(name) < 2 || strlen(name) >= 32 - 1)
    {
        printf("Didn't enter the name\n");
        leave_flag = 1;
    }
    else
    {
        strcpy(cli->name, name);
        sprintf(buffer, "%s has joined\n", cli->name);
        printf("%s", buffer);
        send_message(buffer, cli->uid);
    }
    bzero(buffer, BUFFER_SZ);

    while (1)
    {
        if (leave_flag)
        {
            break;
        }
        int recev = recv(cli->sockfd, buffer, BUFFER_SZ, 0);
        if (recev > 0)
        {

            if (strlen(buffer) > 0)
            {
                if (strcmp(buffer, "ONLINE\n") == 0)
                {
                    send_online_clients(cli->sockfd);
                }
                else if (strncmp(buffer, "(private)", 9) == 0)
                {
                    char *recipient_name = buffer + 9;             // Extract recipient name
                    char *message_start = strchr(buffer, ' ') + 1; // Find start of message
                    char *message=malloc(sizeof(char)*BUFFER_SZ);
                    for (int i = 0; i < MAX_CLIENTS; i++)
                    {
                        if(clients[i]){
                            if(clients[i]->uid == cli->uid){
                                strcpy(message, clients[i]->name);
                                break;
                            }
                        }
                    }
                    strcat(message, "(private): ");
                    strcat(message, message_start);
                    send_private_message(message, cli->uid, recipient_name);
                    str_trim_lf(buffer, strlen(buffer));
                    // printf("%s (private) -> %s\n", buffer, recipient_name);
                }
                else
                {
                    send_message(buffer, cli->uid);
                    str_trim_lf(buffer, strlen(buffer));
                    // printf("%s -> %s\n",buffer,cli->name);
                    printf("%s\n", buffer);
                }
            }
        }
        else if (recev == 0 || strcmp(buffer, "exit") == 0)
        {
            sprintf(buffer, "%s has left\n", cli->name);
            printf("%s", buffer);
            send_message(buffer, cli->uid);
            leave_flag = 1;
        }
        else
        {
            printf("ERROR: -1");
            leave_flag = 1;
        }
        bzero(buffer, BUFFER_SZ);
    }
    close(cli->sockfd);
    queue_remove(cli->uid);
    free(cli);
    cli_count--;
    pthread_detach(pthread_self());

    return NULL;
}

int main()
{

    char *ip = "127.0.0.1";

    int listenfd = 0, connfd = 0;
    int option = 1;

    struct sockaddr_in serv_addr;
    struct sockaddr_in cli_addr;
    pthread_t tid;

    // socket setting
    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = inet_addr(ip);
    serv_addr.sin_port = htons(PORT);

    // signal setting
    signal(SIGPIPE, SIG_IGN);
    if (setsockopt(listenfd, SOL_SOCKET, (SO_REUSEADDR), (char *)&option, sizeof(option)) < 0)
    {
        printf("ERROR: setsockopt failed");
        return EXIT_FAILURE;
    }

    // bind
    if (bind(listenfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        printf("ERROR: bind failed");
        return EXIT_FAILURE;
    }
    // listen
    if (listen(listenfd, 10) < 0)
    {
        printf("ERROR: listen failed");
        return EXIT_FAILURE;
    }
    printf("\n---WELCOME ----\n");

    while (1)
    {
        socklen_t clilen = sizeof(cli_addr);
        connfd = accept(listenfd, (struct sockaddr *)&cli_addr, &clilen);
        if (connfd < 0)
        {
            printf("ERROR: accept failed");
            return EXIT_FAILURE;
        }
        if ((cli_count + 1) == MAX_CLIENTS)
        {
            printf("MAX_CLIENTS reached, connection rejected\n");
            print_ip_addr(cli_addr);
            close(connfd);
            continue;
        }
        // client setting
        client_t *cli = (client_t *)malloc(sizeof(client_t));
        cli->address = cli_addr;
        cli->sockfd = connfd;
        cli->uid = uid++;

        // add client to queue
        queue_add(cli);
        pthread_create(&tid, NULL, &handle_client, (void *)cli);

        sleep(1);
    }

    return EXIT_SUCCESS;
}