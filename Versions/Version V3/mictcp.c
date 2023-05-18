#include <mictcp.h>
#include <api/mictcp_core.h>
#include "mictcp.h"

#define DEBUG

#ifdef DEBUG
#include <time.h>
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
#else
#define TIMESTAMP
#define DEBUG_APPEL_FUNCTION
#define PRINT_HEADER(pdu, send)
#endif

#define ERROR(msg) error(msg, __FUNCTION__, __LINE__)

#define LOSS_RATE    20 // On fixe le pourcentage de perte à 5%
#define MAX_SENDINGS 6


/*Variables globales*/
static mic_tcp_sock      mysocket; /*En vue de la version finale et le multithreading (pour representer plusieurs clients
comme dans la vie reelle) nous utiliserons un tableau de sockets dans les versions suivantes */
static mic_tcp_sock_addr addr_sock_dest;
static int               next_fd    = 0;
static int               num_packet = 0;
static unsigned int      P_Sent     = 0;
static unsigned int      P_Recv     = 0;

static char sliding_window[LOSS_RATE]; // buffer pour prendre compte des packets qui sont bien arrivé
static unsigned int sliding_window_index; // index pour savoir quelle case changer quand un packet arrive

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
#endif

static void print_info(struct mic_tcp_pdu const pdu) {
    printf("\tpdu.header.seq_num = %d\n", pdu.header.seq_num);
    printf("\tpdu.header.ack_num = %d\n", pdu.header.ack_num);
    printf("\tpdu.header.syn = %d\n", pdu.header.syn);
    printf("\tpdu.header.ack = %d\n", pdu.header.ack);
    printf("\tpdu.header.fin = %d\n", pdu.header.fin);
    printf("\tpdu.payload.size = %d\n", pdu.payload.size);
}

static void error(char* msg, const char* function, int line) {
    fprintf(stderr, "Error in function %s at line %d: %s\n", function, line, msg);
}

// vérifie que l'on ne dépasse pas le taux de perte fixé à l'avance.

static int can_accept_loss() {
    int loss = 0;
    for (int i = 0; i < LOSS_RATE; ++i) {
        if (!sliding_window[i]) {
            if (loss) {
                return 0;
            }
            loss = 1;
        }
    }
    return 1;
}

int mic_tcp_socket(start_mode sm)
{
    int result = -1;
    DEBUG_APPEL_FUNCTION;
    for (int i = 0; i < LOSS_RATE; ++i) {
        sliding_window[i] = 1;
    }

    if((result = initialize_components(sm)) == -1){
        return -1;
    }
    else{
        mysocket.fd    = next_fd;
        next_fd++;
        mysocket.state = IDLE;
        set_loss_rate(LOSS_RATE);
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

    if((mysocket.fd == socket) && (mysocket.state != CLOSED)){
        mysocket.state = ESTABLISHED;
        return 0;
    }
    else{
        return -1;
    }
    P_Recv = 0;
    P_Sent = 0;
}

/*
* Permet de réclamer l’établissement d’une connexion
* Retourne 0 si la connexion est établie, et -1 en cas d’échec
*/
int mic_tcp_connect(int socket, mic_tcp_sock_addr addr)
{
    DEBUG_APPEL_FUNCTION;
    if((mysocket.fd == socket)&&(mysocket.state != CLOSED)){
        mysocket.state = ESTABLISHED;
        addr_sock_dest = addr;
        return 0;
    }
    else{
        return -1;
    }
    P_Recv = 0;
    P_Sent = 0;
}

/*
* Permet de réclamer l’envoi d’une donnée applicative
* Retourne la taille des données envoyées, et -1 en cas d'erreur
*/
int mic_tcp_send (int mic_sock, char* mesg, int mesg_size) {
    DEBUG_APPEL_FUNCTION;
    int          size_PDU;
    int          nb_sent = 0;
    mic_tcp_pdu  sent_PDU;
    mic_tcp_pdu  ack;
    unsigned int timeout = 3;   //100 ms time
    int ack_recv = 0;
    char tested_for_possible_loss = 0;

    if (mysocket.state != ESTABLISHED || mysocket.fd != mic_sock) {
        ERROR("Socket number incorrect or connexion not established");
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
    TIMESTAMP;
    PRINT_HEADER(sent_PDU, 1);
    size_PDU = IP_send(sent_PDU,addr_sock_dest);
    printf("Envoi du packet : %d, tentative No : %d.\n", num_packet,nb_sent + 1);
    num_packet++;
    nb_sent++;


    while(!ack_recv){

        if ((IP_recv(&(ack),&addr_sock_dest,timeout) != -1) && (ack.header.ack == 1) && (ack.header.ack_num == P_Sent) ){
            ack_recv = 1;
        }
        if (!ack_recv) {
            if(nb_sent < MAX_SENDINGS){
                sliding_window[sliding_window_index] = 1;
                if (!tested_for_possible_loss && can_accept_loss()) { // La perte est acceptable
                    sliding_window_index = (sliding_window_index + 1) % LOSS_RATE;
                    ++P_Sent;
                    return 0;
                } else {
                    tested_for_possible_loss = 1; // on a déjà teste si la perte est acceptable, cela ne sert à rien de le tester de nouveau
                    PRINT_HEADER(sent_PDU, 1);
                    size_PDU = IP_send(sent_PDU,addr_sock_dest);
                    printf("Renvoi du packet : %d, tentative No : %d.\n",num_packet,nb_sent + 1);
                    nb_sent++;
                }
            } else {
                ERROR("too many failed attempts");
                return -1;
            }
        }
    }

    sliding_window_index = (sliding_window_index + 1) % LOSS_RATE;

    //Sent packets number incrementation
    ++P_Sent;//  = (P_Sent + 1) % 2;
    return size_PDU;
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
    mic_tcp_payload PDU;
    PDU.data = mesg;
    PDU.size = max_mesg_size;

    if ((mysocket.fd == socket)&&(mysocket.state == ESTABLISHED)){
        //Awaiting for a PDU
        mysocket.state = IDLE ;

        //PDU retrieval from the reception buffer
        nb_read_bytes = app_buffer_get(PDU);

        mysocket.state = ESTABLISHED;
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
    TIMESTAMP;
    PRINT_HEADER(pdu, 0);
    DEBUG_APPEL_FUNCTION;

    mic_tcp_pdu ack;

    ack.header.source_port = mysocket.addr.port;
    ack.header.dest_port = addr.port;
    ack.header.seq_num = 0;
    ack.header.ack_num = pdu.header.seq_num;
    ack.header.syn = 0;
    ack.header.ack = 1;
    ack.header.fin = 0;

    ack.payload.size = 0;// No need of DU for an ACK
    ack.payload.data = NULL;

    //ACK Sent
    TIMESTAMP;
    PRINT_HEADER(ack, 1);
    IP_send(ack,addr);

    if (pdu.header.seq_num >= P_Recv){ //Checks if the received PDU has the correct sequence number or if a PDU has been skipped
        app_buffer_put(pdu.payload);
        mysocket.state = ESTABLISHED;
        ++P_Recv;// = (P_Recv + 1) % 2;
    }
}
