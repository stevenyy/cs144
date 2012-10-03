
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <assert.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>

#include "rlib.h"

#define PACKET_MAX_SIZE 512
#define PAYLOAD_MAX_SIZE 500
#define PACKET_HEADER_SIZE 12
#define ACK_PACKET_SIZE 8

enum transmitter_state{
  WAITING_INPUT_DATA, WAITING_ACK, WAITING_EOF_ACK, FINISHED
};

struct reliable_state {
  rel_t *next;			/* Linked list for traversing all connections */
  rel_t **prev;

  conn_t *c;			/* This is the connection object */

  /* Add your own data fields below this */

  /* State for the transmitting piece */
  enum transmitter_state transmitterState;
  uint8_t lastPacketSent[PACKET_MAX_SIZE];
  uint32_t lastAckedSeqNumber;
};

rel_t *rel_list;



/* Helper function declarations */

packet_t *create_packet_from_input (rel_t *relState);
void prepare_for_transmission (packet_t *packet);
void convert_packet_to_network_byte_order (packet_t *packet);
uint16_t compute_checksum (packet_t *packet, int packetLength);
int is_packet_corrupted(packet_t *packet, size_t received_length);
void convert_packet_to_host_byte_order (packet_t *packet); 
void process_received_ack_packet (rel_t *relState, struct ack_packet *packet);
void process_received_data_packet (rel_t *relState, packet_t *packet);
void process_ack (rel_t *relState, packet_t *packet_t);


/* Creates a new reliable protocol session, returns NULL on failure.
 * Exactly one of c and ss should be NULL.  (ss is NULL when called
 * from rlib.c, while c is NULL when this function is called from
 * rel_demux.) */
rel_t *
rel_create (conn_t *c, const struct sockaddr_storage *ss,
	    const struct config_common *cc)
{
  rel_t *r;

  r = xmalloc (sizeof (*r));
  memset (r, 0, sizeof (*r));

  if (!c) {
    c = conn_create (r, ss);
    if (!c) {
      free (r);
      return NULL;
    }
  }

  r->c = c;
  r->next = rel_list;
  r->prev = &rel_list;
  if (rel_list)
    rel_list->prev = &r->next;
  rel_list = r;

  /* Do any other initialization you need here */

  r->transmitterState = WAITING_INPUT_DATA;
  r->lastAckedSeqNumber = 0;

  return r;
}

void
rel_destroy (rel_t *r)
{
  if (r->next)
    r->next->prev = r->prev;
  *r->prev = r->next;
  conn_destroy (r->c);

  /* Free any other allocated memory here */
}


/* This function only gets called when the process is running as a
 * server and must handle connections from multiple clients.  You have
 * to look up the rel_t structure based on the address in the
 * sockaddr_storage passed in.  If this is a new connection (sequence
 * number 1), you will need to allocate a new conn_t using rel_create
 * ().  (Pass rel_create NULL for the conn_t, so it will know to
 * allocate a new connection.)
 */
void
rel_demux (const struct config_common *cc,
	   const struct sockaddr_storage *ss,
	   packet_t *pkt, size_t len)
{
}

void
rel_recvpkt (rel_t *relState, packet_t *packet, size_t received_length)
{
  // IMPLEMENTATION NOTES: cannot assume pkt will persist in memory beyond the function call, its memory is 'borrowed'
  if (is_packet_corrupted (packet, received_length)) /* do not do anything if packet is corrupted */
    return;

  convert_packet_to_host_byte_order (packet); 

  if (packet->len == ACK_PACKET_SIZE)
    process_received_ack_packet (relState, (struct ack_packet*) packet);
  else
    process_received_data_packet (relState, packet);
}


void
rel_read (rel_t *relState)
{
  if (relState->transmitterState == WAITING_INPUT_DATA)
  {
    /* try to read from input and create a packet */
    packet_t *packet = create_packet_from_input (relState);

    /* in case there was data in the input and a packet was created, proceed to process and 
       send the packet */
    if (packet != NULL)
    {
      int packetLength = packet->len;
      memcpy (relState->lastPacketSent, packet, packetLength);

      /* change transmitter state according to whether we are sending EOF packet or normal packet
         (if there is no payload then we are sending EOF packet) */
      relState->transmitterState = (packetLength == PACKET_HEADER_SIZE) ? WAITING_EOF_ACK : WAITING_ACK;
      if (packetLength < PACKET_HEADER_SIZE)
      {
        fprintf (stderr, "Created a malformed packet of length %d inside rel_read. Aborting. \n", packetLength);
        abort ();
      } // TODO: delete this internal check eventually

      prepare_for_transmission (packet);
      conn_sendpkt (relState->c, packet, (size_t) packetLength);

      free (packet);
    }
  }
}

void
rel_output (rel_t *r)
{
}

void
rel_timer ()
{
  /* Retransmit any packets that need to be retransmitted */

}





/********* HELPER FUNCTION SECTION *********/

/* 
  This function is called to read from conn_input, create a packet from that 
  data if any data is available from conn_input, and return it. 

  Notes:
  - The function will try to read data from conn_input, if there is no input
    available (conn_input returns 0) the function will not create a packet and will 
    return NULL.
  - In case a packet is created, this function allocates memory for the packet
    which the caller should free.
  - This function does not compute/write the cksum field of the packet. This 
    should be done when the packet is to be transmitted over UDP only after 
    converting all necessary fields to network byte order. 
*/
packet_t *
create_packet_from_input (rel_t *relState)
{
  packet_t *packet;
  packet = xmalloc (sizeof (*packet));

  /* try to read one full packet's worth of data from input */
  uint16_t bytesRead = conn_input (relState->c, packet->data, PAYLOAD_MAX_SIZE);

  if (bytesRead == 0) /* there is no input, don't create a packet */
  {
    free(packet);
    return NULL;
  }
  else /* there is some input, create a packet */
  {
    /* if we read an EOF create a zero byte payload, otherwise we read normal bytes
       that should be declared in the len field */
    packet->len = (bytesRead == -1) ? (uint16_t) PACKET_HEADER_SIZE : 
                                      (uint16_t) (PACKET_HEADER_SIZE + bytesRead);
  }
  packet->ackno = (uint32_t) 1; // TODO: write appropriate ackno here
  packet->seqno = (uint32_t) (relState->lastAckedSeqNumber + 1);

  return packet;
}

/*
  This function takes a packet and gets it ready for transmission over UDP. 
  Specifically it first converts all necessary fields to network byte order
  and then computes and writes the checksum to the cksum field.
  NOTE: this function works for data packets as well as ack only packets, i.e. packet
  could really be a packet_t* or a struct ack_packet*.
*/
void 
prepare_for_transmission (packet_t *packet)
{
  int packetLength = (int)(packet->len);

  convert_packet_to_network_byte_order (packet);
  packet->cksum = compute_checksum (packet, packetLength);
}

/* 
  This function takes a packet and converts all necessary fields to network byte order.
  NOTE: this function works for data packets as well as ack only packets, i.e. packet
  could really be a packet_t* or a struct ack_packet*.
*/
void 
convert_packet_to_network_byte_order (packet_t *packet)
{
  /* if the packet is a data packet it also has a seqno that has to be converted to 
     network byte order */
  if (packet->len != ACK_PACKET_SIZE) 
    packet->seqno = htonl (packet->seqno);

  packet->len = htons (packet->len);
  packet->ackno = htonl (packet->ackno);  
}

/* 
  Returns the cksum of a packet. Need packetLength as an parameter since the packet's len
  field may already be in network byte order. 
  NOTE: this function works for data packets as well as ack only packets, i.e. packet
  could really be a packet_t* or a struct ack_packet*.
*/
uint16_t 
compute_checksum (packet_t *packet, int packetLength)
{  
  memset (&(packet->cksum), 0, sizeof (packet->cksum)); // TODO: test
  return cksum ((void*)packet, packetLength);
}

/* 
  Function checks if a packet is corrupted by computing its checksum and comparing
  to the checksum in the packet. Returns 1 if packet is corrupted and 0 if it is not. 
  NOTE: this function works for data packets as well as ack only packets, i.e. packet
  could really be a packet_t* or a struct ack_packet*.
*/
int 
is_packet_corrupted(packet_t *packet, size_t received_length)
{
  int packetLength = (int) ntohs (packet->len);

  /* If we received fewer bytes than the packet's size declare corruption. */
  if (received_length < (size_t)packetLength) 
    return 1;

  uint16_t packetChecksum = packet->cksum;
  uint16_t computedChecksum = compute_checksum (packet, packetLength);

  return packetChecksum != computedChecksum;
}

/* 
  This function takes a packet and converts all necessary fields to host byte order.
  NOTE: this function works for data packets as well as ack only packets, i.e. packet
  could really be a packet_t* or a struct ack_packet*.
*/
void 
convert_packet_to_host_byte_order (packet_t *packet)
{
  packet->len = ntohs (packet->len);
  packet->ackno = ntohl (packet->ackno);
  
  /* if the packet is a data packet it additionally has a seqno that has 
     to be converted to host byte order */
  if (packet->len != ACK_PACKET_SIZE) 
    packet->seqno = ntohl (packet->seqno);
}

/*
  This function processes received ack only packets which have passed the corruption check 
*/
void 
process_received_ack_packet (rel_t *relState, struct ack_packet *packet)
{
  process_ack (relState, (packet_t*) packet);
  // /* proceed only if we are waiting for an ack */ 
  // if (relState->transmitterState == WAITING_ACK)
  // {
  //   /* received ack for last normal packet sent, go back to waiting for input 
  //      and try to read */
  //   if (packet->ackno == relState->lastAckedSeqNumber + 1)
  //   {
  //     relState->transmitterState = WAITING_INPUT_DATA;
  //     rel_read(relState);
  //   }
  // }
  // else if (relState->transmitterState == WAITING_EOF_ACK)
  // {
  //   /* received ack for EOF packet, enter closed connection state */
  //   if (packet->ackno == relState->lastAckedSeqNumber + 1)
  //   {
  //     relState->transmitterState = FINISHED;
  //   } 
  // }
  // TODO: clean up above mess
}

void 
process_received_data_packet (rel_t *relState, packet_t *packet)
{
  // TODO: first process data part as the receiver and possibly update ack in relState for 
  //       piggybacking ack to packet sent by transmitter 
  process_ack(relState, packet);
}

void 
process_ack (rel_t *relState, packet_t *packet)
{
  /* proceed only if we are waiting for an ack */ 
  if (relState->transmitterState == WAITING_ACK)
  {
    /* received ack for last normal packet sent, go back to waiting for input 
       and try to read */
    if (packet->ackno == relState->lastAckedSeqNumber + 1)
    {
      relState->transmitterState = WAITING_INPUT_DATA;
      rel_read(relState);
    }
  }
  else if (relState->transmitterState == WAITING_EOF_ACK)
  {
    /* received ack for EOF packet, enter closed connection state */
    if (packet->ackno == relState->lastAckedSeqNumber + 1)
    {
      relState->transmitterState = FINISHED;
    } 
  }
}
