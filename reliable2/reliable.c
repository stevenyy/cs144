
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
  uint16_t payloadSize;
  uint32_t seqno;
  struct timeval lastTransmissionTime;
  int numFlushedBytes; /* for server. how many bytes of packet have been delivered to application */
  
  /* This is a copy of the packet as passed to conn_sendpkt (i.e. in network byte order and with checksum) */
  packet_t packet;
};
typedef struct packet_record packet_record_t;


/* 
  This is a structure for a generic node in a singly linked list. 
  It is used by functions which manipulate generic linked lists for 
  casting (see for example append_to_list). 
  NOTE: for these generic list manipulation functions to work, nodes 
  in the lists must have a 'next' pointer as their first field. 
*/
struct node
{
  struct node *next;
};
typedef struct node node_t;


struct client_state {
  /* state for linked list of packets in flight */ 
  packet_record_t *headPacketsInFlightList;
  packet_record_t *tailPacketsInFlightList; 
  int numPacketsInFlight;
  int isEOFinFlight;
  uint32_t EOFseqno;
  int isPartialInFlight;
  uint32_t partialSeqno;
  int numPartialsInFlight; 
  int windowSize; /* Send Window Size (SWS) */
  uint32_t lastAckedSeqno; /* LAR */
  uint32_t lastSentSeqno; /* LSS; sender must maintain LSS - LAR <= SWS */
  
  /* Buffer in case Nagle forces to buffer a partial packet */
  uint8_t partialPayloadBuffer[PAYLOAD_MAX_SIZE]; /* store only the packet's payload */
  uint16_t bufferLength; /* how many bytes used in the buffer */

  int isFinished; /* has client finished sending data? (i.e. sent and received an ack for EOF packet)*/
};
typedef struct client_state client_state_t; 


struct server_state {
  /* state for linked list of packets in window that were received and are being buffered */ 
  packet_record_t *headReceiveWindowList;
  int windowSize; /* Receive window size (RWS) */
  int numPacketsBuffered;
  int lastAcceptableSeqno; /* end of window */
  int lastReceivedSeqno; /* seqno of the last packet delivered to application */
  int isFinished; /* has server finished receiving data? (i.e. received and outputed an EOF packet)*/
};
typedef struct server_state server_state_t; 


struct reliable_state {
  rel_t *next;         /* Linked list for traversing all connections */
  rel_t **prev;

  conn_t *c;      /* This is the connection object */

  /* Add your own data fields below this */
  int timeout;
  struct sockaddr_storage ss;

  client_state_t clientState;
  server_state_t serverState;
};

rel_t *rel_list;


/* Function declarations */

/* Helper functions for client piece */

packet_t *create_packet (rel_t *relState);
void process_received_ack_packet (rel_t *relState, struct ack_packet *packet);
void handle_retransmission (rel_t *relState);
int get_time_since_last_transmission (packet_record_t *packet_record);
void save_outgoing_data_packet (rel_t *relState, packet_t *packet, int packetLength, uint32_t seqno);

/* Helper functions for server piece */

void process_received_data_packet (rel_t *relState, packet_t *packet);
void process_data_packet (rel_t *relState, packet_t *packet);
void create_and_send_ack_packet (rel_t *relState, uint32_t ackno);
struct ack_packet *create_ack_packet (uint32_t ackno);
void save_incoming_data_packet (rel_t *relState, packet_t *packet);

/* Helper functions shared by client and server pieces */

void prepare_for_transmission (packet_t *packet);
void convert_packet_to_network_byte_order (packet_t *packet);
void convert_packet_to_host_byte_order (packet_t *packet); 
uint16_t compute_checksum (packet_t *packet, int packetLength);
int is_packet_corrupted (packet_t *packet, size_t receivedLength);
void process_ack (rel_t *relState, packet_t *packet_t);


void send_packet (rel_t *relState, packet_t *packet);
void send_EOF_or_full_packet_only (rel_t *relState, packet_t *packet);
int is_partial_packet_in_flight (rel_t *relState);
int is_client_finished (rel_t *relState);
int is_EOF_in_flight (rel_t *relState);
int is_client_window_full (rel_t *relState);
int have_packets_in_flight (rel_t *relState);
packet_record_t *create_sent_packet_record (packet_t *packet, int packetLength, uint32_t seqno);
void save_to_in_flight_list (rel_t *relState, packet_record_t *packetRecord);
void append_to_list (node_t **head, node_t **tail, node_t *newNode);
void update_client_state_on_addition (rel_t *relState, packet_record_t *packetRecord);
void delete_acked_packets (rel_t *relState, uint32_t ackno);
void delete_acked_packets_from_in_flight_list (rel_t *relState, uint32_t ackno);
void update_client_state_on_deletion (rel_t *relState, uint32_t ackno);
int is_valid_ackno (rel_t *relState, uint32_t ackno);
void retransmit_packet_if_necessary (rel_t *relState, packet_record_t *packet_record);
packet_t *create_packet_from_buffer_and_input (rel_t *relState);
packet_t *create_packet_from_input (rel_t *relState);
int havePartialPayloadBuffered (rel_t *relState);
int is_server_finished (rel_t *relState);
int is_receive_window_empty (rel_t *relState);
void flush_receive_window_buffer_to_output (rel_t *relState);
int is_packet_inside_receive_window (rel_t *relState, packet_t *packet);
int is_packet_in_receive_window_buffer (rel_t *relState, packet_t *packet);
packet_record_t * create_received_packet_record (packet_t *packet);
void save_to_receive_window_list (rel_t *relState, packet_record_t *packetRecord);
void update_server_state_on_addition (rel_t *relState, packet_record_t *packetRecord);
void update_server_state_on_deletion (rel_t *relState, packet_record_t *packetRecord);
void insert_in_order_into_received_window_list (rel_t *relState, packet_record_t *packetRecord);
int is_next_packet_to_flush (rel_t *relState, packet_record_t *packetRecord);
void delete_flushed_packet (rel_t *relState, packet_record_t *packetRecord);
void delete_flushed_packet_from_receive_window_list (rel_t *relState, packet_record_t *packetRecord);
int flush_payload_to_output (rel_t *relState, packet_record_t *packetRecord);

rel_t *get_rel_t_from_sockaddr_storage (const struct sockaddr_storage *ss);





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

  if (ss)
    memcpy (&(r->ss), ss, sizeof (*ss));

  /* Client initialization */
  r->clientState.windowSize = cc->window;
  r->clientState.lastAckedSeqno = 0; 
  r->clientState.lastSentSeqno = 0;
  r->clientState.numPacketsInFlight = 0;
  r->clientState.headPacketsInFlightList = NULL;
  r->clientState.tailPacketsInFlightList = NULL;
  r->clientState.isFinished = FALSE;
  r->clientState.isEOFinFlight = FALSE;
  r->clientState.EOFseqno = 0; 
  r->clientState.isPartialInFlight = FALSE;
  r->clientState.partialSeqno = 0; 
  r->clientState.bufferLength = 0;
  
  r->clientState.numPartialsInFlight = 0; 
  
  /* Server initialization */
  r->serverState.headReceiveWindowList = NULL;
  r->serverState.windowSize = cc->window;
  r->serverState.numPacketsBuffered = 0;
  r->serverState.lastAcceptableSeqno = cc->window;
  r->serverState.lastReceivedSeqno = 0;
  r->serverState.isFinished = FALSE;

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
     packet_t *packet, size_t receivedLength)
{
  packet_t *packetCopy;
  packetCopy = xmalloc (sizeof (*packetCopy));
  memcpy (packetCopy, packet, PACKET_MAX_SIZE);

  if (is_packet_corrupted (packetCopy, receivedLength))
  {
    free (packetCopy);
    return; 
  }

  convert_packet_to_host_byte_order (packetCopy); 
  
  rel_t *relState = get_rel_t_from_sockaddr_storage (ss);

  /* we know this connection so just redirect to recvpkt */
  if (relState != NULL)
    rel_recvpkt (relState, packet, receivedLength);
  /* if this is a new connection with seqno 1 then create a relState
   for the connection and process the packet */
  else if (relState == NULL && packetCopy->seqno == 1)
  {
    relState = rel_create (NULL, ss, cc);
    rel_recvpkt (relState, packet, receivedLength);
  }

  free (packetCopy);
}

void
rel_recvpkt (rel_t *relState, packet_t *packet, size_t receivedLength)
{
  if (is_packet_corrupted (packet, receivedLength)) /* do not do anything if packet is corrupted */
  {
    return;
  }
  
  convert_packet_to_host_byte_order (packet); 

  if (packet->len == ACK_PACKET_SIZE)
  {
    process_received_ack_packet (relState, (struct ack_packet*) packet);
  }
  else
  {
    process_received_data_packet (relState, packet);
  }
}


void
rel_read (rel_t *relState)
{
  /* only send packets if the window is not full */
  while (!is_client_window_full (relState))
  {
    /* do not read anything from input if: 1) the client is finished transmitting data, OR 
       2) an EOF packet is in flight. */ 
    if (is_client_finished (relState) || is_EOF_in_flight (relState))
      return;

    packet_t *packet = create_packet (relState);

    /* if packet is NULL then there was no more data available from the input and no packet 
       was allocated. In this case stop sending packets and return. */ 
    if (packet == NULL)
      return;

    /* Otherwise a packet was created, so proceed to process it and try to send. */
    send_packet (relState, packet);
  }
}

/* 
  This functionality belongs to the server piece and is called when there 
  is space available to output a received data packet. 
*/
void
rel_output (rel_t *relState)
{
  /* continue if there was a packet in the receive window 
     that was waiting to be flushed to the output */
  if (!is_server_finished (relState) && !is_receive_window_empty (relState))
    flush_receive_window_buffer_to_output (relState);
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

























/* 
  This function processes a received data packet.
  NOTE: This functionality belongs to the server piece. 
*/ 
void
process_data_packet (rel_t *relState, packet_t *packet)
{

  /* if we receive a packet we have seen and processed before then just send an ack back
     regardless on which state the server is in */
  if (packet->seqno <= relState->serverState.lastReceivedSeqno)
  {
    create_and_send_ack_packet (relState, relState->serverState.lastReceivedSeqno + 1);
  }
  /* if the server has received EOF and is finished don't ack an unseen data packet */
  if (is_server_finished (relState))
  { 
    return;
  }
  /* if we have received a packet inside our receive window that 
     we have not seen/buffered before then save the packet to 
     our receive window list and try to flush the buffered packets
     in the receive window to the output
  */
  if (is_packet_inside_receive_window (relState, packet))
  {
    if (!is_packet_in_receive_window_buffer (relState, packet))
      save_incoming_data_packet (relState, packet);
    
    flush_receive_window_buffer_to_output (relState);
  }
}

int
is_server_finished (rel_t *relState)
{
  return relState->serverState.isFinished;
}

int
is_packet_inside_receive_window (rel_t *relState, packet_t *packet)
{
  return (packet->seqno > relState->serverState.lastReceivedSeqno) 
    && (packet->seqno <= relState->serverState.lastAcceptableSeqno);
}

/* This function returns true if the packet passed in is already being buffered
   by the receive window list the server maintains, otherwise returns false. 
   NOTE: this functionality belongs to the server piece. */
int 
is_packet_in_receive_window_buffer (rel_t *relState, packet_t *packet)
{
  uint32_t seqno = packet->seqno;

  packet_record_t *itr = relState->serverState.headReceiveWindowList;

  while (itr)
  {
    if (itr->seqno == seqno)
      return TRUE;
    itr = itr->next;
  }

  return FALSE;
}

/* 
  This function saves a received data packet into the receive window
  list in case we can not flush it all at once and need to do it as 
  output space becomes available. 
  NOTE: This functionality belongs to the server piece. 
*/
void
save_incoming_data_packet (rel_t *relState, packet_t *packet)
{  
  packet_record_t *packetRecord = create_received_packet_record (packet);
  save_to_receive_window_list (relState, packetRecord);
}

void 
save_to_receive_window_list (rel_t *relState, packet_record_t *packetRecord)
{
  update_server_state_on_addition (relState, packetRecord);
  insert_in_order_into_received_window_list (relState, packetRecord);
}

void
update_server_state_on_addition (rel_t *relState, packet_record_t *packetRecord)
{
  relState->serverState.numPacketsBuffered += 1;
}

void 
insert_in_order_into_received_window_list (rel_t *relState, packet_record_t *packetRecord)
{
  packet_record_t *itr = relState->serverState.headReceiveWindowList;
  packet_record_t **itr2 = &(relState->serverState.headReceiveWindowList);

  while (itr != NULL && itr->seqno < packetRecord->seqno)
  {
    itr2 = &(itr->next);
    itr = itr->next;
  }

  *itr2 = packetRecord;
  packetRecord->next = itr;
}

/*
  The funtion tries to flush the parts of the last received data packet that were
  not previously flushed to the output. It returns 1 if ALL the data in the last
  packet has been flushed to the output and 0 otherwise. 
  NOTE: This funcionality belongs to the server piece. 
*/
void
flush_receive_window_buffer_to_output (rel_t *relState)
{
  packet_record_t *packetItr = relState->serverState.headReceiveWindowList;
  int flushedAnyPackets = FALSE;
  
  while (packetItr != NULL && is_next_packet_to_flush (relState, packetItr))
  {
    if (flush_payload_to_output (relState, packetItr))
    {
      flushedAnyPackets = TRUE;
      packetItr = packetItr->next;
    }
    else
    {
      break;
    }
  }

  if (flushedAnyPackets)
  {
    create_and_send_ack_packet (relState, relState->serverState.lastReceivedSeqno + 1);
    if (is_server_finished (relState))
    {
      if (is_client_finished (relState))
        rel_destroy (relState);
    }
  }
}

int
is_next_packet_to_flush (rel_t *relState, packet_record_t *packetRecord)
{
  return packetRecord->seqno == relState->serverState.lastReceivedSeqno + 1;
}

int
flush_payload_to_output (rel_t *relState, packet_record_t *packetRecord)
{
  if (packetRecord->packetLength == EOF_PACKET_SIZE)
  {
    conn_output (relState->c, NULL, 0);
    delete_flushed_packet (relState, packetRecord);
    return 1;
  }

  size_t bufferSpace = conn_bufspace (relState->c);
  
  if (bufferSpace == 0)
    return 0;

  size_t bytesLeft = packetRecord->payloadSize - packetRecord->numFlushedBytes; /* how many bytes we still have to flush */
  size_t writeLength = (bytesLeft < bufferSpace) ? bytesLeft : bufferSpace;
  char *payload = packetRecord->packet.data;
  uint16_t offset = packetRecord->numFlushedBytes;

  /* try to write writeLength bytes of unflushed data to the output */
  int bytesWritten = conn_output (relState->c, &payload[offset], writeLength);

  packetRecord->numFlushedBytes += bytesWritten;

  if (packetRecord->numFlushedBytes == packetRecord->payloadSize)
  {
    delete_flushed_packet (relState, packetRecord);
    return 1;
  }

  return 0;
}

void 
delete_flushed_packet (rel_t *relState, packet_record_t *packetRecord)
{
  update_server_state_on_deletion (relState, packetRecord);
  delete_flushed_packet_from_receive_window_list (relState, packetRecord);
}

void
update_server_state_on_deletion (rel_t *relState, packet_record_t *packetRecord)
{
  relState->serverState.lastAcceptableSeqno += 1;
  relState->serverState.lastReceivedSeqno += 1;
  relState->serverState.numPacketsBuffered -= 1;
  if (packetRecord->packetLength == EOF_PACKET_SIZE)
    relState->serverState.isFinished = TRUE;
}

void
delete_flushed_packet_from_receive_window_list (rel_t *relState, packet_record_t *packetRecord)
{
  packet_record_t *itr = relState->serverState.headReceiveWindowList;
  packet_record_t **itr2 = &(relState->serverState.headReceiveWindowList);

  while (itr != NULL && itr != packetRecord)
  {
    itr2 = &(itr->next);
    itr = itr->next;
  }

  if (itr == packetRecord)
  {
    *itr2 = packetRecord->next;
    free (packetRecord);
  }
}


/* 
  This function is used to create a record of a packet that was received
  and is inside the receive window. This function creates a packet_record_t 
  struct with a copy of the packet in host byte order. 
  NOTE: this functionality belongs to the server piece. 
  NOTE: this function allocates memory for the packet_record_t, this memory should
  be freed when the packet is flushed and taken off the receive window list.
*/
packet_record_t *
create_received_packet_record (packet_t *packet)
{
  packet_record_t *packetRecord;
  packetRecord = xmalloc (sizeof (*packetRecord));

  packetRecord->next = NULL;
  packetRecord->packetLength = (size_t) packet->len;
  packetRecord->payloadSize = packet->len - PACKET_HEADER_SIZE;
  packetRecord->seqno = packet->seqno;
  packetRecord->numFlushedBytes = 0; /* no bytes has been flushed to application layer yet */
  memcpy (&(packetRecord->packet), packet, packet->len); 

  return packetRecord;
}

int
is_receive_window_empty (rel_t *relState)
{
  return relState->serverState.numPacketsBuffered == 0; 
}






























/* 
  This function checks to see if there are any expired timeouts for unacknowledged packets
  and retransmits accordingly. 
  NOTE: this functionality belongs to the client piece.
*/
void 
handle_retransmission (rel_t *relState)
{
  /* proceed only if we are waiting for an ack (i.e. we have packets in flight) 
     and the client has not finished */ 
  if (!have_packets_in_flight (relState) || is_client_finished (relState))
    return;

  /* iterate over all packets currently in flight and retransmit selectively if their timeout has expired */
  packet_record_t *packet_record_ptr = relState->clientState.headPacketsInFlightList;

  while (packet_record_ptr != NULL)
  {
    retransmit_packet_if_necessary (relState, packet_record_ptr);
    packet_record_ptr = packet_record_ptr->next;
  }
}

/*
  This function takes a packet_record, inspects its last time of transmission and retransmits
  the packet if it has timed out. 
*/
void 
retransmit_packet_if_necessary (rel_t *relState, packet_record_t *packet_record)
{
  int millisecondsSinceLastTransmission = get_time_since_last_transmission (packet_record);

  /* if timeout expired, retransmit last packet*/
  if (millisecondsSinceLastTransmission > relState->timeout) 
  {
    conn_sendpkt (relState->c, &(packet_record->packet), packet_record->packetLength);
    gettimeofday (&(packet_record->lastTransmissionTime), NULL); /* record retransmission time */
  }
}

/*
  This function returns the time interval, in milliseconds, between the time the last packet 
  was transmitted and now. 
*/
int 
get_time_since_last_transmission (packet_record_t *packet_record)
{
  struct timeval now;
  gettimeofday (&now, NULL);
  
  return ( ( (int)now.tv_sec * 1000 + (int)now.tv_usec / 1000 ) - 
  ( (int)packet_record->lastTransmissionTime.tv_sec * 1000 + (int)packet_record->lastTransmissionTime.tv_usec / 1000 ) );
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
  /* proceed only if we are waiting for an ack (i.e. we have packets in flight) 
     and the client has not finished */ 
  if (!have_packets_in_flight (relState) || is_client_finished (relState))
    return;

  /* discard the ack if the ackno is for a packet that: 1) has been previously 
    acked, OR 2) we have not sent, OR 3) not within our window */
  if (!is_valid_ackno (relState, packet->ackno))
    return;

  /* delete acked packets from in-flight packet list and update client state accordingly */
  delete_acked_packets (relState, packet->ackno);

  /* if we received ack for EOF packet and our client is in FINISHED state destroy the connection 
     if the other side's client has finished transmitting. */
  if (is_client_finished (relState))
  {
    if (is_server_finished (relState))
      rel_destroy (relState);
  }

  /* received ack for a non-EOF packet. now there is room in the window for sending packets,
     try to read from input */
  else 
    rel_read (relState);
}

/*
  Function takes an ack number for a received ack packet and determines if the 
  ackno corresponds to packets currently in flight. 
  NOTE: this functionality belongs to the client piece. 
*/
int 
is_valid_ackno (rel_t *relState, uint32_t ackno)
{
  /* ackno is for packets that have previously been acked, so it's not valid */
  if (ackno <= relState->clientState.lastAckedSeqno + 1)
    return FALSE;
  /* ackno is for packets that have not yet been sent, so it's not valid */
  else if (ackno > relState->clientState.lastSentSeqno + 1)
    return FALSE;
  /* the remaining option is that the ackno is for packets we have sent and
     have not yet been acknowledged, so the ackno is valid */
  else 
    return TRUE;
}

/* 
  This function takes a valid ackno for the last acked packet, updates the clientState
  and deletes all acked packets from the in-flight packet list. 
  NOTE: this function belongs to the client piece. 
*/
void 
delete_acked_packets (rel_t *relState, uint32_t ackno)
{
  update_client_state_on_deletion (relState, ackno);
  delete_acked_packets_from_in_flight_list (relState, ackno);
}

/*
  This function updates the state of the client when acked packet_record(s) is/are 
  deleted from the in-flight packet list. It takes an ackno for the last acked 
  packet and updates the clientState fields accordingly, except for linked list pointers.
  NOTE: this function belongs to the client piece. 
*/
void 
update_client_state_on_deletion (rel_t *relState, uint32_t ackno)
{
  /* ackno is the number of next expected packet, so ackno - 1 
     is the seqno of the last acked packet */
  int latestAckedSeqno = ackno - 1; 
  int numPacketsAcked = latestAckedSeqno - relState->clientState.lastAckedSeqno;

  /* slide window */
  relState->clientState.lastAckedSeqno = latestAckedSeqno;
  relState->clientState.numPacketsInFlight -= numPacketsAcked;

  if (is_partial_packet_in_flight (relState))
  {
    /* if partial packet was acked then turn isPartialInFlight flag off. */
    if (relState->clientState.partialSeqno <= latestAckedSeqno)
    {
      relState->clientState.isPartialInFlight = FALSE;
      relState->clientState.numPartialsInFlight -= 1;
    }
  }

  if (is_EOF_in_flight (relState))
  {
    /* received ack for EOF-packet, declare connection finished on client side and turn isEOFinFlight flag off. */
    if (relState->clientState.EOFseqno <= latestAckedSeqno)
    {
      relState->clientState.isEOFinFlight = FALSE;
      relState->clientState.isFinished = TRUE;
    }
  }
}

void 
delete_acked_packets_from_in_flight_list (rel_t *relState, uint32_t ackno)
{
  /* all packets with seqno <= akno - 1 have been acknowledged and must be deleted */
  uint32_t latestAckedSeqno = ackno - 1;

  packet_record_t **head = &(relState->clientState.headPacketsInFlightList);
  packet_record_t **tail = &(relState->clientState.tailPacketsInFlightList);

  /* keep deleting packets whose sequence number is less than or equal to 
     the latestAckedSeqno */
  while ((*head != NULL) && ((*head)->seqno <= latestAckedSeqno))
  {
    /* remove first element fromn linked list */
    packet_record_t *toDelete = *head;
    *head = (*head)->next;

    free(toDelete);

    /* edge case: when we remove the last element we need to set the tail to point to NULL*/
    if (*head == NULL)
      *tail = NULL;
  }
}


/*
  This function takes a packet (with full or partial payload) to be sent, 
  prepares it for transmission, sends it via conn_sendpkt, and saves a record
  in the in-flight packet list.
  NOTE: this funcionatlity belongs to the client piece.  
*/
void
send_packet (rel_t *relState, packet_t *packet)
{
  int packetLength = packet->len;
  uint32_t seqno = packet->seqno;

  /* convert packet to network byte order, compute checksum, and send it */
  prepare_for_transmission (packet);
  conn_sendpkt (relState->c, packet, (size_t) packetLength);

  /* save last packet sent to the window of in flight packets */
  save_outgoing_data_packet (relState, packet, packetLength, seqno);
}

/* 
  This function is call to try to send a packet while we have a partial packet
  in flight. Per Nagle's algorithm we shoud only send a packet if it has a full
  payload or it's an EOF. Otherwise, we buffer the payload and wait until we either 
  get enough data from the input to form a full payload or the partial packet in 
  flight is acked. 
  NOTE: this functionality belongs to the client piece. 
*/
void 
send_EOF_or_full_packet_only (rel_t *relState, packet_t *packet)
{
  /* Only send the packet if it has a full payload, per Nagle's algorithm */ 
  if (packet->len == PACKET_MAX_SIZE || packet->len == EOF_PACKET_SIZE)
    send_packet (relState, packet);

  else /* otherwise we buffer the packet's payload */
  {
    int payloadSize = packet->len - PACKET_HEADER_SIZE; 
    memcpy (relState->clientState.partialPayloadBuffer, packet->data, payloadSize);
    relState->clientState.bufferLength = payloadSize;
  }
}

/* 
  This function is called from rel_read to create a packet. 
  The function will use any buffered partial payload and/or
  data provided by conn_input.
  NOTES: 
  - In case a packet is created, this function returns allocated memory
    for the packet which the caller should free.
  - The packets returned do not have a valid cksum field of the packet. This 
    should be done when the packet is to be transmitted over UDP only after 
    converting all necessary fields to network byte order. 
  - This functionality belongs to the client piece. 
*/
packet_t *
create_packet (rel_t *relState)
{
  return create_packet_from_input (relState);
}

/* 
  This function is called to read from conn_input, create a packet from that 
  data if any data is available from conn_input, and return it. 

  Notes:
  - The function will try to read data from conn_input, if there is no input
    available (conn_input returns 0) the function will not create a packet and will 
    return NULL.
  - This function belongs to the client piece.
*/
packet_t *
create_packet_from_input (rel_t *relState)
{
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
  packet->ackno = (uint32_t) 1; /* not piggybacking acks, don't ack any packets */
  packet->seqno = (uint32_t) (relState->clientState.lastSentSeqno + 1); 

  return packet;  
}


/* 
  This function creates a packet by using buffered partial payload and 
  data from conn_input. 
  NOTE: this functionality belongs to the client piece.
 */
packet_t *
create_packet_from_buffer_and_input (rel_t *relState)
{
  packet_t *packet;
  packet = xmalloc (sizeof (*packet));

  int payloadSize = 0;

  /* copy buffered payload to packet */
  memcpy (packet->data, relState->clientState.partialPayloadBuffer, relState->clientState.bufferLength);
  payloadSize += relState->clientState.bufferLength;

  /* try to fill the remaining free space in the packet's payload from the input */
  size_t numBytesToCopy = PAYLOAD_MAX_SIZE - payloadSize;
  int bytesRead = conn_input (relState->c, packet->data + payloadSize, numBytesToCopy);

  /* if we read EOF disregard it, we first send the buffered data */
  if (bytesRead != -1)
    payloadSize += bytesRead;

  packet->len = (uint16_t) (PACKET_HEADER_SIZE + payloadSize);
  packet->ackno = (uint32_t) 1; /* not piggybacking acks, don't ack any packets */
  packet->seqno = (uint32_t) (relState->clientState.lastSentSeqno + 1);  

  relState->clientState.bufferLength = 0; /* empty buffer since we have used its data in a packet */

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
  packet_record_t *packetRecord = create_sent_packet_record (packet, packetLength, seqno);
  save_to_in_flight_list (relState, packetRecord);
}

int 
is_partial_packet_in_flight (rel_t *relState)
{
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
havePartialPayloadBuffered (rel_t *relState)
{
  return (relState->clientState.bufferLength > 0);
}

int 
is_client_window_full (rel_t *relState)
{
  int numPacketsInFlight = relState->clientState.numPacketsInFlight;
  int windowSize = relState->clientState.windowSize;
  
  if (numPacketsInFlight >= 0 && numPacketsInFlight < windowSize)
    return FALSE;
  else 
    return TRUE;
}

int 
have_packets_in_flight (rel_t *relState)
{
  int numPacketsInFlight = relState->clientState.numPacketsInFlight;
  
  if (numPacketsInFlight == 0)
    return FALSE;
  else 
    return TRUE;
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
create_sent_packet_record (packet_t *packet, int packetLength, uint32_t seqno)
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
  NOTE: this function belongs to the client piece. 
*/
void 
save_to_in_flight_list (rel_t *relState, packet_record_t *packetRecord)
{  
  update_client_state_on_addition (relState, packetRecord);
  append_to_list ((node_t **) &(relState->clientState.headPacketsInFlightList), 
                  (node_t **) &(relState->clientState.tailPacketsInFlightList),
                  (node_t *) packetRecord);
}

/*
  This function updates the state of the client when a packet_record is added to the 
  in-flight packet list. It takes a packet_record_t to be stored in the linked list
  of packets in flight and updates the clientState fields accordingly, except for 
  linked list pointers.
  NOTE: this function belongs to the client piece. 
*/
void 
update_client_state_on_addition (rel_t *relState, packet_record_t *packetRecord)
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
    relState->clientState.isPartialInFlight = TRUE;
    relState->clientState.partialSeqno = packetRecord->seqno;

    relState->clientState.numPartialsInFlight += 1; 
  }
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
    (*tail)->next = newNode; /* point 'next' pointer of last node in the list to newNode */
    *tail = newNode; /* point tail to newNode */
  }
}

rel_t *
get_rel_t_from_sockaddr_storage (const struct sockaddr_storage *ss)
{
  rel_t *itr = rel_list;

  while (itr != NULL)
  {
    if (addreq (&(itr->ss), ss))
      return itr;
    itr = itr->next;
  }

  return NULL;
}