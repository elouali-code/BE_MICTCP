#include <mictcp.h>
#include <api/mictcp_core.h>
/* #include <pthread.h> */
#include "mictcp.h"
#include <errno.h>


// décommentez la prochaine ligne pour n'afficher que les infos vitales
#define DEBUG


// MULTHITHREADING défini par nous-mêmes dans mic_tcp_core.h -> permet de faire des ifdef dans plusieurs fichier pour une transition plus facile entre différentes versions
#ifdef MULTITHREADING
#include <pthread.h>
#endif

#ifdef DEBUG
#include <time.h>
#include <stdio.h>
#endif

#ifdef DEBUG
#define TIMESTAMP            timestamp()
#define DEBUG_APPEL_FUNCTION printf("[MIC-TCP] Appel de la fonction: "); \
    printf(__FUNCTION__);                       \
    printf("\n")
#define PRINT_HEADER(pdu, send) TIMESTAMP;      \
    if (send) {                                 \
        printf("\tpdu sent: \n");               \
    } else {                                    \
        printf("\tpdu recieved: \n");           \
    }                                           \
    print_info(pdu)
#define PRINT_DEBUG printf
#else // redefinir les macros mêmes si elles ne font rien
#define TIMESTAMP
#define DEBUG_APPEL_FUNCTION
#define PRINT_HEADER(pdu, send)
#define PRINT_DEBUG(...)
#endif

// en cas d'erreurs critiques, afficher la où il y a eu une erreure
#define ERROR(message, err) error(message, __FUNCTION__, __LINE__, err)

#define MAX_LOSS_RATE 100
#define LOSS_RATE    0 // On fixe le pourcentage de perte à 5%
#define EFFECTIVE_LOSS_RATE 30
#define MAX_SENDINGS 6
#define TIMEOUT 3

extern int errno;


/*Variables globales*/
static mic_tcp_sock      mysocket; /*En vue de la version finale et le multithreading (pour representer plusieurs clients
comme dans la vie reelle) nous utiliserons un tableau de sockets dans les versions suivantes */
static mic_tcp_sock_addr addr_sock_dest;
static int               next_fd    = 0;
static int               num_packet = 0;
static unsigned int      P_Sent     = 0;
static unsigned int      P_Recv     = 0;

static char sliding_window[MAX_LOSS_RATE]; // buffer pour prendre compte des packets qui sont bien arrivé
static unsigned int sliding_window_index; // index pour savoir quelle case changer quand un packet arrive
static int loss_rate;

#ifdef MULTITHREADING
extern pthread_mutex_t lock;
static pthread_t listen_th;
#endif

static pthread_mutex_t listen_mut = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t listen_cond = PTHREAD_COND_INITIALIZER;
/*
* Permet de créer un socket entre l’application et MIC-TCP
* Retourne le descripteur du socket ou bien -1 en cas d'erreur
*/

#ifdef DEBUG
static void timestamp() {
    time_t rawtime;
    struct tm* timeinfo;
    char   buff[12];
    time(&rawtime);
    timeinfo = localtime(&rawtime);
    strftime(buff, 16, "[%H:%M:%S]", timeinfo);
    puts(buff);
}

static void print_info(struct mic_tcp_pdu const pdu) {
    printf("\tpdu.header.seq_num = %d\n", pdu.header.seq_num);
    printf("\tpdu.header.ack_num = %d\n", pdu.header.ack_num);
    printf("\tpdu.header.syn = %d\n", pdu.header.syn);
    printf("\tpdu.header.ack = %d\n", pdu.header.ack);
    printf("\tpdu.header.fin = %d\n", pdu.header.fin);
    printf("\tpdu.payload.size = %d\n", pdu.payload.size);
}
#endif

static void error(const char* message, const char* function, const int line, const int err) {
    fprintf(stderr, "[MIC_TCP] Error in %s at line %d: %s", function, line, message);
    if (err == -1) {
        fprintf(stderr, "\n");
    } else {
        fprintf(stderr, ": %s\n", strerror(err));
    }
    exit(EXIT_FAILURE);
}

// vérifie que l'on ne dépasse pas le taux de perte fixé à l'avance.

static int can_accept_loss() {
    int loss = 0;
    for (int i = 0; i < MAX_LOSS_RATE; ++i) {
        if (!sliding_window[i]) {
            ++loss;
            if (loss > loss_rate) {
                PRINT_DEBUG("can_accept_loss -> loss unacceptable\n");
                return 0;
            }
        }
    }
    PRINT_DEBUG("can_accept_loss -> loss acceptable\n");
    return 1;
}

// return size of pdu or -1 on error

static int send_and_wait_for_ack(struct mic_tcp_pdu pdu, struct mic_tcp_sock_addr addr) {
    int          size_PDU;
    int          nb_sent = 0;
    int ack_recv = 0;
    char tested_for_possible_loss = 0;
    mic_tcp_pdu  ack;
    TIMESTAMP;
    PRINT_HEADER(pdu, 1);
    size_PDU = IP_send(pdu,addr);
    PRINT_DEBUG("Envoi du packet : %d, tentative No : %d.\n", num_packet,nb_sent + 1);
    num_packet++;
    nb_sent++;


    while(!ack_recv){

        if ((IP_recv(&(ack),&addr,TIMEOUT) != -1) && ack.header.ack && (ack.header.ack_num == P_Sent) ){
            PRINT_HEADER(ack, 0);
            ack_recv = 1;
            sliding_window[sliding_window_index] = 1;
        }
        if (!ack_recv) {
            if(nb_sent < MAX_SENDINGS){
                sliding_window[sliding_window_index] = 0;
                if (!tested_for_possible_loss && can_accept_loss()) { // La perte est acceptable
                    sliding_window_index = (sliding_window_index + 1) % MAX_LOSS_RATE;
                    ++P_Sent;
                    return 0;
                } else {
                    tested_for_possible_loss = 1; // on a déjà teste si la perte est acceptable, cela ne sert à rien de le tester de nouveau
                    PRINT_HEADER(pdu, 1);
                    size_PDU = IP_send(pdu,addr);
                    PRINT_DEBUG("Renvoi du packet : %d, tentative No : %d.\n",num_packet,nb_sent + 1);
                    nb_sent++;
                }
            } else {
                ERROR("too many failed attempts", -1);
                return -1;
            }
        }
    }

    sliding_window_index = (sliding_window_index + 1) % MAX_LOSS_RATE;

    //Sent packets number incrementation
    ++P_Sent;//  = (P_Sent + 1) % 2;
    return size_PDU;

}

#ifdef MULTITHREADING
void* listen_for_pdu(void* args) {

    DEBUG_APPEL_FUNCTION;
    (void) args;
    struct mic_tcp_pdu pdu_tmp;
    struct mic_tcp_sock_addr remote;
    int recv_size;

    pthread_mutex_init(&lock, NULL);

    const int payload_size = 1500 - API_HD_Size;
    pdu_tmp.payload.size = payload_size;
    pdu_tmp.payload.data = malloc(payload_size);

    while (1) {
        pdu_tmp.payload.size = payload_size;
        recv_size = IP_recv(&pdu_tmp, &remote, 0);
        if (recv_size != -1) {
            process_received_PDU(pdu_tmp, remote);
        }
    }
    pthread_exit(NULL);
    return NULL;
}
#endif

int mic_tcp_socket(start_mode sm)
{
    int result = -1;
    DEBUG_APPEL_FUNCTION;
    for (int i = 0; i < MAX_LOSS_RATE; ++i) {
        sliding_window[i] = 1;
    }

    if((result = initialize_components(sm)) == -1){
        return -1;
    }

    else{
        mysocket.fd    = next_fd;
        next_fd++;
        mysocket.state = IDLE;
        set_loss_rate(EFFECTIVE_LOSS_RATE);
        #ifdef MULTITHREADING
        if (sm == SERVER) {
            pthread_create (&listen_th, NULL, listen_for_pdu, "1");
        }
        #endif
        return mysocket.fd;
    }

}

/*
* Permet d’attribuer une adresse à un socket.
* Retourne 0 si succès, et -1 en cas d’échec
*/
int mic_tcp_bind(int socket, mic_tcp_sock_addr addr)
{
    DEBUG_APPEL_FUNCTION;
    if (mysocket.fd == socket){
        memcpy((char *)&mysocket.addr,(char *)&addr, sizeof(mic_tcp_sock_addr));
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
    DEBUG_APPEL_FUNCTION;
    int err;
    (void) addr;
    /* int acceptable_loss_rate_correct = 0; */
    /* int loss_rate_correct = 0; */
    loss_rate = LOSS_RATE;
    /* printf("\n"); */
    /* while (!loss_rate_correct) { */
    /*     printf("Please set the loss rate : "); */
    /*     scanf("%d", &loss_rate); */
    /*     if (loss_rate < 0 || loss_rate > 100) { */
    /*         printf("integer has to be between 0 and 100\n"); */
    /*     } else { */
    /*         loss_rate_correct = 1; */
    /*     } */
    /* } */
    /* fflush(stdin); */
    /* set_loss_rate(loss_rate); */
    /* while (!acceptable_loss_rate_correct) { */
    /*     printf("Please set the acceptable loss rate : "); */
    /*     scanf("%d", &loss_rate); */
    /*     if (loss_rate < 0 || loss_rate > 100) { */
    /*         printf("integer has to be between 0 and 100\n"); */
    /*     } else { */
    /*         acceptable_loss_rate_correct = 1; */
    /*     } */
    /* } */
    /* fflush(stdin); */
    if((mysocket.fd == socket) && (mysocket.state != CLOSED)){
        mysocket.state = IDLE;
        if (pthread_mutex_lock(&listen_mut)) {
            ERROR("Could not lock the listen mutex", errno);
        }
        if ((err = pthread_cond_wait(&listen_cond, &listen_mut))) {
            ERROR("Could not wait for listen condition", errno);
        }
        if (pthread_mutex_unlock(&listen_mut)) {
            ERROR("Could not unlock the listen mutex", errno);
        }
        PRINT_DEBUG("[MIC_TCP] exiting from tcp_accept\n");
        mysocket.state = ESTABLISHED;
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
int mic_tcp_connect(int socket, mic_tcp_sock_addr addr)
{
    struct mic_tcp_pdu pdu;
    int nb_sent = 0;
    int syn_ack_recv = 0;
    int connection_not_established = 1;
    mic_tcp_pdu  syn_ack;
    DEBUG_APPEL_FUNCTION;
    if (mysocket.fd != socket || mysocket.state == CLOSED) {
        return -1;
    }
    addr_sock_dest = addr;
    pdu.header.source_port = mysocket.addr.port;
    pdu.header.dest_port = addr_sock_dest.port;
    pdu.header.seq_num = P_Sent;
    pdu.header.ack_num = 0;
    pdu.header.syn = 1;
    pdu.header.ack = 0;
    pdu.header.fin = 0;
    pdu.payload.data = NULL;
    pdu.payload.size = 0;


    PRINT_HEADER(pdu, 1);
    PRINT_DEBUG("Envoi du packet : %d, tentative No : %d.\n", num_packet,nb_sent + 1);
    IP_send(pdu, addr);
    num_packet++;
    nb_sent++;


    while(!syn_ack_recv){
        if (IP_recv(&syn_ack, &addr, TIMEOUT) != -1) {
            PRINT_HEADER(syn_ack, 0);
            PRINT_DEBUG("Recieved package\n");
            if (syn_ack.header.ack && syn_ack.header.syn) {
                PRINT_DEBUG("Package is syn_ack\n");
                if (syn_ack.header.ack_num == P_Sent) {
                    PRINT_DEBUG("The ack_number is right\n");
                    loss_rate = syn_ack.header.seq_num;
                    /* if (syn_ack.payload.size == 4) { */
                    /*     loss_rate = *syn_ack.payload.data; */
                    /*     set_loss_rate(loss_rate); */
                    /* } else { */
                    /*     ERROR("Unable to negociate loss rate", -1); */
                    /* } */
                    syn_ack_recv = 1;
                }
            }
        } else {
            #ifdef DEBUG
            perror("IP_recv failed");
            #endif
            PRINT_DEBUG("IP_recv failed...\n");
        }
        /* if ((IP_recv(&(syn_ack),&addr,TIMEOUT) != -1) && syn_ack.header.ack && syn_ack.header.syn && (syn_ack.header.ack_num == P_Sent) ){ */
        /*     syn_ack_recv = 1; */
        /* } */
        if (!syn_ack_recv) {
            if(nb_sent < MAX_SENDINGS){
                PRINT_HEADER(pdu, 1);
                PRINT_DEBUG("Renvoi du packet : %d, tentative No : %d.\n",num_packet,nb_sent + 1);
                nb_sent++;
                IP_send(pdu, addr);
            } else {
                ERROR("too many failed attempts", -1);
                return -1;
            }
        }
    }


    //Sent packets number incrementation
    ++P_Sent;//  = (P_Sent + 1) % 2;

    pdu.header.source_port = mysocket.addr.port;
    pdu.header.dest_port = addr_sock_dest.port;
    pdu.header.seq_num = P_Sent;
    pdu.header.ack_num = 0;
    pdu.header.syn = 0;
    pdu.header.ack = 1;
    pdu.header.fin = 0;
    pdu.payload.data = NULL;
    pdu.payload.size = 0;

    PRINT_HEADER(pdu, 1);
    IP_send(pdu, addr);

    nb_sent = 1;
    while (nb_sent < MAX_SENDINGS && connection_not_established) {
        if (IP_recv(&syn_ack, &addr, TIMEOUT) == -1) {
            connection_not_established = 0;
        } else {
            nb_sent++;
            PRINT_HEADER(pdu, 1);
            IP_send(pdu, addr);
        }
    }


    mysocket.state = ESTABLISHED;

    return 0;
}

/*
* Permet de réclamer l’envoi d’une donnée applicative
* Retourne la taille des données envoyées, et -1 en cas d'erreur
*/
int mic_tcp_send (int mic_sock, char* mesg, int mesg_size) {
    DEBUG_APPEL_FUNCTION;
    mic_tcp_pdu  sent_PDU;

    if (mysocket.state != ESTABLISHED || mysocket.fd != mic_sock) {
        ERROR("Socket number incorrect or connexion not established", -1);
        return -1;
    }

    //Construction du PDU
        //Header
    sent_PDU.header.source_port = mysocket.addr.port;
    sent_PDU.header.dest_port = addr_sock_dest.port;
    sent_PDU.header.seq_num = P_Sent;
    sent_PDU.header.ack_num = 0;
    sent_PDU.header.syn = 0;
    sent_PDU.header.ack = 0;
    sent_PDU.header.fin = 0;
        //Payload
    sent_PDU.payload.data = mesg;
    sent_PDU.payload.size = mesg_size;


    //Envoi du PDU
    return send_and_wait_for_ack(sent_PDU, addr_sock_dest);
}

/*
* Permet à l’application réceptrice de réclamer la récupération d’une donnée
* stockée dans les buffers de réception du socket
* Retourne le nombre d’octets lu ou bien -1 en cas d’erreur
* NB : cette fonction fait appel à la fonction app_buffer_get()
*/
int mic_tcp_recv (int socket, char* mesg, int max_mesg_size)
{
    DEBUG_APPEL_FUNCTION;

    int             nb_read_bytes;
    mic_tcp_payload payload;
    payload.data = mesg;
    payload.size = max_mesg_size;

    if ((mysocket.fd == socket)&&(mysocket.state == ESTABLISHED)){
        //Awaiting for a payload

        //payload retrieval from the reception buffer
        nb_read_bytes = app_buffer_get(payload);
        PRINT_DEBUG("connection is correct, trying to retrieve package\n");

        return nb_read_bytes;
    }
    else{
        return -1;
    }
}

/*
* Permet de réclamer la destruction d’un socket.
* Engendre la fermeture de la connexion suivant le modèle de TCP.
* Retourne 0 si tout se passe bien et -1 en cas d'erreur
*/
int mic_tcp_close (int socket)
{
    DEBUG_APPEL_FUNCTION;

    if ((mysocket.fd   == socket)&&(mysocket.state == ESTABLISHED)){
        mysocket.state  = CLOSED;
        return 0;
    }
    else {
        return -1;
    }
}

/*
* Traitement d’un PDU MIC-TCP reçu (mise à jour des numéros de séquence
* et d'acquittement, etc.) puis insère les données utiles du PDU dans
* le buffer de réception du socket. Cette fonction utilise la fonction
* app_buffer_put().
*/

void process_received_PDU(mic_tcp_pdu pdu, mic_tcp_sock_addr addr)
{
    static int nb_sent;
    mic_tcp_pdu to_send;
    int err;

    DEBUG_APPEL_FUNCTION;
    PRINT_HEADER(pdu, 0);
    switch (mysocket.state) {
        case ESTABLISHED:
            PRINT_DEBUG("mysocket.state == ESTABLISHED\n");
            to_send.header.source_port = mysocket.addr.port;
            to_send.header.dest_port = addr.port;
            to_send.header.seq_num = 0;
            to_send.header.ack_num = pdu.header.seq_num;
            to_send.header.syn = 0;
            to_send.header.ack = 1;
            to_send.header.fin = 0;

            to_send.payload.size = 0;// No need of DU for an TO_SEND
            to_send.payload.data = NULL;

            //TO_SEND Sent
            TIMESTAMP;
            PRINT_HEADER(to_send, 1);
            IP_send(to_send,addr);

            if (pdu.header.seq_num != P_Recv){ //Checks if the received PDU has the correct sequence number or if a PDU has been skipped
                app_buffer_put(pdu.payload);
                ++P_Recv;// = (P_Recv + 1) % 2;
            }
            break;
        case IDLE:
            PRINT_DEBUG("mysocket.state == IDLE\n");
            if (pdu.header.syn && !pdu.header.ack && !pdu.header.fin) {
                to_send.header.source_port = mysocket.addr.port;
                to_send.header.dest_port = addr.port;
                to_send.header.seq_num = loss_rate;
                to_send.header.ack_num = pdu.header.seq_num;
                to_send.header.syn = 1;
                to_send.header.ack = 1;
                to_send.header.fin = 0;

                pdu.payload.data = NULL;
                pdu.payload.size = 0;
                /* pdu.payload.data = (char*) &loss_rate; */
                /* pdu.payload.size = 4; */
                PRINT_HEADER(to_send, 1);
                IP_send(to_send,addr);
                mysocket.state = SYN_SENT;
                nb_sent = 1;
            }
            break;
        case SYN_SENT:
            PRINT_DEBUG("mysocket.state == SYN_SENT\n");
            if (!pdu.header.syn && pdu.header.ack && !pdu.header.fin) {
                if (pthread_mutex_lock(&listen_mut)) {
                    ERROR("Could not lock the listen mutex", errno);
                }
                if ((err = pthread_cond_signal(&listen_cond))) {
                    ERROR("Could not wait for signal condition", errno);
                }
                if (pthread_mutex_unlock(&listen_mut)) {
                    ERROR("Could not unlock the listen mutex", errno);
                }
                mysocket.state = ESTABLISHED;
                PRINT_DEBUG("[MIC_TCP] Connection is now established\n");
            } else if (pdu.header.syn && !pdu.header.ack && !pdu.header.fin) {
                if (nb_sent < MAX_SENDINGS) {
                    to_send.header.source_port = mysocket.addr.port;
                    to_send.header.dest_port = addr.port;
                    to_send.header.seq_num = loss_rate;
                    to_send.header.ack_num = pdu.header.seq_num;
                    to_send.header.syn = 1;
                    to_send.header.ack = 1;
                    to_send.header.fin = 0;

                    pdu.payload.data = NULL;
                    pdu.payload.size = 0;
                    PRINT_HEADER(to_send, 1);
                    IP_send(to_send,addr);
                    ++nb_sent;
                }
            } else if (!pdu.header.syn && !pdu.header.ack && !pdu.header.fin) {
                if (pthread_mutex_lock(&listen_mut)) {
                    ERROR("Could not lock the listen mutex", errno);
                }
                if ((err = pthread_cond_signal(&listen_cond))) {
                    ERROR("Could not wait for signal condition", errno);
                }
                if (pthread_mutex_unlock(&listen_mut)) {
                    ERROR("Could not unlock the listen mutex", errno);
                }
                mysocket.state = ESTABLISHED;
                to_send.header.source_port = mysocket.addr.port;
                to_send.header.dest_port = addr.port;
                to_send.header.seq_num = 0;
                to_send.header.ack_num = pdu.header.seq_num;
                to_send.header.syn = 0;
                to_send.header.ack = 1;
                to_send.header.fin = 0;

                to_send.payload.size = 0;// No need of DU for an TO_SEND
                to_send.payload.data = NULL;

                //TO_SEND Sent
                TIMESTAMP;
                PRINT_HEADER(to_send, 1);
                IP_send(to_send,addr);

                if (pdu.header.seq_num != P_Recv){ //Checks if the received PDU has the correct sequence number or if a PDU has been skipped
                    app_buffer_put(pdu.payload);
                    ++P_Recv;// = (P_Recv + 1) % 2;
                }
            }
            break;
        default:
            PRINT_DEBUG("mysocket.state == unknown state\n");
            break;
    }


}
