#include <mictcp.h>
#include <api/mictcp_core.h>
#include <stdio.h>
#include <pthread.h>
#include <errno.h>

#define DEBUG

#ifdef DEBUG
#include <time.h>
#endif

#define MAX_NB_SOCKET 20
#define RAND_PORT 35682
#define MAX_IP_ADDR_SIZE 15
#define TIMEOUT 3000
#define MAX_RESEND 3

#define ERROR(message, err) error(message, __FUNCTION__, __LINE__, err)

#ifdef DEBUG
#define TIMESTAMP timestamp()
#define DEBUG_APPEL_FUNCTION printf("[MIC-TCP] Appel de la fonction: ");\
    printf(__FUNCTION__);\
    printf("\n")
#define PRINT_HEADER(pdu, send) TIMESTAMP; \
    if (send) {                            \
        printf("\tpdu sent: \n"); \
    } else { \
        printf("\tpdu recieved: \n"); \
    } \
    print_info(pdu)
#else
#define TIMESTAMP
#define DEBUG_APPEL_FUNCTION
#define PRINT_HEADER(pdu, send)
#endif


/*
 * Permet de créer un socket entre l’application et MIC-TCP
 * Retourne le descripteur du socket ou bien -1 en cas d'erreur
 */

extern int errno;

static struct mic_tcp_sock sockets[MAX_NB_SOCKET];
static int used_sockets = 0;
static unsigned int seq_num = 0;
static unsigned int ack_num = 0;

static pthread_mutex_t listen_mut = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t listen_cond = PTHREAD_COND_INITIALIZER;

static void error(const char* message, const char* function, const int line, const int err) {
    fprintf(stderr, "[MIC_TCP] Error in %s at line %d: %s", function, line, message);
    if (err == -1) {
        fprintf(stderr, "\n");
    } else {
        fprintf(stderr, ": %s\n", strerror(err));
    }
    exit(EXIT_FAILURE);
}

#ifdef DEBUG
static void timestamp() {
    time_t rawtime;
    struct tm* timeinfo;
    char buff[12];
    time(&rawtime);
    timeinfo = localtime(&rawtime);
    strftime(buff, 16, "[%H:%M:%S]", timeinfo);
    puts(buff);
}
#endif

static void print_info(struct mic_tcp_pdu const pdu) {
    printf("\tpdu.header.seq_num = %d\n", pdu.header.seq_num);
    printf("\tpdu.header.ack_num = %d\n", pdu.header.ack_num);
    printf("\tpdu.header.syn = %d\n", pdu.header.syn);
    printf("\tpdu.header.ack = %d\n", pdu.header.ack);
    printf("\tpdu.header.fin = %d\n", pdu.header.fin);
    printf("\tpdu.payload.size = %d\n", pdu.payload.size);
}

static int check_ack(struct mic_tcp_pdu pdu, struct mic_tcp_sock_addr pdu_addr, struct mic_tcp_sock_addr dest_addr) {
    if (pdu_addr.ip_addr_size != dest_addr.ip_addr_size) {
        printf("[MIC_TCP] Did not recieve a packet from the right ip adress, size didn't match: %s\n", __FUNCTION__);
        return 0;
    } else if (pdu_addr.port != dest_addr.port) {
        printf("[MIC_TCP] Did not recieve a packet from the right port: %s\n", __FUNCTION__);
        return 0;
    /* } else if (strncmp(pdu_addr.ip_addr, dest_addr.ip_addr, dest_addr.ip_addr_size - 1) && strncmp(pdu_addr.ip_addr, "127.0.0.1", dest_addr.ip_addr_size - 1)) { */
    /*     printf("dest_addr.ip_addr_size = %d\n", dest_addr.ip_addr_size); */
    /*     printf("src.addr = %s\ndest.addr = %s\n", pdu_addr.ip_addr, dest_addr.ip_addr); */
    /*     printf("[MIC_TCP] Did not recieve a packet from the right ip adress: %s\n", __FUNCTION__); */
    /*     return 0; TODO check ip address*/
    } else if (pdu.header.ack && !pdu.header.syn && !pdu.header.fin) {
        ++seq_num;
        #ifdef DEBUG
        printf("[MIC_TCP] %s was succesfull\n", __FUNCTION__);
        #endif
        return 1;
    }
    return 0;
}

static int send_ack(struct mic_tcp_sock_addr dest_addr, unsigned short src_port, char increment) {
    struct mic_tcp_pdu ack;
    ack.header.source_port = src_port;
    ack.header.dest_port = dest_addr.port;
    ack.header.seq_num = 0;
    ack.header.ack_num = ack_num;
    ack.header.syn = 0;
    ack.header.ack = 1;
    ack.header.fin = 0;
    ack.payload.data = NULL;
    ack.payload.size = 0;
    #ifdef DEBUG
    printf("[MIC_TCP] sent packet from %d port to %d port %s\n", src_port, dest_addr.port, __FUNCTION__);
    #endif
    PRINT_HEADER(ack, 1);
    if (IP_send(ack, dest_addr) == -1) {
        printf("[MIC_TCP] Could not send ack\n");
        return -1;
    }
    if (increment) {
        ++ack_num;
    }
    return 0;
}

static int wait_for_pdu_and_send_ack(struct mic_tcp_pdu* pdu, struct mic_tcp_sock_addr* addr, int socket,
                                     int condition(int socket, struct mic_tcp_pdu*, struct mic_tcp_sock_addr*), unsigned long timeout) {
    char pdu_recieved;
    /* flush_buffer(); */
    pdu_recieved = 0;
    DEBUG_APPEL_FUNCTION;
    for (int i = 0; i < MAX_RESEND; ++i) {
        if (IP_recv(pdu, addr, timeout) == -1) {
            printf("[MIC_TCP] Timeout reached... trying again\n");
        } else {
            PRINT_HEADER(*pdu, 0);
            if (condition(socket, pdu, addr)) {
                pdu_recieved = 1;
                break;
            } else {
                printf("[MIC_TCP] Recieved an unexpected package\n");
            }
        }
    }
    if (!pdu_recieved) {
        #ifdef DEBUG
        printf("[MIC_TCP] %d timeouts reached while waiting for pdu, aborting\n", MAX_RESEND);
        #endif
        return -1;
    } else {
        if (send_ack(*addr, pdu->header.dest_port, 1) == - 1) {
            printf("[MIC_TCP] Was unable to send ack\n");
            return -1;
        }
    }
    return 0;
}

/* return -1 on timeouts or sending error, 0 on success, 1 on unexpected package */
static int send_and_wait_for_ack(struct mic_tcp_pdu pdu, struct mic_tcp_sock_addr dest_addr, struct mic_tcp_pdu *response, struct mic_tcp_sock_addr *response_addr) {
    char pdu_recieved;
    struct mic_tcp_pdu res;
    struct mic_tcp_sock_addr res_addr;
    struct mic_tcp_pdu* pres;
    struct mic_tcp_sock_addr* pres_addr;
    if (response == NULL) {
        pres = &res;
    } else {
        pres = response;
    }
    if (response_addr == NULL) {
        pres_addr = &res_addr;
    } else {
        pres_addr = response_addr;
    }
    DEBUG_APPEL_FUNCTION;
    PRINT_HEADER(pdu, 1);
    if (IP_send(pdu, dest_addr) == -1) {
        printf("[MIC_TCP] Could not send pdu\n");
        return -1;
    }
    pdu_recieved = 0;
    for (int i = 0; i < MAX_RESEND; ++i) {
        if (IP_recv(pres, pres_addr, TIMEOUT) != -1) {
            PRINT_HEADER(*pres, 0);
            if (check_ack(*pres, *pres_addr, dest_addr)) {
                pdu_recieved = 1;
                break;
            } else {
                printf("[MIC_TCP] Recieved unexpected package, ignoring it\n");
            }
        } else {
            if (i < MAX_RESEND - 1) {
                #ifdef DEBUG
                sleep(1);
                #endif // DEBUG
                printf("[MIC_TCP] Could not recieve ack before timeout, resending (tries number = %d)\n", i + 1);
                PRINT_HEADER(pdu, 1);
                if (IP_send(pdu, dest_addr) == -1) {
                    printf("[MIC_TCP] Could not send pdu\n");
                    return -1;
                }
            }
        }
    }
    if (!pdu_recieved) {
        return -1;
    }
    return 0;
}

int mic_tcp_socket(start_mode sm)
{
    int result = -1;
    DEBUG_APPEL_FUNCTION;
    result = initialize_components(sm); /* Appel obligatoire */
    set_loss_rate(0);

    if (used_sockets < MAX_NB_SOCKET) {
        sockets[used_sockets].fd = used_sockets;
        sockets[used_sockets].state = CLOSED;
        sockets[used_sockets].addr.ip_addr = NULL;
        sockets[used_sockets].addr.ip_addr_size=0;
        sockets[used_sockets].addr.port = RAND_PORT;
        result = sockets[used_sockets].fd;
        ++used_sockets;
    }

    return result;
}

/*
 * Permet d’attribuer une adresse à un socket.
 * Retourne 0 si succès, et -1 en cas d’échec
 */
int mic_tcp_bind(int socket, mic_tcp_sock_addr addr)
{
    DEBUG_APPEL_FUNCTION;
    if (sockets[socket].fd == socket){
        int ip_size = strnlen(addr.ip_addr, MAX_IP_ADDR_SIZE);
        if (sockets[socket].addr.ip_addr != NULL) {
            free(sockets[socket].addr.ip_addr);
        }
        sockets[socket].addr.ip_addr =  malloc((ip_size + 1) * sizeof(char));
        strncpy(sockets[socket].addr.ip_addr, addr.ip_addr, ip_size + 1);
        sockets[socket].addr.ip_addr_size = ip_size + 1;
        sockets[socket].addr.port = addr.port;
        /* memcpy((char *)&sockets[socket].addr, (char *)&addr, sizeof(mic_tcp_sock_addr)); */
        /* sockets[socket].addr.ip_addr=malloc((ip_size +1 )*sizeof(char)); */
        /* strncpy(sockets[socket].addr.ip_addr, addr.ip_addr, ip_size + 1); */
        return 0;
    }
    else {
        return -1;
    }
}

/*
 * Met le socket en état d'acceptation de connexions
 * Retourne 0 si succès, -1 si erreur
 */
int mic_tcp_accept(int socket, mic_tcp_sock_addr* addr)
{
    int err;
    (void) addr;
    DEBUG_APPEL_FUNCTION;
    /* flush_buffer(); */

    if((sockets[socket].state = CLOSED)){
        sockets[socket].state = IDLE;
        if (pthread_mutex_lock(&listen_mut)) {
            ERROR("Could not lock the listen mutex", errno);
        }
        if ((err = pthread_cond_wait(&listen_cond, &listen_mut))) {
            ERROR("Could not wait for listen condition", errno);
        }
        if (pthread_mutex_unlock(&listen_mut)) {
            ERROR("Could not unlock the listen mutex", errno);
        }
        printf("[MIC_TCP] exiting from tcp_accept\n");
        sockets[socket].state = ESTABLISHED;
        return 0;
    }
    else{
        return -1;
    }
}

/*
 * Permet de réclamer l’établissement d’une connexion
 * Retourne 0 si la connexion est établie, et -1 en cas d’échec
 */

static int mic_tcp_connect_syn_condition(int socket, struct mic_tcp_pdu* pdu, struct mic_tcp_sock_addr* addr) {
        if (addr->ip_addr_size != sockets[socket].addr.ip_addr_size) {
            printf("[MIC_TCP] Did not recieve a packet from the right ip adress: %s\n", __FUNCTION__);
            #ifdef DEBUG
            printf("[MIC_TCP] dest_addr.ip_addr_size = %d\n[MIC_TCP] sockets[socket].addr.ip_addr_size = %d\n", addr->ip_addr_size, sockets[socket].addr.ip_addr_size);
            #endif
            return 0;
        } else if (addr->port != sockets[socket].addr.port) {
            printf("[MIC_TCP] Did not recieve a packet from the right port: %s\n", __FUNCTION__);
            #ifdef DEBUG
            printf("[MIC_TCP] dest_addr.port = %d\n[MIC_TCP] sockets[socket].addr.port = %d\n", addr->port, sockets[socket].addr.port);
            #endif
            return 0;
        } else if (!strncmp(addr->ip_addr, sockets[socket].addr.ip_addr, addr->ip_addr_size)) {
            printf("[MIC_TCP] Did not recieve a packet from the right ip adress: %s\n", __FUNCTION__);
            return 0;
        } else if (pdu->header.seq_num != ack_num) {
            printf("[MIC_TCP] Packet did not acknowledge the right number: %s\n", __FUNCTION__);
            #ifdef DEBUG
            printf("[MIC_TCP] pdu.header.ack_num = %d\n[MIC_TCP] seq_num = %d\n", pdu->header.ack_num, seq_num);
            #endif
            return 0;
        } else if (!pdu->header.syn || pdu->header.ack || pdu->header.fin) {
            printf("[MIC_TCP] Unexpected packet was recieved: %s\n", __FUNCTION__);
            #ifdef DEBUG
            printf("[MIC_TCP] syn = %d\n", pdu->header.syn);
            printf("[MIC_TCP] ack = %d\n", pdu->header.ack);
            printf("[MIC_TCP] fin = %d\n", pdu->header.fin);
            #endif
            return 0;
        } else if (pdu->payload.size != 0) {
            printf("[MIC_TCP] Packet had an unexpected payload: %s\n", __FUNCTION__);
            return 0;
        }
        return 1;
}

int mic_tcp_connect(int socket, mic_tcp_sock_addr addr) {
    struct mic_tcp_pdu pdu;
    struct mic_tcp_sock_addr dest_addr;
    struct mic_tcp_pdu response;
    struct mic_tcp_sock_addr response_addr;
    int ip_size;
    int res;
    DEBUG_APPEL_FUNCTION;
    /* flush_buffer(); */
    if (sockets[socket].state != CLOSED) {
        printf("[MIC_TCP] Socket was not Closed: %s\n", __FUNCTION__);
        return -1;
    }
    ip_size = strnlen(addr.ip_addr, MAX_IP_ADDR_SIZE);
    if (sockets[socket].addr.ip_addr != NULL) {
        free(sockets[socket].addr.ip_addr);
    }
    sockets[socket].addr.ip_addr =  malloc((ip_size + 1) * sizeof(char));
    strncpy(sockets[socket].addr.ip_addr, addr.ip_addr, ip_size + 1);
    sockets[socket].addr.ip_addr_size = ip_size + 1;
    sockets[socket].addr.port = addr.port;

    pdu.header.source_port = RAND_PORT;
    pdu.header.dest_port = addr.port;
    pdu.header.seq_num = seq_num;
    pdu.header.ack_num = 0;
    pdu.header.syn = 1;
    pdu.header.ack = 0;
    pdu.header.fin = 0;
    pdu.payload.data = NULL;
    pdu.payload.size = 0;
    res = send_and_wait_for_ack(pdu, sockets[socket].addr, &response, &response_addr);
    switch (res) {
        case -1:
            printf("[MIC_TCP] Syn packet was lost: %s\n", __FUNCTION__);
            return -1;
            break;
        case 0:
            sockets[socket].state = SYN_SENT;
            if (wait_for_pdu_and_send_ack(&pdu, &dest_addr, socket, mic_tcp_connect_syn_condition, TIMEOUT) == -1) {
                ERROR("Problem during reception of SYN", -1);
            } else {
                sockets[socket].state = ESTABLISHED;
                /* ++seq_num; */
                /* if (send_ack(sockets[socket].addr, RAND_PORT, 1) == -1) { */
                /*     printf("[MIC_TCP] Syn packet was lost: %s\n", __FUNCTION__); */
                /*     sockets[socket].state = CLOSED; */
                /*     return -1; */
                /* } */
            }
            return 0;
            break;
        case 1:
            if (mic_tcp_connect_syn_condition(socket, &response, &response_addr)) {
                sockets[socket].state = ESTABLISHED;
                ++seq_num;
                if (send_ack(sockets[socket].addr, RAND_PORT, 1) == -1) {
                    printf("[MIC_TCP] Syn packet was lost: %s\n", __FUNCTION__);
                    sockets[socket].state = CLOSED;
                    return -1;
                }
            } else {
                printf("[MIC_TCP] Recieved unexpected package: %s\n", __FUNCTION__);
            }
            return 0;
            break;
        default:
            return -1;
            break;
    }
}

/*
 * Permet de réclamer l’envoi d’une donnée applicative
 * Retourne la taille des données envoyées, et -1 en cas d'erreur
 */
int mic_tcp_send (int socket, char* mesg, int mesg_size)
{
    struct mic_tcp_pdu pdu;
    DEBUG_APPEL_FUNCTION;
    pdu.header.source_port = RAND_PORT;
    pdu.header.dest_port = sockets[socket].addr.port;
    pdu.header.seq_num = seq_num;
    pdu.header.ack_num = 0;
    pdu.header.syn = 0;
    pdu.header.ack = 0;
    pdu.header.fin = 0;
    pdu.payload.data = mesg;
    pdu.payload.size = mesg_size;
    if (send_and_wait_for_ack(pdu, sockets[socket].addr, NULL, NULL) == -1) {
        printf("[MIC_TCP] Syn packet was lost: %s\n", __FUNCTION__);
        return -1;
    }
    return 0;
}

/*
 * Permet à l’application réceptrice de réclamer la récupération d’une donnée
 * stockée dans les buffers de réception du socket
 * Retourne le nombre d’octets lu ou bien -1 en cas d’erreur
 * NB : cette fonction fait appel à la fonction app_buffer_get()
 */
int mic_tcp_recv (int socket, char* mesg, int max_mesg_size)
{
    struct mic_tcp_payload payload;
    DEBUG_APPEL_FUNCTION;
    (void) socket;
    // TODO check with socket
    payload.size = max_mesg_size;
    payload.data = mesg;
    return app_buffer_get(payload);
}

/*
 * Permet de réclamer la destruction d’un socket.
 * Engendre la fermeture de la connexion suivant le modèle de TCP.
 * Retourne 0 si tout se passe bien et -1 en cas d'erreur
 */

static int mic_tcp_close_fin_condition(int socket, struct mic_tcp_pdu* pdu, struct mic_tcp_sock_addr* addr) {
    if (addr->ip_addr_size != sockets[socket].addr.ip_addr_size) {
        printf("[MIC_TCP] Did not recieve a packet from the right ip adress\n");
        return 0;
    } else if (addr->port != sockets[socket].addr.port) {
        printf("[MIC_TCP] Did not recieve a packet from the right port\n");
        #ifdef DEBUG
        printf("[MIC_TCP] src.port = %d\n[MIC_TCP] dest.port = %d\n", addr->port, sockets[socket].addr.port);
        #endif
        return 0;
    } else if (!strncmp(addr->ip_addr, sockets[socket].addr.ip_addr, addr->ip_addr_size)) {
        printf("[MIC_TCP] Did not recieve a packet from the right ip adress\n");
        return 0;
    } else if (pdu->header.syn || pdu->header.ack || !pdu->header.fin) {
        printf("[MIC_TCP] Flags did not show a fin packet\n");
        return 0;
    } else if (pdu->payload.size != 0) {
        printf("[mic_TCP] Unnecessary data was send, did not read it\n");
    }
    return 1;
}

/* static int check_fin(struct mic_tcp_pdu) */
int mic_tcp_close (int socket)
{
    (void) socket;
    struct mic_tcp_pdu fin;
    struct mic_tcp_sock_addr addr;
    struct mic_tcp_pdu response;
    struct mic_tcp_sock_addr res_addr;
    int res;
    DEBUG_APPEL_FUNCTION;
    fin.header.source_port = RAND_PORT;
    fin.header.dest_port = sockets[socket].addr.port;
    fin.header.seq_num = seq_num;
    fin.header.ack_num = 0;
    fin.header.syn = 0;
    fin.header.ack = 0;
    fin.header.fin = 1;
    fin.payload.data = NULL;
    fin.payload.size = 0;
    res = send_and_wait_for_ack(fin, sockets[socket].addr, &response, &res_addr);
    switch (res) {
        case -1:
            printf("[MIC_TCP] Was unable to send fin or recieve ack: %s\n", __FUNCTION__);
            break;
        case 0:
            break;
        case 1:
            if (!mic_tcp_close_fin_condition(socket, &response, &res_addr)) {
                printf("[MIC_TCP] recieved unexpected package: %s\n", __FUNCTION__);
            }
            break;
        default:
            break;
    }
    if (wait_for_pdu_and_send_ack(&fin, &addr, socket, mic_tcp_close_fin_condition, TIMEOUT) == -1) {
        printf("[MIC_TCP] Was unable to recieve fin or send ack: %s\n", __FUNCTION__);
    }
    return 0;
}

/*
 * Traitement d’un PDU MIC-TCP reçu (mise à jour des numéros de séquence
 * et d'acquittement, etc.) puis insère les données utiles du PDU dans
 * le buffer de réception du socket. Cette fonction utilise la fonction
 * app_buffer_put().
 */

void process_received_PDU(mic_tcp_pdu pdu, mic_tcp_sock_addr addr)
{
    char socket_found = 0;
    /* int err; */
    struct mic_tcp_pdu to_send;
    int err;
    DEBUG_APPEL_FUNCTION;
    PRINT_HEADER(pdu, 0);
    for (int i = 0; i < used_sockets; ++i) {
        if (sockets[i].addr.port == pdu.header.dest_port) {
            switch (sockets[i].state) {
                case IDLE:
                    #ifdef DEBUG
                    printf("[MIC_TCP] IDLE\n");
                    #endif
                    /* printf("server was idle\n"); */
                    if (pdu.header.syn && !pdu.header.ack && !pdu.header.fin) {
                        ack_num = pdu.header.seq_num;
                        sockets[i].state = SYN_RECEIVED;
                        if (send_ack(addr, sockets[i].addr.port, 1) == -1) {
                            printf("[MIC_TCP] SYN-Ack was lost: %s\n", __FUNCTION__);
                        }
                        to_send.header.source_port = sockets[i].addr.port;
                        to_send.header.dest_port = addr.port;
                        to_send.header.seq_num = seq_num;
                        to_send.header.ack_num = 0;
                        to_send.header.syn = 1;
                        to_send.header.ack = 0;
                        to_send.header.fin = 0;
                        to_send.payload.data = NULL;
                        to_send.payload.size = 0;
                        if (send_and_wait_for_ack(to_send, addr, NULL, NULL) == -1) {
                            printf("[MIC_TCP] Was unable to send SYN-ACK or recieve ACK: %s\n", __FUNCTION__);
                        }
                        sockets[i].state = ESTABLISHED;
                        if (pthread_mutex_lock(&listen_mut)) {
                            ERROR("Could not lock the listen mutex", errno);
                        }
                        if ((err = pthread_cond_signal(&listen_cond))) {
                            ERROR("Could not wait for signal condition", errno);
                        }
                        if (pthread_mutex_unlock(&listen_mut)) {
                            ERROR("Could not unlock the listen mutex", errno);
                        }
                    } else {
                        printf("[MIC_TCP] Can only receive SYN packets at the moment: %s\n", __FUNCTION__);
                    }
                    break;
                case ESTABLISHED:
                    #ifdef DEBUG
                    printf("[MIC_TCP] ESTABLISHED\n");
                    #endif
                    if (addr.ip_addr_size != sockets[i].addr.ip_addr_size) {
                        printf("[MIC_TCP] Did not recieve a packet from the right ip adress: %s\n", __FUNCTION__);
                    /* } else if (addr.port != sockets[i].addr.port) { // wrong because client port is different than server port*/
                    /*     printf("[MIC_TCP] Did not recieve a packet from the right port: %s\n", __FUNCTION__); */
                    /*     #ifdef DEBUG */
                    /*     printf("[MIC_TCP] src.port = %d\n[MIC_TCP] dest.port = %d\n", addr.port, sockets[i].addr.port); */
                    /*     #endif */
                    /* } else if (!strncmp(addr.ip_addr, sockets[i].addr.ip_addr, addr.ip_addr_size)) { */
                    /*     printf("[MIC_TCP] Did not recieve a packet from the right ip adress: %s\n", __FUNCTION__); */
                    } else if (!pdu.header.syn && !pdu.header.ack && pdu.header.fin) {
                        ack_num = pdu.header.seq_num;
                        sockets[i].state = CLOSING;
                        send_ack(addr, sockets[i].addr.port, 1);
                        to_send.header.source_port = sockets[i].addr.port;
                        to_send.header.dest_port = addr.port;
                        to_send.header.seq_num = seq_num;
                        to_send.header.ack_num = 0;
                        to_send.header.syn = 0;
                        to_send.header.ack = 0;
                        to_send.header.fin = 1;
                        to_send.payload.data = NULL;
                        to_send.payload.size = 0;
                        if (send_and_wait_for_ack(to_send, addr, NULL, NULL) == -1) {
                            printf("[MIC_TCP] Problem occured during the closing of the connection. Closing without waiting for parnter\n");
                        }
                        sockets[i].state = CLOSED;
                    } else if (!pdu.header.syn && !pdu.header.ack && !pdu.header.fin) {
                        ack_num = pdu.header.seq_num;
                        app_buffer_put(pdu.payload);
                        send_ack(addr, sockets[i].addr.port, 1);
                    } else {
                        printf("[MIC_TCP] packet with unexpected flags was recieved: %s\n", __FUNCTION__);
                    }
                    break;
                default:
                    #ifdef DEBUG
                    printf("[MIC_TCP] in an unspecified state\n");
                    #endif
                    printf("[MIC_TCP] Was not in a state to recieve a packet: %s\n", __FUNCTION__);
                    break;
            }
            socket_found = 1;
            break;
        }
    }
    if (!socket_found) {
        printf("[MIC_TCP] Received PDU to unknown port: %s\n", __FUNCTION__);
    }
}
