#include "feed.h"

void cleanup_and_exit(int manager_fd, int client_fd, const char *client_pipe_name, ThreadData *thread_data) {
    printf("\nA Encerrar...\n");

    // Sinalizar para a thread encerrar
    if (thread_data) {
        thread_data->running = 0;
    }

    // Fechar os pipes
    if (manager_fd != -1) {
        close(manager_fd);
    }
    if (client_fd != -1) {
        close(client_fd);
    }

    // Remover o named pipe exclusivo
    if (client_pipe_name) {
        unlink(client_pipe_name);
    }

    printf("Recursos libertados. A terminar...\n");
    exit(EXIT_SUCCESS);
}

void sigint_handler(int signo) {
    extern int global_manager_fd;
    extern int global_client_fd;
    extern char global_client_pipe_name[100];
    extern ThreadData global_thread_data;

    cleanup_and_exit(global_manager_fd, global_client_fd, global_client_pipe_name, &global_thread_data);
}



// Função que envia mensagens ao manager
void send_command_to_manager(int manager_fd, const Message *msg) {
    if (write(manager_fd, msg, sizeof(Message)) == -1) {
        perror("Erro ao enviar comando ao manager");
    }
}

// Thread que escuta respostas do manager
void *listen_manager(void *arg) {
    ThreadData *data = (ThreadData *)arg;
    Message msg;

    while (data->running) {
        ssize_t bytes_read = read(data->client_fd, &msg, sizeof(Message));
        if (bytes_read > 0) {

            if(strcmp(msg.action, "EXIT") == 0){
                printf("Comando de encerramento recebido do manager. A terminar...\n");
                data->running = 0;
                break;
            }

            printf("\n[Mensagem Recebida]\n");
            printf("Tópico: %s\n", msg.topic);
            printf("De: %s\n", msg.username);
            printf("Conteúdo: %s\n", msg.body);
            printf("> ");
            fflush(stdout);
        } else if (bytes_read == 0) {
            // Pipe foi fechado pelo manager
            
            data->running = 0;
            break;
        } else {
            perror("Erro ao ler do pipe do manager");
            break;
        }
    }

    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Uso: %s <username>\n", argv[0]);
        return EXIT_FAILURE;
    }

    signal(SIGINT, sigint_handler); // Configurar manipulador de sinal

    // Variáveis para gerenciar recursos
    char *username = argv[1];
    global_manager_fd = -1;
    global_client_fd = -1;
    snprintf(global_client_pipe_name, sizeof(global_client_pipe_name), "%s%s", CLIENT_PIPE_BASE, username);


    int manager_fd, client_fd;
    char client_pipe_name[100];
    ThreadData thread_data;

    // Criar um named pipe exclusivo para o feed
    snprintf(client_pipe_name, sizeof(client_pipe_name), "%s%s", CLIENT_PIPE_BASE, username);
    if (mkfifo(client_pipe_name, 0666) == -1) {
        perror("Erro ao criar pipe exclusivo do feed");
        return EXIT_FAILURE;
    }

    // Abrir o pipe do manager para envio de comandos
    manager_fd = open(MANAGER_PIPE, O_WRONLY);
    if (manager_fd == -1) {
        perror("Erro ao abrir pipe do manager");
        unlink(client_pipe_name);
        return EXIT_FAILURE;
    }

    // Enviar informações iniciais ao manager (username e nome do pipe)
    Message init_msg = {0};
    strncpy(init_msg.action, "INIT", ACTION_LEN);
    strncpy(init_msg.username, username, sizeof(init_msg.username));
    strncpy(init_msg.body, client_pipe_name, MAX_MSG_BODY);

    send_command_to_manager(manager_fd, &init_msg);

    // Aguardar confirmação do manager
    printf("Aguardando confirmação do manager...\n");
    client_fd = open(client_pipe_name, O_RDONLY);
    if (client_fd == -1) {
        perror("Erro ao abrir pipe exclusivo para leitura");
        close(manager_fd);
        unlink(client_pipe_name);
        return EXIT_FAILURE;
    }

    printf("Conexão estabelecida com o manager!\n");

    // Iniciar a thread para escutar respostas do manager
    pthread_t listener_thread;
    thread_data.client_fd = client_fd;
    thread_data.running = 1;
    if (pthread_create(&listener_thread, NULL, listen_manager, &thread_data) != 0) {
        perror("Erro ao criar a thread");
        close(manager_fd);
        close(client_fd);
        unlink(client_pipe_name);
        return EXIT_FAILURE;
    }

    // Loop principal para comandos do utilizador
    char command[100];
    while (thread_data.running) {
        printf("> ");
        if (fgets(command, sizeof(command), stdin) == NULL) {
            break;
        }

        // Remover newline do comando
        command[strcspn(command, "\n")] = '\0';

        if (strcmp(command, "exit") == 0) {
            printf("A sair...\n");

            // Enviar comando EXIT ao manager
            Message exit_msg = {0};
            strncpy(exit_msg.action, "EXIT", ACTION_LEN);
            strncpy(exit_msg.username, username, sizeof(exit_msg.username));

            send_command_to_manager(manager_fd, &exit_msg);
            break;
        } else if (strncmp(command, "msg ", 4) == 0) {
            // Comando MSG
            Message msg = {0};
            char topic[MAX_TOPIC_NAME];
            int duration;
            char body[MAX_MSG_BODY];

            if (sscanf(command + 4, "%s %d %[^\n]", topic, &duration, body) < 3) {
                printf("Formato inválido. Uso: msg <topico> <duracao> <mensagem>\n");
                continue;
            }

            strncpy(msg.action, "MSG", ACTION_LEN);
            strncpy(msg.topic, topic, MAX_TOPIC_NAME);
            strncpy(msg.username, username, sizeof(msg.username));
            strncpy(msg.body, body, MAX_MSG_BODY);
            msg.duration = duration;

            send_command_to_manager(manager_fd, &msg);
            printf("Mensagem enviada para o tópico '%s'.\n", topic);
        } else if (strncmp(command, "subscribe ", 10) == 0) {
            // Comando SUBSCRIBE
            Message msg = {0};
            char topic[MAX_TOPIC_NAME];

            if (sscanf(command + 10, "%s", topic) != 1) {
                printf("Formato inválido. Uso: subscribe <topico>\n");
                continue;
            }

            strncpy(msg.action, "SUB", ACTION_LEN);
            strncpy(msg.topic, topic, MAX_TOPIC_NAME);
            strncpy(msg.username, username, sizeof(msg.username));

            send_command_to_manager(manager_fd, &msg);
            printf("Subscrito ao tópico '%s'.\n", topic);
        } else if (strncmp(command, "unsubscribe ", 12) == 0) {
            // Comando UNSUBSCRIBE
            Message msg = {0};
            char topic[MAX_TOPIC_NAME];

            if (sscanf(command + 12, "%s", topic) != 1) {
                printf("Formato inválido. Uso: unsubscribe <topico>\n");
                continue;
            }

            strncpy(msg.action, "UNSUB", ACTION_LEN);
            strncpy(msg.topic, topic, MAX_TOPIC_NAME);
            strncpy(msg.username, username, sizeof(msg.username));

            send_command_to_manager(manager_fd, &msg);
            printf("Subscrição removida do tópico '%s'.\n", topic);
        } else {
            printf("Comando desconhecido: %s. Tente um dos seguintes: topics, msg, subscribe, unsubscribe, exit.\n", command);
        }
    }

    // Encerrar a thread e limpar recursos
    thread_data.running = 0;
    pthread_join(listener_thread, NULL);
    close(manager_fd);
    close(client_fd);
    unlink(client_pipe_name);

    return EXIT_SUCCESS;
}
