#include <mictcp.h>
#include <api/mictcp_core.h>
#include <pthread.h>
#include "../include/mictcp.h"

#define LOSS_RATE 20
#define MAX_SENDINGS 20
#define SIZE 10 //Taille de la fenêtre glissante

/*Variables globales*/
mic_tcp_sock mysocket; /*En vue de la version finale et le multithreading (pour representer plusieurs clients
comme dans la vie reelle) nous utiliserons un tableau de sockets dans les versions suivantes */
mic_tcp_sock_addr addr_sock_dest;
int next_fd = 0;
int num_packet = 0;
int tab[SIZE] = {1,1,1,1,1,1,1,1,1,1};
int i;
int result = -1;
int max_loss_rate;
int agreed_max_loss_rate;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

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
        mysocket.fd = 0;
        mysocket.state = IDLE;
        if (sm == CLIENT){
            max_loss_rate = 20;
            printf("The maximum loss rate proposed by the client is : %d\n",max_loss_rate);
        }
        else{
            max_loss_rate = 10;
            printf("The maximum loss rate proposed by the server is : %d\n",max_loss_rate);
        }
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

        //WAIT FOR SYN
        mysocket.state = WAIT_FOR_SYN;

        //SYN Reception
        if(pthread_mutex_lock(&lock) != 0){
            perror("Error pthread_mutex_lock");
            exit(EXIT_FAILURE);
        }
        while(mysocket.state == WAIT_FOR_SYN){
            pthread_cond_wait(&cond,&lock);
        }
        if(pthread_mutex_unlock(&lock) != 0){
            perror("Error pthread_mutex_unlock");
            exit(EXIT_FAILURE);
        }

        //SYNACK Construction
        mic_tcp_pdu SYNACK = {{mysocket.addr.port,addr->port,0,max_loss_rate,1,1,0},{"",0}};

        //SYNACK sending
        if (IP_send(SYNACK,*addr) == -1){
            perror("Error IP_Send");
            exit(EXIT_FAILURE);
        }
        mysocket.state = IDLE;

        //WAIT FOR ACK
        if(pthread_mutex_lock(&lock) != 0){
            perror("Error pthread_mutex_lock");
            exit(EXIT_FAILURE);
        }
        while(mysocket.state == IDLE){
            if(pthread_cond_wait(&cond,&lock) != 0){
                perror("Error pthread_cond_wait");
                exit(EXIT_FAILURE);
            }
        }
        if(pthread_mutex_unlock(&lock) != 0){
            perror("Error pthread_mutex_unlock");
            exit(EXIT_FAILURE);
        }

        printf("The negotiation of the maximum authorized loss rate : %d\n", agreed_max_loss_rate);

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

    mic_tcp_pdu SYNACK;
    unsigned long timeout = 100;
    int SYNACK_recieved = 0;
    int nb_attempts = 0;
    int loss_percentage_server;

    if((mysocket.fd == socket)&&(mysocket.state != CLOSED)){

        //SYN Construction
        mic_tcp_pdu SYN = {{mysocket.addr.port,addr.port,0,0,1,0,0},{NULL,0}};

        while(!SYNACK_recieved && (nb_attempts < MAX_SENDINGS)){
            nb_attempts++;

            //Sending SYN
            if(IP_send(SYN,addr) == -1){
                perror("Error IP_send");
                exit(EXIT_FAILURE);
            }

            //WAIT FOR SYNACK
            mysocket.state = WAIT_FOR_SYNACK;

            SYNACK.payload.size = 2*sizeof(short)+2*sizeof(int)+3*sizeof(char);
            SYNACK.payload.data = malloc(SYNACK.payload.size);

            if(IP_recv(&SYNACK,&addr_sock_dest,timeout)==-1){
                printf("SYNACK not recieved \n");
            }
            else{
                if(SYNACK.header.syn == 1 && SYNACK.header.ack == 1){
                    SYNACK_recieved = 1;
                    loss_percentage_server = SYNACK.header.ack_num;

                    //Negotiation of the maximum allowed loss percentage, we keep the lowest percentage
                    if(loss_percentage_server > max_loss_rate){
                        agreed_max_loss_rate = max_loss_rate;
                    }
                    else{
                        agreed_max_loss_rate = loss_percentage_server;
                    }
                    printf("The negotiation of the maximum authorized loss rate : %d\n", agreed_max_loss_rate);

                    //ACKet_loss_rate(LOSS_RATE); Construction
                    mic_tcp_pdu ACK = {{mysocket.addr.port,addr.port,0,agreed_max_loss_rate,0,1,0},{"",0}};

                    //Sending ACK
                    if(IP_send(ACK,addr) == -1){
                        perror("Error IP_send");
                        exit(EXIT_FAILURE);
                    }

                    mysocket.state = ESTABLISHED;
                    printf("ESTABLISHED !\n");

                }
            }
        }

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
    int nb_sent = 0;
    mic_tcp_pdu sent_PDU;
    mic_tcp_pdu ack;
    unsigned int timeout = 100;//100 ms time
    int recieved_PDU = 0;
    int nb_recieved_mesg = 0;
    float losses = 0;
    int i;
    int result = -1;
    int P_Sent = 0;

    if ((mysocket.fd == mic_sock)&&(mysocket.state == ESTABLISHED)){

        //Construction du PDU
            //Header
        sent_PDU.header.source_port = mysocket.addr.port;
        sent_PDU.header.dest_port = addr_sock_dest.port;
        sent_PDU.header.seq_num = P_Sent;
        sent_PDU.header.syn = 0;
        sent_PDU.header.ack = 0;
        sent_PDU.header.fin = 0;

            //Payload
        sent_PDU.payload.data = mesg;
        sent_PDU.payload.size = mesg_size;

        //Envoi du PDU
        size_PDU = IP_send(sent_PDU,addr_sock_dest);
        printf("Envoi du packet : %d, tentative No : %d.\n",num_packet,nb_sent);
        num_packet++;
        nb_sent++;

        //IDLE
        mysocket.state = IDLE;

        //Memory allocation of ACK in order to verify if it is the right one
        ack.payload.size = 2*sizeof(short)+2*sizeof(int)+3*sizeof(char);
        ack.payload.data = malloc(ack.payload.size);

        while(!recieved_PDU){
            if (((result = IP_recv(&(ack),&addr_sock_dest,timeout)) != -1) && (ack.header.ack == 1) && (ack.header.ack_num == P_Sent)){
                recieved_PDU = 1;
                tab[next_fd] = 1;
                next_fd = (next_fd + 1) % SIZE;
            }
            else{
                if(result < 0){
                    printf("Timer expired : no ACK has been recieved !\n");
                    tab[next_fd] = 0;
                    nb_recieved_mesg = 0;
                    for(i=0;i<SIZE;i++){
                        nb_recieved_mesg += tab[i];
                    }
                    losses = (float)(SIZE - nb_recieved_mesg)/(float)SIZE*100.0;
                    printf("Loss percetage = %f\n",losses);
                    if(losses <= agreed_max_loss_rate){
                        next_fd = (next_fd + 1) % SIZE;
                        recieved_PDU = 1;
                        printf("Loss rate acceptable : PDU not sent back\n");
                    }
                    else{
                        if(nb_sent < MAX_SENDINGS){
                            size_PDU = IP_send(sent_PDU,addr_sock_dest);
                            nb_sent++;
                            printf("Renvoi du packet : %d, tentative No : %d.\n",num_packet,nb_sent);
                        }
                        else{
                            perror("Error : too many failed attempts !\n");
                            exit(EXIT_FAILURE);
                        }
                    }
                }
                else{
                    printf("ACK recieved but P_Sent!=P_Recv\n");
                }
            }
        }
        if (result != -1){
            P_Sent = (P_Sent + 1) % 2;
        }
    }
    else{
        perror("Error : Socket number incorrect or connexion not established !\n");
        exit(EXIT_FAILURE);
    }
    mysocket.state = ESTABLISHED;
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
    printf("[MIC-TCP] Appel de la fonction: "); printf(__FUNCTION__); printf("\n");

    int nb_read_bytes = -1;
    mic_tcp_payload PDU;
    PDU.data = mesg;
    PDU.size = max_mesg_size;

    if ((mysocket.fd == socket)&&(mysocket.state == ESTABLISHED)){
        //Awaiting for a PDU
        mysocket.state = WAIT_FOR_PDU;

        //PDU retrieval from the reception buffer
        nb_read_bytes = app_buffer_get(PDU);

        mysocket.state = ESTABLISHED;
    }
    return nb_read_bytes;
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

    mic_tcp_pdu ack;
    int P_Recv = 0;

    //WAIT FOR SYN
    if(mysocket.state == WAIT_FOR_SYN){
        if(pdu.header.syn == 1){
            if(pthread_cond_broadcast(&cond) != 0){
                perror("Error pthread_cond_broadcast");
                exit(EXIT_FAILURE);
            }
            mysocket.state = IDLE;
            if(pthread_mutex_unlock(&lock) != 0){
                perror("Error pthread_mutex_unlock");
                exit(EXIT_FAILURE);
            }
        }
    }
    //WAIT FOR ACK
    else if(mysocket.state == IDLE){
        if(pdu.header.ack == 1){
            agreed_max_loss_rate = pdu.header.ack_num;
            if(pthread_cond_broadcast(&cond) != 0){
                perror("Error pthread_cond_broadcast");
                exit(EXIT_FAILURE);
            }
            mysocket.state = ESTABLISHED;
            printf("ESTABLISHED !\n");
            if(pthread_mutex_unlock(&lock) != 0){
                perror("Error pthread_mutex_unlock");
                exit(EXIT_FAILURE);
            }
        }
    }
    else{
        ack.header.source_port = mysocket.addr.port;
        ack.header.dest_port = addr.port;
        ack.header.ack_num = P_Recv;
        ack.header.syn = 0;
        ack.header.ack = 1;
        ack.header.fin = 0;

        ack.payload.size = 0;// No need of DU for an ACK

        //ACK Sent
        IP_send(ack,addr);

        if (pdu.header.seq_num == P_Recv){ //Checks if the received PDU has the correct sequence number
            app_buffer_put(pdu.payload);
            mysocket.state = ESTABLISHED;
            P_Recv = (P_Recv + 1) % 2;
        }
    }
}
