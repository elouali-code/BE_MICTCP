#include <mictcp.h>
#include <api/mictcp_core.h>
#include "mictcp.h"

#define D 20

/*Variables globales*/
mic_tcp_sock mysocket; /*En vue de la version finale et le multithreading (pour representer plusieurs clients
comme dans la vie reelle) nous utiliserons un tableau de sockets dans les versions suivantes */
mic_tcp_sock_addr addr_sock_dest;
int next_fd = 0;

/*
 * Permet de créer un socket entre l’application et MIC-TCP
 * Retourne le descripteur du socket ou bien -1 en cas d'erreur
 */
int mic_tcp_socket(start_mode sm)
{
   int result = -1;
   printf("[MIC-TCP] Appel de la fonction: ");  printf(__FUNCTION__); printf("\n");

   if((result = initialize_components(sm)) == -1){
       return -1;
   }
   else{
        mysocket.fd = next_fd;
        next_fd++;
        mysocket.state = IDLE;
        set_loss_rate(D);
        return mysocket.fd;
   }
}

/*
 * Permet d’attribuer une adresse à un socket.
 * Retourne 0 si succès, et -1 en cas d’échec
 */
int mic_tcp_bind(int socket, mic_tcp_sock_addr addr)
{
   printf("[MIC-TCP] Appel de la fonction: ");  printf(__FUNCTION__); printf("\n");
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
    printf("[MIC-TCP] Appel de la fonction: ");  printf(__FUNCTION__); printf("\n");

    if((mysocket.fd == socket)&&(mysocket.state != CLOSED)){
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
    printf("[MIC-TCP] Appel de la fonction: ");  printf(__FUNCTION__); printf("\n");
    if((mysocket.fd == socket)&&(mysocket.state != CLOSED)){
        mysocket.state = ESTABLISHED;
        addr_sock_dest = addr;
        return 0;
    }
    else{
        return -1;
    }
}

/*
 * Permet de réclamer l’envoi d’une donnée applicative
 * Retourne la taille des données envoyées, et -1 en cas d'erreur
 */
int mic_tcp_send (int mic_sock, char* mesg, int mesg_size)
{
    printf("[MIC-TCP] Appel de la fonction: "); printf(__FUNCTION__); printf("\n");
    int size_PDU;
    mic_tcp_pdu sent_PDU;

    if ((mysocket.fd == mic_sock)&&(mysocket.state == ESTABLISHED)){

        //Construction du PDU
            //Header
        sent_PDU.header.source_port = mysocket.addr.port;
        sent_PDU.header.dest_port = addr_sock_dest.port;
        sent_PDU.header.syn = 0;
        sent_PDU.header.ack = 0;
        sent_PDU.header.fin = 0;

            //Payload
        sent_PDU.payload.data = mesg;
        sent_PDU.payload.size = mesg_size;

        //Envoi du PDU
        size_PDU = IP_send(sent_PDU,addr_sock_dest);

        return size_PDU;
    }
    else{
        return -1;
    }
}

/*
 * Permet à l’application réceptrice de réclamer la récupération d’une donnée
 * stockée dans les buffers de réception du socket
 * Retourne le nombre d’octets lu ou bien -1 en cas d’erreur
 * NB : cette fonction fait appel à la fonction app_buffer_get()
 */
int mic_tcp_recv (int socket, char* mesg, int max_mesg_size)
{
    printf("[MIC-TCP] Appel de la fonction: "); printf(__FUNCTION__); printf("\n");

    int nb_read_bytes;
    mic_tcp_payload PDU;
    PDU.data = mesg;
    PDU.size = max_mesg_size;

    if ((mysocket.fd == socket)&&(mysocket.state == ESTABLISHED)){
        //Awaiting for a PDU
        mysocket.state = IDLE;

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
    printf("[MIC-TCP] Appel de la fonction :  "); printf(__FUNCTION__); printf("\n");

    if ((mysocket.fd == socket)&&(mysocket.state == ESTABLISHED)){
        mysocket.state = CLOSED;
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
    printf("[MIC-TCP] Appel de la fonction: "); printf(__FUNCTION__); printf("\n");

    app_buffer_put(pdu.payload);
    mysocket.state = ESTABLISHED;
}
