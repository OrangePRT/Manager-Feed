#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include "signal.h"

#define MAX_TOPIC_NAME 20
#define MAX_MSG_BODY 300
#define MANAGER_PIPE "/tmp/manager_pipe"   // Pipe principal para comunicação com o manager
#define CLIENT_PIPE_BASE "/tmp/feed_pipe_" // Base para o pipe exclusivo do feed
#define ACTION_LEN 10  // Tamanho máximo para as strings de ação

// Estrutura para mensagens enviadas ao manager
typedef struct {
    char action[ACTION_LEN];       // Tipo de ação ("MSG", "SUB", "UNSUB", "EXIT", "INIT")
    char topic[MAX_TOPIC_NAME];   // Nome do tópico
    char username[50];            // Nome do utilizador
    char body[MAX_MSG_BODY];      // Corpo da mensagem
    int duration;                 // Duração (aplicável para mensagens persistentes)
} Message;

// Estrutura para dados compartilhados
typedef struct {
    int client_fd;  // Pipe exclusivo para receber respostas do manager
    int running;    // Flag para encerrar a thread
} ThreadData;

int global_manager_fd = -1;
int global_client_fd = -1;
char global_client_pipe_name[100];
ThreadData global_thread_data;