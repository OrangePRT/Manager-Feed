#include "manager.h"


void cleanup_and_exit(ManagerState *state) {
    pthread_mutex_lock(&state->lock);

    // Fechar e remover todos os feeds
    for (int i = 0; i < state->feed_count; i++) {
        close(state->feeds[i].pipe_fd);
        unlink(state->feeds[i].pipe_name);
    }

    // Remover pipe principal
    unlink(MANAGER_PIPE);

    pthread_mutex_unlock(&state->lock);
    pthread_mutex_destroy(&state->lock);

    printf("\nRecursos libertados. A encerrar...\n");
    exit(EXIT_SUCCESS);
}


void sigint_handler(int signo) {
    extern ManagerState global_state; // Estado global para acesso no manipulador
    cleanup_and_exit(&global_state);
}

void init_manager_state(ManagerState *state) {
    state->feed_count = 0;
    state->topic_count = 0;
    pthread_mutex_init(&state->lock, NULL);
}

int add_feed(ManagerState *state, const char *username, const char *pipe_name) {
    pthread_mutex_lock(&state->lock);

    if (state->feed_count >= MAX_FEEDS) {
        pthread_mutex_unlock(&state->lock);
        return -1;
    }

    Feed *feed = &state->feeds[state->feed_count];
    strncpy(feed->username, username, sizeof(feed->username));
    strncpy(feed->pipe_name, pipe_name, sizeof(feed->pipe_name));

    feed->pipe_fd = open(pipe_name, O_WRONLY);
    if (feed->pipe_fd == -1) {
        perror("Erro ao abrir pipe exclusivo do feed");
        pthread_mutex_unlock(&state->lock);
        return -1;
    }

    state->feed_count++;
    pthread_mutex_unlock(&state->lock);
    return 0;
}

void remove_feed(ManagerState *state, const char *username) {
    pthread_mutex_lock(&state->lock);

    for (int i = 0; i < state->feed_count; i++) {
        if (strcmp(state->feeds[i].username, username) == 0) {
            close(state->feeds[i].pipe_fd);
            unlink(state->feeds[i].pipe_name);

            state->feeds[i] = state->feeds[state->feed_count - 1];
            state->feed_count--;
            break;
        }
    }

    pthread_mutex_unlock(&state->lock);
}

Topic *get_or_create_topic(ManagerState *state, const char *name) {
    for (int i = 0; i < state->topic_count; i++) {
        if (strcmp(state->topics[i].name, name) == 0) {
            return &state->topics[i];
        }
    }

    if (state->topic_count >= MAX_TOPICS) {
        return NULL; // Limite de tópicos atingido
    }

    Topic *topic = &state->topics[state->topic_count];
    strncpy(topic->name, name, MAX_TOPIC_NAME);
    topic->sub_count = 0;
    topic->msg_count = 0;
    state->topic_count++;

    return topic;
}

void subscribe_feed_to_topic(ManagerState *state, const char *username, const char *topic_name) {
    pthread_mutex_lock(&state->lock);

    Topic *topic = get_or_create_topic(state, topic_name);
    if (!topic) {
        printf("Erro: Limite de tópicos atingido ou falha ao criar tópico.\n");
        pthread_mutex_unlock(&state->lock);
        return;
    }

    for (int i = 0; i < state->feed_count; i++) {
        if (strcmp(state->feeds[i].username, username) == 0) {
            if (topic->sub_count < MAX_FEEDS) {
                topic->subscribers[topic->sub_count++] = &state->feeds[i];
                printf("Feed '%s' subscrito ao tópico '%s'.\n", username, topic_name);
            } else {
                printf("Erro: Limite de subscritores no tópico '%s'.\n", topic_name);
            }
            break;
        }
    }

    pthread_mutex_unlock(&state->lock);
}

// Remove um feed de um tópico
void unsubscribe_feed_from_topic(ManagerState *state, const char *username, const char *topic_name) {
    pthread_mutex_lock(&state->lock);

    for (int i = 0; i < state->topic_count; i++) {
        if (strcmp(state->topics[i].name, topic_name) == 0) {
            Topic *topic = &state->topics[i];

            for (int j = 0; j < topic->sub_count; j++) {
                if (strcmp(topic->subscribers[j]->username, username) == 0) {
                    // Remover subscrição
                    topic->subscribers[j] = topic->subscribers[topic->sub_count - 1];
                    topic->sub_count--;
                    printf("Feed '%s' cancelou subscrição do tópico '%s'.\n", username, topic_name);

                    // Remover o tópico se não houver subscritores
                    if (topic->sub_count == 0) {
                        state->topics[i] = state->topics[state->topic_count - 1];
                        state->topic_count--;
                        printf("Tópico '%s' removido (sem subscritores).\n", topic_name);
                    }

                    pthread_mutex_unlock(&state->lock);
                    return;
                }
            }

            printf("Feed '%s' não está subscrito ao tópico '%s'.\n", username, topic_name);
        }
    }

    printf("Tópico '%s' não encontrado.\n", topic_name);
    pthread_mutex_unlock(&state->lock);
}

void process_message(ManagerState *state, const Message *msg) {
    pthread_mutex_lock(&state->lock);

    // Obter o tópico
    Topic *topic = NULL;
    for (int i = 0; i < state->topic_count; i++) {
        if (strcmp(state->topics[i].name, msg->topic) == 0) {
            topic = &state->topics[i];
            break;
        }
    }

    if (!topic) {
        printf("Erro: Tópico '%s' não encontrado.\n", msg->topic);
        pthread_mutex_unlock(&state->lock);
        return;
    }

    // Verificar se o tópico está bloqueado
    if (topic->is_locked) {
        printf("Erro: Tópico '%s' está bloqueado. Mensagem rejeitada.\n", msg->topic);

        // Notificar o feed enviador
        for (int i = 0; i < state->feed_count; i++) {
            if (strcmp(state->feeds[i].username, msg->username) == 0) {
                Message error_msg = {0};
                strncpy(error_msg.action, "ERROR", sizeof(error_msg.action));
                strncpy(error_msg.username, "SYSTEM", sizeof(error_msg.username));
                snprintf(error_msg.body, sizeof(error_msg.body), "Erro: Tópico '%s' está bloqueado. Mensagem rejeitada.", msg->topic);

                if (write(state->feeds[i].pipe_fd, &error_msg, sizeof(Message)) == -1) {
                    perror("Erro ao enviar mensagem de erro ao feed");
                }
                break;
            }
        }

        pthread_mutex_unlock(&state->lock);
        return;
    }

    // Verificar se o feed está subscrito ao tópico
    int is_subscribed = 0;
    for (int i = 0; i < topic->sub_count; i++) {
        if (strcmp(topic->subscribers[i]->username, msg->username) == 0) {
            is_subscribed = 1;
            break;
        }
    }

    if (!is_subscribed) {
        printf("Erro: Feed '%s' tentou enviar mensagem ao tópico '%s' sem estar subscrito.\n", msg->username, msg->topic);

        // Notificar o feed enviador
        for (int i = 0; i < state->feed_count; i++) {
            if (strcmp(state->feeds[i].username, msg->username) == 0) {
                Message error_msg = {0};
                strncpy(error_msg.action, "ERROR", sizeof(error_msg.action));
                strncpy(error_msg.username, "SYSTEM", sizeof(error_msg.username));
                snprintf(error_msg.body, sizeof(error_msg.body), "Erro: Não subscrito ao tópico '%s'. Mensagem rejeitada.", msg->topic);

                if (write(state->feeds[i].pipe_fd, &error_msg, sizeof(Message)) == -1) {
                    perror("Erro ao enviar mensagem de erro ao feed");
                }
                break;
            }
        }

        pthread_mutex_unlock(&state->lock);
        return;
    }

    // Guardar mensagens persistentes
    if (msg->duration > 0 && topic->msg_count < 5) {
        // Inicializar a mensagem na posição atual
        topic->messages[topic->msg_count] = *msg;

        // Adicionar o tempo relativo
        topic->messages[topic->msg_count].created_time = state->ticks;

        // Incrementar o contador apenas após a inicialização
        topic->msg_count++;
    }

    // Enviar mensagem para todos os subscritores
    for (int i = 0; i < topic->sub_count; i++) {
        if (write(topic->subscribers[i]->pipe_fd, msg, sizeof(Message)) == -1) {
            perror("Erro ao enviar mensagem ao feed");
        }
    }

    printf("Mensagem enviada ao tópico '%s' por '%s'.\n", msg->topic, msg->username);
    pthread_mutex_unlock(&state->lock);
}



void process_command(ManagerState *state, const Message *msg) {
    if (strcmp(msg->action, "INIT") == 0) {
        if (add_feed(state, msg->username, msg->body) == 0) {
            printf("Feed '%s' conectado.\n", msg->username);
        } else {
            printf("Erro: Limite de feeds atingido ou falha na conexão.\n");
        }
    } else if (strcmp(msg->action, "EXIT") == 0) {
        printf("Feed '%s' desconectado.\n", msg->username);
        remove_feed(state, msg->username);
    } else if (strcmp(msg->action, "MSG") == 0) {
        process_message(state, msg);
    } else if (strcmp(msg->action, "SUB") == 0) {
        subscribe_feed_to_topic(state, msg->username, msg->topic);
    } else if(strcmp(msg->action, "UNSUB")==0){
        unsubscribe_feed_from_topic(state,msg->username, msg->topic);
    } 
}


// Lista os utilizadores conectados
void list_users(ManagerState *state) {
    pthread_mutex_lock(&state->lock);
    printf("Utilizadores conectados:\n");
    for (int i = 0; i < state->feed_count; i++) {
        printf("- %s\n", state->feeds[i].username);
    }
    pthread_mutex_unlock(&state->lock);
}

// Remove um utilizador
void remove_user(ManagerState *state, const char *username) {
    pthread_mutex_lock(&state->lock);
    for (int i = 0; i < state->feed_count; i++) {
        if (strcmp(state->feeds[i].username, username) == 0) {
            // Notificar o feed a ser removido
            Message msg = {0};
            strncpy(msg.action, "EXIT", sizeof(msg.action));
            if (write(state->feeds[i].pipe_fd, &msg, sizeof(Message)) == -1) {
                perror("Erro ao notificar feed");
            }

            close(state->feeds[i].pipe_fd);
            unlink(state->feeds[i].pipe_name);

            // Notificar outros feeds
            Message notif = {0};
            snprintf(notif.body, sizeof(notif.body), "Utilizador '%s' foi removido.", username);
            for (int j = 0; j < state->feed_count; j++) {
                if (j != i) {
                    write(state->feeds[j].pipe_fd, &notif, sizeof(Message));
                }
            }

            // Remover o feed
            state->feeds[i] = state->feeds[state->feed_count - 1];
            state->feed_count--;

            printf("Utilizador '%s' removido.\n", username);
            pthread_mutex_unlock(&state->lock);
            return;
        }
    }
    printf("Utilizador '%s' não encontrado.\n", username);
    pthread_mutex_unlock(&state->lock);
}

// Lista os tópicos existentes
void list_topics(ManagerState *state) {
    pthread_mutex_lock(&state->lock);
    printf("Tópicos existentes:\n");
    for (int i = 0; i < state->topic_count; i++) {
        printf("- %s (Mensagens persistentes: %d, Bloqueado: %s)\n", 
               state->topics[i].name, state->topics[i].msg_count, 
               state->topics[i].is_locked ? "Sim" : "Não");
    }
    pthread_mutex_unlock(&state->lock);
}

// Mostra as mensagens de um tópico
void show_topic_messages(ManagerState *state, const char *topic_name) {
    pthread_mutex_lock(&state->lock);
    for (int i = 0; i < state->topic_count; i++) {
        if (strcmp(state->topics[i].name, topic_name) == 0) {
            printf("Mensagens no tópico '%s':\n", topic_name);
            for (int j = 0; j < state->topics[i].msg_count; j++) {
                printf("- %s: %s\n", state->topics[i].messages[j].username, state->topics[i].messages[j].body);
            }
            pthread_mutex_unlock(&state->lock);
            return;
        }
    }
    printf("Tópico '%s' não encontrado.\n", topic_name);
    pthread_mutex_unlock(&state->lock);
}

// Bloqueia ou desbloqueia um tópico
void set_topic_lock(ManagerState *state, const char *topic_name, int lock) {
    pthread_mutex_lock(&state->lock);
    for (int i = 0; i < state->topic_count; i++) {
        if (strcmp(state->topics[i].name, topic_name) == 0) {
            state->topics[i].is_locked = lock;
            printf("Tópico '%s' %s.\n", topic_name, lock ? "bloqueado" : "desbloqueado");
            pthread_mutex_unlock(&state->lock);
            return;
        }
    }
    printf("Tópico '%s' não encontrado.\n", topic_name);
    pthread_mutex_unlock(&state->lock);
}

// Encerra a plataforma
void close_platform(ManagerState *state) {
    pthread_mutex_lock(&state->lock);
    state->running = 0;

    // Notificar todos os feeds
    Message msg = {0};
    strncpy(msg.action, "EXIT", sizeof(msg.action));
    for (int i = 0; i < state->feed_count; i++) {
        if (write(state->feeds[i].pipe_fd, &msg, sizeof(Message)) == -1) {
            perror("Erro ao notificar feed");
        }
        close(state->feeds[i].pipe_fd);
        unlink(state->feeds[i].pipe_name);
    }
    printf("Plataforma encerrada.\n");

    pthread_mutex_unlock(&state->lock);
}

// Thread para comandos do administrador
void *admin_commands(void *arg) {
    ManagerState *state = (ManagerState *)arg;
    char command[100];

    while (state->running) {
        printf("Admin> ");
        if (fgets(command, sizeof(command), stdin) == NULL) break;

        command[strcspn(command, "\n")] = '\0'; // Remover newline

        if (strcmp(command, "users") == 0) {
            list_users(state);
        } else if (strncmp(command, "remove ", 7) == 0) {
            if (strlen(command + 7) == 0) {
                printf("Erro: O comando 'remove' requer um <username>. Uso: remove <username>\n");
            } else {
                remove_user(state, command + 7);
            }
        } else if (strcmp(command, "topics") == 0) {
            list_topics(state);
        } else if (strncmp(command, "show ", 5) == 0) {
            if (strlen(command + 5) == 0) {
                printf("Erro: O comando 'show' requer um <topico>. Uso: show <topico>\n");
            } else {
                show_topic_messages(state, command + 5);
            }
        } else if (strncmp(command, "lock ", 5) == 0) {
            if (strlen(command + 5) == 0) {
                printf("Erro: O comando 'lock' requer um <topico>. Uso: lock <topico>\n");
            } else {
                set_topic_lock(state, command + 5, 1);
            }
        } else if (strncmp(command, "unlock ", 7) == 0) {
            if (strlen(command + 7) == 0) {
                printf("Erro: O comando 'unlock' requer um <topico>. Uso: unlock <topico>\n");
            } else {
                set_topic_lock(state, command + 7, 0);
            }
        } else if (strcmp(command, "close") == 0) {
            close_platform(state);
            break;
        } else {
            printf("Comando desconhecido: %s. Tente um dos seguintes: users, remove, topics, show, lock, unlock, close\n", command);
        }
    }

    return NULL;
}

void *process_commands_thread(void *arg) {
    struct {
        int fd;
        ManagerState *state;
    } *params = arg;

    int manager_fd = params->fd;
    ManagerState *state = params->state;

    Message msg;

    while (state->running) {
        ssize_t bytes_read = read(manager_fd, &msg, sizeof(Message));
        if (bytes_read > 0) {
            // Processar comando recebido
            process_command(state, &msg);
        } else if (bytes_read == 0) {
            // Fim de comunicação
            break;
        } else {
            perror("Erro ao ler do pipe do manager");
        }
    }

    printf("Thread de processamento de comandos encerrada.\n");
    return NULL;
}


// Função para a Thread de Monitorização
void *monitor_persistent_messages(void *arg) {
    ManagerState *state = (ManagerState *)arg;

    while (state->running) {
        pthread_mutex_lock(&state->lock);

        // Incrementar "ticks"
        state->ticks++;

        for (int i = 0; i < state->topic_count; i++) {
            Topic *topic = &state->topics[i];

            // Verificar mensagens persistentes no tópico
            int new_count = 0;
            for (int j = 0; j < topic->msg_count; j++) {
                Message *msg = &topic->messages[j];
                if (state->ticks - msg->created_time < msg->duration) {
                    topic->messages[new_count++] = *msg;
                } else {
                    printf("Mensagem de '%s' no tópico '%s' expirou e foi removida.\n",
                           msg->username, msg->topic);
                }
            }
            topic->msg_count = new_count;
        }

        pthread_mutex_unlock(&state->lock);

        sleep(1); // Simular "tick" a cada 1 segundo
    }

    return NULL;
}

void save_persistent_messages(ManagerState *state) {
    const char *filename = getenv("MSG_FICH");
    if (!filename) {
        printf("Erro: Variável de ambiente MSG_FICH não definida.\n");
        return;
    }

    FILE *file = fopen(filename, "w");
    if (!file) {
        perror("Erro ao abrir ficheiro para salvar mensagens");
        return;
    }

    pthread_mutex_lock(&state->lock);

    for (int i = 0; i < state->topic_count; i++) {
        Topic *topic = &state->topics[i];
        for (int j = 0; j < topic->msg_count; j++) {
            Message *msg = &topic->messages[j];
            int remaining_time = msg->duration - (state->ticks - msg->created_time);

            if (remaining_time > 0) {
                fprintf(file, "%s %s %d %s\n", 
                        topic->name, 
                        msg->username, 
                        remaining_time, 
                        msg->body);
            }
        }
    }

    pthread_mutex_unlock(&state->lock);
    fclose(file);

    printf("Mensagens persistentes salvas no '%s'.\n", filename);
}

void load_persistent_messages(ManagerState *state) {
    const char *filename = getenv("MSG_FICH");
    if (!filename) {
        printf("Erro: Variável de ambiente MSG_FICH não definida.\n");
        return;
    }

    FILE *file = fopen(filename, "r");
    if (!file) {
        perror("Erro ao abrir ficheiro para carregar mensagens");
        return;
    }

    pthread_mutex_lock(&state->lock);

    char line[512];
    while (fgets(line, sizeof(line), file)) {
        char topic_name[MAX_TOPIC_NAME];
        char username[50];
        int remaining_time;
        char body[MAX_MSG_BODY];

        // Ler os campos do ficheiro
        if (sscanf(line, "%s %s %d %[^\n]", topic_name, username, &remaining_time, body) != 4) {
            printf("Erro: Linha inválida no ficheiro: %s\n", line);
            continue;
        }

        // Encontrar ou criar o tópico
        Topic *topic = NULL;
        for (int i = 0; i < state->topic_count; i++) {
            if (strcmp(state->topics[i].name, topic_name) == 0) {
                topic = &state->topics[i];
                break;
            }
        }
        if (!topic && state->topic_count < MAX_TOPICS) {
            topic = &state->topics[state->topic_count++];
            strncpy(topic->name, topic_name, MAX_TOPIC_NAME);
            topic->sub_count = 0;
            topic->msg_count = 0;
            topic->is_locked = 0;
        }

        if (!topic) {
            printf("Erro: Limite de tópicos atingido ao carregar mensagem do tópico '%s'.\n", topic_name);
            continue;
        }

        // Adicionar a mensagem ao tópico
        if (topic->msg_count < 5) {
            Message *msg = &topic->messages[topic->msg_count++];
            strncpy(msg->username, username, sizeof(msg->username));
            strncpy(msg->body, body, sizeof(msg->body));
            msg->duration = remaining_time;

            // Ajustar o tempo de criação com base no `ticks` atual
            msg->created_time = state->ticks;
        } else {
            printf("Erro: Limite de mensagens atingido no tópico '%s'.\n", topic_name);
        }
    }

    pthread_mutex_unlock(&state->lock);
    fclose(file);

    printf("Mensagens persistentes recuperadas de '%s'.\n", filename);
}



int main() {
    int manager_fd;
    ManagerState state;

    init_manager_state(&state);

    // Configurar manipulador de sinal
    signal(SIGINT, sigint_handler);

    // Inicializar o contador de ticks
    state.ticks = 0;

    // Recuperar mensagens persistentes do ficheiro (se existir)
    load_persistent_messages(&state);

    // Criar o pipe principal
    if (mkfifo(MANAGER_PIPE, 0666) == -1) {
        perror("Erro ao criar pipe do manager");
        return EXIT_FAILURE;
    }

    // Abrir o pipe principal em leitura e escrita para evitar bloqueios
    manager_fd = open(MANAGER_PIPE, O_RDWR);
    if (manager_fd == -1) {
        perror("Erro ao abrir pipe do manager");
        unlink(MANAGER_PIPE);
        return EXIT_FAILURE;
    }

    printf("Manager iniciado. Aguardando conexões...\n");

    // Iniciar a thread para comandos administrativos
    pthread_t admin_thread;
    if (pthread_create(&admin_thread, NULL, admin_commands, &state) != 0) {
        perror("Erro ao criar thread administrativa");
        close(manager_fd);
        unlink(MANAGER_PIPE);
        return EXIT_FAILURE;
    }

    // Iniciar a thread para monitorar mensagens persistentes
    pthread_t monitor_thread;
    if (pthread_create(&monitor_thread, NULL, monitor_persistent_messages, &state) != 0) {
        perror("Erro ao criar thread de monitorização");
        close(manager_fd);
        unlink(MANAGER_PIPE);
        return EXIT_FAILURE;
    }

    // Iniciar a thread para processar mensagens do pipe
    pthread_t command_thread;
    struct {
        int fd;
        ManagerState *state;
    } params = {manager_fd, &state};

    if (pthread_create(&command_thread, NULL, process_commands_thread, &params) != 0) {
        perror("Erro ao criar thread de processamento de comandos");
        close(manager_fd);
        unlink(MANAGER_PIPE);
        return EXIT_FAILURE;
    }

    // Esperar a thread administrativa encerrar
    pthread_join(admin_thread, NULL);
    state.running = 0; // Sinalizar para as threads secundárias pararem

    // Esperar as threads secundárias encerrarem
    pthread_join(monitor_thread, NULL);
    pthread_join(command_thread, NULL);

    // Salvar mensagens persistentes antes de encerrar
    save_persistent_messages(&state);

    // Encerrar o manager
    close(manager_fd);
    unlink(MANAGER_PIPE);

    pthread_mutex_destroy(&state.lock);
    printf("Manager encerrado.\n");

    return EXIT_SUCCESS;
}
