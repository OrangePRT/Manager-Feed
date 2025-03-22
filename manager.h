#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <signal.h>

#define MAX_FEEDS 10
#define MAX_TOPICS 20
#define MAX_TOPIC_NAME 20
#define MAX_MSG_BODY 300
#define MANAGER_PIPE "/tmp/manager_pipe" // Pipe principal para comunicação com feeds

typedef struct {
    char action[10];              // Tipo de ação ("INIT", "MSG", "SUB", "UNSUB", "EXIT")
    char topic[MAX_TOPIC_NAME];   // Nome do tópico
    char username[50];            // Nome do utilizador
    char body[MAX_MSG_BODY];      // Corpo da mensagem
    int duration;                 // Duração (segundos)
    int created_time;             // Tempo de criação em "ticks"
} Message;


typedef struct {
    char username[50];
    char pipe_name[100];
    int pipe_fd;
} Feed;

typedef struct {
    char name[MAX_TOPIC_NAME];
    int locked;
    Feed *subscribers[MAX_FEEDS]; // Lista de feeds subscritos
    int sub_count;
    Message messages[5];          // Mensagens persistentes
    int msg_count;
    int is_locked;
} Topic;

typedef struct {
    Feed feeds[MAX_FEEDS];
    int feed_count;
    Topic topics[MAX_TOPICS];
    int topic_count;
    pthread_mutex_t lock;
    int running; // Flag para encerrar as threads
    int ticks;   // Contador global de "ticks"
} ManagerState;



ManagerState global_state;