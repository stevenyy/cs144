
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
#define EOF_PACKET_SIZE 12
#define ACK_PACKET_SIZE 8
#define MILLISECONDS_IN_SECOND 1000
#define MICROSECONDS_IN_MILLISECOND 1000
#define TRUE 1
#define FALSE 0

/* Wrapper struct around packet_t with extra information useful for retransmission. */
struct packet_record {
  /* This pointer must be the first field in order for linked list generic function to work */
  struct packet_record *next; 

  size_t packetLength;
  uint32_t seqno;
  struct timeval lastTransmissionTime;
  
  /* This is a copy of the packet as passed to conn_sendpkt (i.e. in network byte order and with checksum) */
  packet_t packet;
};
typedef struct packet_record packet_record_t;

/* 
  This is a structure for a generic node in a singly linked list. 
  It is used by functions which manipulate generic linked lists for 
  casting (see for example append_to_list). 
  NOTE: for these generic list manipulation functions to work, nodes 
  in the lists must have a 'next' pointer as their first field. */
struct node
{
  struct node *next;
};
typedef struct node node_t;


struct client_state {
  int windowSize; /* SWS */
  uint32_t lastAckedSeqno; /* LAR */
  uint32_t lastSentSeqno; /* LSS */
  /* client must maintain invariant LSS - LAR <= SWS */

  int numPacketsInFlight;
  /* head and tail pointers to linked list of packets in flight */ 
  packet_record_t *headPacketsInFlightList;
  packet_record_t *tailPacketsInFlightList; 
  
  int isFinished; /* has client finished sending data? (i.e. sent and received an ack for EOF packet)*/

  /* Extra state for convenience */
  int isEOFinFlight;
  uint32_t EOFseqno;
  int isPartialInFlight;

  // for debugging
  int numPartialsInFlight; // TODO: delete for submission
};
typedef struct client_state client_state_t; 

enum server_state{
  WAITING_DATA_PACKET, WAITING_TO_FLUSH_DATA, SERVER_FINISHED
};



struct reliable_state {
  rel_t *next;			/* Linked list for traversing all connections */
  rel_t **prev;

  conn_t *c;			/* This is the connection object */

  /* Add your own data fields below this */
  int timeout;

  /* State for the client */
  client_state_t clientState;
  
  /* State for the server */
  enum server_state serverState;
  uint32_t nextInOrderSeqNo; /* The sequence number of the next in-order packet we expect to receive. */
  uint8_t lastReceivedPacketPayload[PAYLOAD_MAX_SIZE]; /* buffer for the last received packet's payload */
  uint32_t lastReceivedPacketSeqno;
  uint16_t lastReceivedPayloadSize; /* size of the last received packet's payload */
  uint16_t numFlushedBytes; /* number of bytes of lastReceivedPacketPayload that have been flushed out to conn_output  */
};

rel_t *rel_list;


/* Function declarations */

/* Helper functions for client piece */

packet_t *create_packet_from_input (rel_t *relState);
void process_received_ack_packet (rel_t *relState, struct ack_packet *packet);
void handle_retransmission (rel_t *relState);
int get_time_since_last_transmission (rel_t *relState);
void save_outgoing_data_packet (rel_t *relState, packet_t *packet, int packetLength, uint32_t seqno);

/* Helper functions for server piece */

void process_received_data_packet (rel_t *relState, packet_t *packet);
void process_data_packet (rel_t *relState, packet_t *packet);
void create_and_send_ack_packet (rel_t *relState, uint32_t ackno);
struct ack_packet *create_ack_packet (uint32_t ackno);
void save_incoming_data_packet (rel_t *relState, packet_t *packet);
int flush_payload_to_output (rel_t *relState);

/* Helper functions shared by client and server pieces */

void prepare_for_transmission (packet_t *packet);
void convert_packet_to_network_byte_order (packet_t *packet);
void convert_packet_to_host_byte_order (packet_t *packet); 
uint16_t compute_checksum (packet_t *packet, int packetLength);
int is_packet_corrupted (packet_t *packet, size_t receivedLength);
void process_ack (rel_t *relState, packet_t *packet_t);



/* TODO: remove next comment. */
/* new helper functions for lab2 */
void send_full_or_partial_packet (rel_t *relState, packet_t *packet);
void send_full_packet_only (rel_t *relState, packet_t *packet);
int is_partial_packet_in_flight (rel_t *relState);
int is_client_finished (rel_t *relState);
int is_EOF_in_flight (rel_t *relState);
int is_window_full (rel_t *relState);
packet_record_t *create_packet_record (packet_t *packet, int packetLength, uint32_t seqno);
void save_to_in_flight_list (rel_t *relState, packet_record_t *packetRecord);
void append_to_list (node_t **head, node_t **tail, node_t *newNode);
void update_client_state (rel_t *relState, packet_record_t *packetRecord);

// TODO delete function for submission
void abort_if (int expression, char *msg);




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

  r->timeout = cc->timeout;

  /* Client initialization */
  r->clientState.windowSize = cc->window;
  r->clientState.lastAckedSeqno = 0; // TODO/BUG_RISK: check this initializations
  r->clientState.lastSentSeqno = 0;
  r->clientState.numPacketsInFlight = 0;
  r->clientState.headPacketsInFlightList = NULL;
  r->clientState.tailPacketsInFlightList = NULL;
  r->clientState.isFinished = FALSE;
  r->clientState.isEOFinFlight = FALSE;
  r->clientState.EOFseqno = 0; // TODO: this is semantically incorrect but it should be OK
  r->clientState.isPartialInFlight = FALSE;

  r->clientState.numPartialsInFlight = 0; // TODO: delete for submission
  
  /* Server initialization */
  r->serverState = WAITING_DATA_PACKET;
  r->nextInOrderSeqNo = 1;
  r->numFlushedBytes = 0;
  r->lastReceivedPayloadSize = 0;
  r->lastReceivedPacketSeqno = 0;

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
  free (r);
  // TODO: free memory allocated to packets in flight from client window 
  // TODO: free memory allocated to buffered packets in server window 
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
rel_recvpkt (rel_t *relState, packet_t *packet, size_t receivedLength)
{
  if (is_packet_corrupted (packet, receivedLength)) /* do not do anything if packet is corrupted */
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
  /* do not read anything from input if: 1) the client is finished transmitting data, OR 
     2) an EOF packet is in flight. */ 
  if (is_client_finished (relState) || is_EOF_in_flight (relState))
    return;

  /* the window is not full, so there is room to send packets */
  while (!is_window_full (relState))
  {
    /* try to read from input and create a packet */
    packet_t *packet = create_packet_from_input (relState);

    /* if packet is NULL then there was no more data available from the input and no packet 
       was allocated. In this case stop sending packets and return. */ 
    if (packet == NULL)
      return;

    // TODO: delete when implementing Nagle
    send_full_or_partial_packet (relState, packet);
    
    /* Otherwise a packet was created, so proceed to process it and try to send. */

    /* Case 1: all packets in flight carry full payload. 
       In this case we can send either a packet with full or partial payload if there 
       is any input data available. */
       // TODO: uncomment when Nagle is implemented
    // if (!is_partial_packet_in_flight (relState))
    //   send_full_or_partial_packet (relState, packet);
    
    /* Case 2: there is a partially filled packet in flight.
       In this case we can only send a packet if it has a full payload. If not enough 
       data is available from the input we will buffer the partial payload until either
       the partial packet in flight is acked or we get enough data from the input to form 
       a full packet. */
       // TODO: uncomment when Nagle is implemented. 
    // else
    //   send_full_packet_only (relState, packet);

    free (packet);    
  }
}

/* 
  This functionality belongs to the server piece and is called when there 
  is space available to output a received data packet. 
*/
void
rel_output (rel_t *relState)
{
  /* continue if there was a packet that was waiting to be flushed to the output */
  if (relState->serverState == WAITING_TO_FLUSH_DATA)
  {
    if (flush_payload_to_output (relState))
    {
      /* send ack back only after flushing ALL the packet */
      create_and_send_ack_packet (relState, relState->lastReceivedPacketSeqno + 1);
      relState->nextInOrderSeqNo = relState->lastReceivedPacketSeqno + 1;
      relState->serverState = WAITING_DATA_PACKET;
    }
  }
}

void
rel_timer ()
{
  /* Retransmit any packets that need to be retransmitted */
  rel_t *r = rel_list;

  /* go through every open reliable connection and retransmit packets as needed */ 
  while (r)
  {
    handle_retransmission (r);
    r = r->next;
  }
}





/********* HELPER FUNCTION SECTION *********/

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
  memset (&(packet->cksum), 0, sizeof (packet->cksum));
  return cksum ((void*)packet, packetLength);
}

/* 
  Function checks if a packet is corrupted by computing its checksum and comparing
  to the checksum in the packet. Returns 1 if packet is corrupted and 0 if it is not. 
  NOTE: this function works for data packets as well as ack only packets, i.e. packet
  could really be a packet_t* or a struct ack_packet*.
*/
int 
is_packet_corrupted (packet_t *packet, size_t receivedLength)
{
  int packetLength = (int) ntohs (packet->len);

  /* If we received fewer bytes than the packet's size declare corruption. */
  if (receivedLength < (size_t)packetLength) 
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

void 
process_received_ack_packet (rel_t *relState, struct ack_packet *packet)
{
  process_ack (relState, (packet_t*) packet);
}

void 
process_received_data_packet (rel_t *relState, packet_t *packet)
{
  /* Server piece should process the data part of the packet and client piece
     should process part the ack part of the packet. */  

  /* Pass the packet to the server piece to process the data packet */
  process_data_packet (relState, packet);

  /* Pass the packet to the client piece to process the ackno field */
  process_ack (relState, packet);
}

/*
  This function processes received ack only packets which have passed the corruption check. 
  NOTE: this function works for data packets as well as ack only packets, i.e. packet
  could really be a packet_t* or a struct ack_packet*.
  NOTE: This functionality belongs to the client piece.  
*/
void 
process_ack (rel_t *relState, packet_t *packet)
{
  /* proceed only if we are waiting for an ack */ 
  if (relState->clientState == WAITING_ACK)
  {
    /* received ack for last normal packet sent, go back to waiting for input 
       and try to read */
    if (packet->ackno == relState->seqnoLastPacketSent + 1)
    {
      relState->clientState = WAITING_INPUT_DATA;
      rel_read (relState);
    }
  }
  else if (relState->clientState == WAITING_EOF_ACK)
  {
    /* received ack for EOF packet, enter declare client connection to be finished */
    if (packet->ackno == relState->seqnoLastPacketSent + 1)
    {
      relState->clientState = CLIENT_FINISHED;

      /* destroy the connection only if the other side's client has finished transmitting */
      if (relState->serverState == SERVER_FINISHED)
        rel_destroy (relState);
    } 
  }
}

/* 
  This function processes a data packet.
  NOTE: This functionality belongs to the server piece. 
*/ 
void
process_data_packet (rel_t *relState, packet_t *packet)
{
  /* if we receive a packet we have seen and processed before then just send an ack back
     regardless on which state the server is in */
  if (packet->seqno < relState->nextInOrderSeqNo)
    create_and_send_ack_packet (relState, packet->seqno + 1);

  /* if we have received the next in-order packet we were expecting and we are waiting 
     for data packets process the packet */
  if ( (packet->seqno == relState->nextInOrderSeqNo) && (relState->serverState == WAITING_DATA_PACKET) )
  {
    /* if we received an EOF packet signal to conn_output and destroy the connection if appropriate */
    if (packet->len == EOF_PACKET_SIZE)
    {
      conn_output (relState->c, NULL, 0);
      relState->serverState = SERVER_FINISHED;
      create_and_send_ack_packet (relState, packet->seqno + 1);

      /* destroy the connection only if our client has finished transmitting */
      if (relState->clientState == CLIENT_FINISHED)
        rel_destroy (relState);      
    }
    /* we receive a non-EOF data packet, so try to flush it to conn_output */
    else
    {
      save_incoming_data_packet (relState, packet);
      
      if (flush_payload_to_output (relState))
      {
        create_and_send_ack_packet (relState, packet->seqno + 1);
        relState->nextInOrderSeqNo = packet->seqno + 1;
      }
      else
      {
        relState->serverState = WAITING_TO_FLUSH_DATA;
      }
    }
  }
}

/* 
  This function saves a received data packet in case we can not flush it all
  at once and need to do it as output space becomes available. 
  NOTE: This functionality belongs to the server piece. 
*/
void
save_incoming_data_packet (rel_t *relState, packet_t *packet)
{  
  uint16_t payloadSize = packet->len - PACKET_HEADER_SIZE;

  memcpy (&(relState->lastReceivedPacketPayload), &(packet->data), payloadSize);
  relState->lastReceivedPayloadSize = payloadSize;
  relState->lastReceivedPacketSeqno = packet->seqno;
  relState->numFlushedBytes = 0;
}

/*
  The funtion tries to flush the parts of the last received data packet that were
  not previously flushed to the output. It returns 1 if ALL the data in the last
  packet has been flushed to the output and 0 otherwise. 
  NOTE: This funcionality belongs to the server piece.
*/
int
flush_payload_to_output (rel_t *relState)
{
  size_t bufferSpace = conn_bufspace (relState->c);
  
  if (bufferSpace == 0)
    return 0;

  size_t bytesLeft = relState->lastReceivedPayloadSize - relState->numFlushedBytes; /* how many bytes we still have to flush */
  size_t writeLength = (bytesLeft < bufferSpace) ? bytesLeft : bufferSpace;
  uint8_t *payload = relState->lastReceivedPacketPayload;
  uint16_t offset = relState->numFlushedBytes;

  /* try to write writeLength bytes of unflushed data to the output */
  int bytesWritten = conn_output (relState->c, &payload[offset], writeLength);

  relState->numFlushedBytes += bytesWritten;

  if (relState->numFlushedBytes == relState->lastReceivedPayloadSize)
    return 1;

  return 0;
}

/* 
  This function checks to see if there are any expired timeouts for unacknowledged packets
  and retransmits accordingly. 
*/
void 
handle_retransmission (rel_t *relState)
{
  /* Only retransmit if we are waiting for acks */
  if (relState->clientState == WAITING_ACK || relState->clientState == WAITING_EOF_ACK)
  {
    int millisecondsSinceTransmission = get_time_since_last_transmission (relState);

    /* last transmission timed out, retransmit last packet*/
    if (millisecondsSinceTransmission > relState->timeout) 
    {
      conn_sendpkt (relState->c, &(relState->lastPacketSent), relState->lengthLastPacketSent);
      gettimeofday (&(relState->lastTransmissionTime), NULL); /* record retransmission time */
    }
  }
}

/*
  This function returns the time interval, in milliseconds, between the time the last packet 
  was transmitted and now. 
*/
int 
get_time_since_last_transmission (rel_t *relState)
{
  struct timeval now;
  gettimeofday (&now, NULL);
  
  return ( ( (int)now.tv_sec * 1000 + (int)now.tv_usec / 1000 ) - 
  ( (int)relState->lastTransmissionTime.tv_sec * 1000 + (int)relState->lastTransmissionTime.tv_usec / 1000 ) );
}

void
create_and_send_ack_packet (rel_t *relState, uint32_t ackno)
{
  struct ack_packet *ackPacket = create_ack_packet (ackno);
  int packetLength = ackPacket->len;
  prepare_for_transmission ((packet_t*)ackPacket);
  conn_sendpkt (relState->c, (packet_t*)ackPacket, (size_t) packetLength);
  free (ackPacket);
}

struct ack_packet *
create_ack_packet (uint32_t ackno)
{
  struct ack_packet *ackPacket;
  ackPacket = xmalloc (sizeof (*ackPacket));

  ackPacket->len = (uint16_t) ACK_PACKET_SIZE;
  ackPacket->ackno = ackno;
  
  return ackPacket;
}




























// TODO comment
void
send_full_or_partial_packet (rel_t *relState, packet_t *packet)
{
  int packetLength = packet->len;
  uint32_t seqno = packet->seqno;

  /* convert packet to network byte order, compute checksum, and send it */
  prepare_for_transmission (packet);
  conn_sendpkt (relState->c, packet, (size_t) packetLength);

  /* save last packet sent to the window of in flight packets */
  save_outgoing_data_packet (relState, packet, packetLength, seqno);
}

// TODO comment
void 
send_full_packet_only (rel_t *relState, packet_t *packet)
{
  // TODO: implement
}

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
  // BUG_RISK 
  packet_t *packet;
  packet = xmalloc (sizeof (*packet));

  /* try to read one full packet's worth of data from input */
  int bytesRead = conn_input (relState->c, packet->data, PAYLOAD_MAX_SIZE);

  if (bytesRead == 0) /* there is no input, don't create a packet */
  {
    free (packet);
    return NULL;
  }
  /* else there is some input, so create a packet */

  /* if we read an EOF create a zero byte payload, otherwise we read normal bytes
     that should be declared in the len field */
  packet->len = (bytesRead == -1) ? (uint16_t) PACKET_HEADER_SIZE : 
                                    (uint16_t) (PACKET_HEADER_SIZE + bytesRead);
  packet->ackno = (uint32_t) 0; /* not piggybacking acks, don't ack any packets */
  packet->seqno = (uint32_t) (relState->clientState.lastSentSeqno + 1); // BUG_RISK +- 1 error

  return packet;
}


/* 
  Save a copy of the last packet sent to the list of in-flight packets in case 
  we need to retransmit. Note that the caller must provide a pointer to a packet
  which has already been prepared for transmission, i.e. neccesary fields
  are already in network byte order, as well as its length and sequence number.
  NOTE: This funtionality belongs to the client piece.
*/ 
void 
save_outgoing_data_packet (rel_t *relState, packet_t *packet, int packetLength, uint32_t seqno)
{
  packet_record_t *packetRecord = create_packet_record (packet, packetLength, seqno);
  save_to_in_flight_list (relState, packetRecord);
}

int 
is_partial_packet_in_flight (rel_t *relState)
{
  // TODO: delete for submission
  abort_if(relState->clientState.numPartialsInFlight > 1 || relState->clientState.numPartialsInFlight < 0, "in is_partial_packet_in_flight: more than 1 packet in flight.")
  return relState->clientState.isPartialInFlight;
}

int 
is_client_finished (rel_t *relState)
{
  return relState->clientState.isFinished;
} 

int 
is_EOF_in_flight (rel_t *relState)
{
  return relState->clientState.isEOFinFlight;
}

int 
is_window_full (rel_t *relState)
{
  int numPacketsInFlight = relState->clientState.numPacketsInFlight;
  int windowSize = relState->clientState.windowSize;
  
  // TODO clean up after testing & before submission
  if (numPacketsInFlight < windowSize && numPacketsInFlight >= 0)
    return 0;
  else if (numPacketsInFlight == windowSize)
    return 1;
  else abort_if (TRUE, "in is_window_full: numPacketsInFlight < 0 or > windowSize");
}

/* 
  This function is used to create a record of a packet that was sent with 
  conn_sendpkt and is in flight. This function creates a packet_record_t 
  struct with a copy of the packet in network byte order. 
  NOTE: this functionality belongs to the client piece. 
  NOTE: this function allocates memory for the packet_record_t, this memory should
  be freed when the packet is acked and taken off packet-in-flight list.
*/
packet_record_t *
create_packet_record (packet_t *packet, int packetLength, uint32_t seqno)
{
  packet_record_t *packetRecord;
  packetRecord = xmalloc (sizeof (*packetRecord));

  memcpy (&(packetRecord->packet), packet, packetLength); 
  packetRecord->packetLength = (size_t) packetLength;
  packetRecord->seqno = seqno;
  gettimeofday (&(packetRecord->lastTransmissionTime), NULL); /* record the time of transmission */

  return packetRecord;
}

/*
  This function takes in a packet_record for a packet that has just been
  sent, updates the clientState accordingly and appends it to the linked
  list of packets in flight.  
*/
void 
save_to_in_flight_list (rel_t *relState, packet_record_t *packetRecord)
{  
  update_client_state (relState, packetRecord);
  append_to_list ((node_t **) &(relState->clientState.headPacketsInFlightList), 
                  (node_t **) &(relState->clientState.tailPacketsInFlightList),
                  (node_t *) packetRecord);
}

/*
  This function takes a packet_record_t to be stored in the linked list of packets
  in flight and updates the clientState fields accordingly, except for linked list pointers.
  NOTE: this function belongs to the client piece. 
  // TODO: update_client_state is also a name for the function that updates client
  state if a node is deleted. change name or incorporate functionality. 
*/
void 
update_client_state (rel_t *relState, packet_record_t *packetRecord)
{
  int packetLength = packetRecord->packetLength;

  relState->clientState.lastSentSeqno = packetRecord->seqno;
  relState->clientState.numPacketsInFlight += 1;
  if (packetLength == EOF_PACKET_SIZE)
  {
    relState->clientState.isEOFinFlight = TRUE;
    relState->clientState.EOFseqno = packetRecord->seqno;
  }
  else if (packetLength > EOF_PACKET_SIZE && packetLength < PACKET_MAX_SIZE)
  {
    relState->clientState.isEOFinFlight = FALSE;
    relState->clientState.isPartialInFlight = TRUE;

    relState->clientState.numPartialsInFlight += 1; // TODO: delete for submission
  }

  // TODO delete for submission
  abort_if(packetLength < EOF_PACKET_SIZE || packetLength > PACKET_MAX_SIZE, "In update_client_state, packet with wrong length");
}

/*
  This function appends a node to a singly linked list. Note that this is a generic
  function since all pointers are casted to node_t * and node_t **. This means 
  that in order to use this function the nodes in the linked list must have 
  a next pointer as the first field (see comment on top of struct node declaration). 
*/
void 
append_to_list (node_t **head, node_t **tail, node_t *newNode)
{
  newNode->next = NULL;

  /* case where list is empty */
  if (*head == NULL && *tail == NULL)
  {
    *head = newNode;
    *tail = newNode;
  }
  /* case where list is non-empty */
  else
  {  
    // BUG_RISK
    (*tail)->next = newNode; /* point 'next' pointer of last node in the list to newNode */
    *tail = newNode /* point tail to newNode */
  }
}

// TODO: delete for submission
void 
abort_if (int expression, char *msg)
{
  if (expression)
  {
    fprintf (stderr, msg);
    abort ();
  }  
}