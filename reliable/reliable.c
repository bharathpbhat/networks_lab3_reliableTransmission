
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdbool.h>
#include <assert.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>

#include "rlib.h"

#define DATA_CHUNK_SIZE 500
#define DATA_PACKET_MINSIZE 12
#define PACKET_MINSIZE 8
#define FIN_WAIT_MULTIPLIER 2
#define COMPLETE_PACKET_SIZE 512

/* structure to hold a packet sent but not yet acked */
struct in_flight_packet {
  packet_t *packet;
  struct timeval sent_time;
};
typedef struct in_flight_packet in_flight_packet_t;

/* linked list of packets in flight */
struct in_flight_packet_buf {
  in_flight_packet_t *current;
  struct in_flight_packet_buf *next;  
};
typedef struct in_flight_packet_buf in_flight_packet_buf_t;

/* linked list of packets */
struct packet_buf {
  packet_t *packet;
  struct packet_buf *next;
};
typedef struct packet_buf packet_buf_t;

/* a buffer to hold data (stdin / stdout) */
struct data_buf {
  char buf[DATA_CHUNK_SIZE];
  uint32_t valid_bytes;
};
typedef struct data_buf input_buf_t;
typedef struct data_buf output_buf_t;

/**
  Structure to maintain send state variables of a two-way connection.
  Holds following information:
  
  in_flight_buf: linked list of packets currently in flight, maintained in order of seqno, head pointing to the lowest seqno in flight.
  waiting_buf: linked list of packets waiting to be sent, head points to the first packet that will be sent next
  in_buf: buffer for data from stdin not yet formed into a packet
  last_ack: packets up to this sequence number have been acked
  last_sent: packets up to this sequence number have been sent
  partial_packet_out: set to true if partial packet is in flight
  partial_packet_seqno: sequence number of the partial packet in flight. Valid only when 'partial_packet_out' is set
  terminate_connection: set to true if EOF received from stdin
  terminate_time: time when 'terminate_connection' was set to true (Used to implement FIN_WAIT)
  config: connection configuration (window size, timeout ...)
 
 */
struct send_state {
  in_flight_packet_buf_t *in_flight_buf;
  packet_buf_t *waiting_buf;
  input_buf_t *in_buf;
  
  uint32_t last_ack;
  uint32_t last_sent;
  uint32_t current_num;
  bool partial_packet_out;
  uint32_t partial_packet_seqno;
  bool terminate_connection;
  struct timeval terminate_time;
  const struct config_common *config;
};
typedef struct send_state send_state_t;

/**
  Utility function to print send state variables for debugging
  */
void print_send_state(send_state_t *s)
{
  fprintf(stderr, "last sent %d\n",s->last_sent);
  fprintf(stderr, "last acked %d\n",s->last_ack);
  fprintf(stderr, "next packet number %d\n",s->current_num);
  fprintf(stderr, "Partial packet out: %d\n", s->partial_packet_out);
  fprintf(stderr, "partial packet seqno: %d\n",s->partial_packet_seqno);
  fprintf(stderr, "Terminate condition set: %d\n",s->terminate_connection);
  fprintf(stderr, "Window size: %d\n",s->config->window);
  int num_in_flight = 0;
  in_flight_packet_buf_t *in_buf = s->in_flight_buf;
  while(in_buf)
  {
    num_in_flight += 1;
	in_buf = in_buf->next;
  }
  fprintf(stderr, "Number of packets in flight: %d\n",num_in_flight);
  int num_waiting = 0;
  packet_buf_t *buf = s->waiting_buf;
  while(buf)
  {
    num_waiting += 1;
	buf = buf->next;
  }
  fprintf(stderr, "Number of packets waiting: %d\n",num_waiting);
  fprintf(stderr, "Number of bytes in input buffer: %d\n",s->in_buf->valid_bytes);
}

/**
  Structure to maintain receive state information of a two-way connection. 
  Holds following information:

  buf: linked list of packets received, but not yet acked, maintained in increasing order of sequence numbers
  out_buf: Buffer to hold bytes waiting to be output to stdout (when waiting for conn_bufspace to clear)
  last_ack: last acknowledgement number sent
  terminate_connection: set to true when EOF packet received from other side
  terminate_time: time when EOF packet was received from other side (Used to implement FIN_WAIT)
  config: connection configuration (window, timeout...) 

  */
struct receive_state {
  packet_buf_t *buf;
  output_buf_t *out_buf;
  uint32_t last_ack;
  bool terminate_connection;
  struct timeval terminate_time;
  const struct config_common *config;
};
typedef struct receive_state receive_state_t;

struct reliable_state {
  rel_t *next;			/* Linked list for traversing all connections */
  rel_t **prev;

  conn_t *c;			/* This is the connection object */

  struct sockaddr_storage *ss; 
  /* Two-way connection maintains both send state and receive state variables */
  send_state_t *send_st;
  receive_state_t *rec_st;
};
rel_t *rel_list;


/**
  Constructor for send state variables. called from rel_create()
  */
send_state_t * send_state_init(const struct config_common *cc)
{
  send_state_t *st = (send_state_t *)malloc(sizeof(send_state_t));
  
  if (st)
  {
    st->in_flight_buf = NULL;
    st->waiting_buf = NULL;
    st->last_ack = 1;
    st->last_sent = 0;
	st->current_num = 1;
	st->partial_packet_out = false;
	st->partial_packet_seqno = 0;
	st->terminate_connection = false;
    st->config = cc;
	st->in_buf = (input_buf_t *)malloc(sizeof(input_buf_t));
	st->in_buf->valid_bytes = 0;
  }
  return st;
}

/**
  Constructor for receive state variables. called from rel_create()
  */
receive_state_t * receive_state_init(const struct config_common *cc)
{
  receive_state_t *st = (receive_state_t *)malloc(sizeof(receive_state_t));
  
  if (st)
  {		  
    st->buf = NULL;
    st->last_ack = 1;
	st->terminate_connection = false;
    st->config = cc;
	st->out_buf = (output_buf_t *)malloc(sizeof(output_buf_t));
	st->out_buf->valid_bytes = 0;
  }
  return st;
}

/**
  Destructor for a Packet object
  */
void destroy_packet(packet_t *packet)
{
  free(packet);
  return;
}

/**
  Destructor for an in-flight packet object
  */
void destroy_in_flight_packet(in_flight_packet_t *in_fl_packet)
{
  destroy_packet(in_fl_packet->packet);
  free(in_fl_packet);
  return;
}

/**
  Destructor for a linked list of packets
  */
void destroy_packet_buffer(packet_buf_t *buf)
{
  if (!buf) return;
  destroy_packet_buffer(buf->next);
  destroy_packet(buf->packet);
  free(buf);
  return;
}

/**
  Destructor for a linked list of in flight packets
  */
void destroy_in_flight_buffer(in_flight_packet_buf_t *buf)
{
  if (!buf) return;
  destroy_in_flight_buffer(buf->next);
  destroy_in_flight_packet(buf->current);
  free(buf);
  return;
}

/**
  Destructor for tearing down the send state. Called from rel_destroy()
  */
void send_state_destroy(send_state_t *st)
{
  destroy_in_flight_buffer(st->in_flight_buf);
  destroy_packet_buffer(st->waiting_buf);
  free(st->in_buf);
  free(st);
  return;
}

/**
  Destructor for tearing down the receive state. Called from rel_destroy()
  */
void receive_state_destroy(receive_state_t *st)
{
  destroy_packet_buffer(st->buf);
  free(st->out_buf);
  free(st);
  return;
}

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
  /* Assign sockaddr_storage address if present */
  if (ss)
  {
    r->ss = (struct sockaddr_storage *)malloc(sizeof(struct sockaddr_storage));
	memcpy(r->ss, ss, sizeof(struct sockaddr_storage));
  }
  else
    r->ss = NULL;

  r->next = rel_list;
  r->prev = &rel_list;
  if (rel_list)
    rel_list->prev = &r->next;
  rel_list = r;

  /* Initialize send state and receive state variables */
  
  r->send_st = send_state_init(cc);
  if (!r->send_st) return NULL;

  r->rec_st = receive_state_init(cc);
  if (!r->rec_st) return NULL;

  return r;
}

/**
  Called to tear down a connection
  */
void
rel_destroy (rel_t *r)
{
  if (r->next)
    r->next->prev = r->prev;
  *r->prev = r->next;
  conn_destroy (r->c);
  if (r->ss)
    free(r->ss);
  /* Tear down send state and receive state */
  send_state_destroy(r->send_st);
  receive_state_destroy(r->rec_st);
  free(r);
}

/**
  This function is used to create a packet out of raw data currently in the input buffer. 
  This is data from stdin for lab2 or from the TCP socket for lab3 server mode. 
  First checks if data can be added on to an existing waiting packet that is only partially full. If yes, add to that packet, and add 
  any remaining data to a new partial packet. Otherwise just create a new packet with the data.
  Any new packet created is added on to waiting packets queue.   
  
  This function is called whenever new data is read from rel_read()

  Params:
  create_empty_packet -> set to true when you want to create an (empty) packet even when the input buffer size is empty. Set to true when EOF is read from input and an empty packet needs to be sent out.
  */
void packetize_buffer(rel_t *s, bool create_empty_packet)
{
  send_state_t *send_state = s->send_st;

  /* See if the last packet in the waiting buffer is a partial packet, if so, add to this packet first */
  packet_buf_t *tail = send_state->waiting_buf; 
  while(tail && tail->next)
    tail = tail->next;
  if (send_state->in_buf->valid_bytes > 0 && tail && ntohs(tail->packet->len) < COMPLETE_PACKET_SIZE)
  {
	packet_t *pkt = tail->packet;
	uint16_t len = ntohs(pkt->len);
    
	/* Copy as much data as can be copied on to the partial packet, and repopulate packet length, checksum, piggybacked ack (might have changed     since last modified time */
	int capacity_left = DATA_CHUNK_SIZE - (len - DATA_PACKET_MINSIZE);
	int bytes_to_be_copied = send_state->in_buf->valid_bytes > capacity_left ? capacity_left : send_state->in_buf->valid_bytes;

	memcpy(pkt->data + len - DATA_PACKET_MINSIZE, send_state->in_buf->buf, bytes_to_be_copied);
	uint16_t new_length = len + bytes_to_be_copied;
	pkt->len = htons(new_length);
	pkt->ackno = htonl(s->rec_st->last_ack);
	pkt->cksum = 0;
    pkt->cksum = cksum(pkt, new_length);

	/* Housekeeping for input buffer */
	send_state->in_buf->valid_bytes -= bytes_to_be_copied;
	memmove(send_state->in_buf->buf, send_state->in_buf->buf + bytes_to_be_copied, send_state->in_buf->valid_bytes);	
  }
  
  /* If there's anything left in the buffer, create a new packet */
  if (send_state->in_buf->valid_bytes == 0 && !create_empty_packet) return;

  packet_t *packet = (packet_t *)malloc(sizeof(packet_t));
  if (!packet) return;
  
  uint16_t packet_length = DATA_PACKET_MINSIZE + send_state->in_buf->valid_bytes;
  
  /* Populate packet fields */
  packet->cksum = 0;
  packet->len = htons(packet_length);
  packet->ackno = htonl(s->rec_st->last_ack);
  packet->seqno = htonl(send_state->current_num);
  memcpy(packet->data, send_state->in_buf->buf, send_state->in_buf->valid_bytes);
  packet->cksum = cksum(packet, packet_length);

  /* add newly created packet to waiting packets buffer */
  packet_buf_t *last = (packet_buf_t *)malloc(sizeof(packet_buf_t));
  last->packet = packet;
  last->next = NULL;

  
  if (!tail) /* Nothing in waiting queue yet */
    send_state->waiting_buf = last;
  else /* Add to the end of queue */
    tail->next = last;

  /* send state housekeeping */
  send_state->current_num += 1;
  send_state->in_buf->valid_bytes = 0;  
}

/**
  This function is used to transmit any packets waiting in the queue.
  Called in response to acknowledgements received for packets in flight or new data input from stdin/TCP socket. 
  Removes packets from waiting queue and adds them to in flight queue.
  First checks waiting packet length and sends a partial packet only if another partial packet is not already in flight(Nagle's algo)
  */
void transmit_waiting_packets(rel_t *r)
{
  send_state_t *send_state = r->send_st;

  /* Make sure LSS - LAS <= send window */ 
  while (send_state->waiting_buf && send_state->last_sent - (send_state->last_ack-1) < send_state->config->window)
  {
	/* Take the head of waiting queue and create an in-flight packet */
    packet_buf_t *waiting_buf = send_state->waiting_buf;
    
	uint16_t packet_len = ntohs(waiting_buf->packet->len);

	/* If a partial packet already out, don't send */
	if (send_state->partial_packet_out && packet_len < COMPLETE_PACKET_SIZE)
		break;

	/* Set partial_packet_out to TRUE if sending out a partial packet */
    if (packet_len < COMPLETE_PACKET_SIZE)
    {
	  send_state->partial_packet_out = true;
	  send_state->partial_packet_seqno = ntohl(waiting_buf->packet->seqno);
    }

	in_flight_packet_t *in_fl_pkt = (in_flight_packet_t *)malloc(sizeof(in_flight_packet_t));
	in_fl_pkt->packet = waiting_buf->packet;
    gettimeofday(&(in_fl_pkt->sent_time), NULL);	

	/* Transmit packet */
	conn_sendpkt(r->c, waiting_buf->packet, ntohs(waiting_buf->packet->len));

	/* Update send state */
	send_state->last_sent = ntohl(waiting_buf->packet->seqno);

	/* Insert at the end of in-flight queue */
	in_flight_packet_buf_t *tail = send_state->in_flight_buf;
	while(tail && tail->next)
	  tail = tail->next;

	in_flight_packet_buf_t *node = (in_flight_packet_buf_t *)malloc(sizeof(in_flight_packet_buf_t));
	node->current = in_fl_pkt;
	node->next = NULL;

	if (tail)
	  tail->next = node;
	else
	  send_state->in_flight_buf = node;
    	
    /* Discard from waiting queue and free up memory */
	send_state->waiting_buf	= waiting_buf->next;
	free(waiting_buf);
  }
}

/**
  This function creates a new connection if the received packet has sequence number 1. 
  Called from rel_demux in server mode. 
  Returns a pointer to the new rel_t object if a new connection is created, else NULL.
 */
rel_t *try_new_connection(packet_t *pkt, const struct sockaddr_storage *ss, const struct config_common *cc)
{
  rel_t *new_conn = NULL;
  if (ntohl(pkt->seqno) == 1)
    new_conn = rel_create(NULL, ss, cc);
  return new_conn;
}

/**
  Utility function to verify the checksum of a packet
  */
bool verify_cksum(packet_t *pkt)
{
  uint16_t len = ntohs(pkt->len);
  if (len < PACKET_MINSIZE || len > DATA_CHUNK_SIZE+DATA_PACKET_MINSIZE) return false;
  uint16_t given_cksum = pkt->cksum;
  pkt->cksum = 0;
  uint16_t calc_cksum = cksum(pkt,len);
  if (calc_cksum != given_cksum) return false;
  
  /* Restore packet checksum */
  pkt->cksum = given_cksum;
  return true; 
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
  if (len < PACKET_MINSIZE) return;
  
  /* Verify checksum */
  if (!verify_cksum(pkt)) return;
  
  /* Check is one of the existing open connections matches the sockaddr */
  rel_t *current = rel_list;
  while(current)
  {
    if (addreq(current->ss, ss) == 1)
	  break;
    current = current->next;	
  } 

  if (!current) /* Nothing on rel_list matches */ 
  { 
    rel_t *new_conn = try_new_connection(pkt, ss, cc);
    if (new_conn) rel_recvpkt(new_conn, pkt, len);
  }
  else /* Existing connection, call rel_recvpkt */
    rel_recvpkt(current, pkt, len);
  return;
}

/**
  This function is used by the receive state to send acknowledgements. 
  Called in response to receiving a data packet. 
  */
void send_acknowledgement(rel_t *r, uint32_t ackno)
{
  packet_t *pkt = (packet_t *)malloc(sizeof(packet_t));
  if (!pkt) return;
  pkt->cksum = 0;
  pkt->ackno = htonl(ackno);
  pkt->len = htons(PACKET_MINSIZE);
  pkt->cksum = cksum(pkt, PACKET_MINSIZE);

  conn_sendpkt(r->c, pkt, PACKET_MINSIZE);
  
  free(pkt);
  
  /* Update recieve state */
  r->rec_st->last_ack = ackno;
}

/**
  Handler for data packets. Called from rel_recvpkt()
  Makes sure the packet sequence number is in the receive window, and if so, adds it to 
  receive buffer, in order. 
  Sends a duplicate ack on receiving out of window packets
  Calls rel_output() to try and drain the receive packet buffer
  */
void handle_datapackets(rel_t *r, packet_t *pkt)
{
  receive_state_t *rec_state = r->rec_st;
  uint32_t seqno = ntohl(pkt->seqno);

  /* Out of receive window, or duplicated, send duplicate ack and discard */
  if (seqno - rec_state->last_ack >= rec_state->config->window) 
  {
    send_acknowledgement(r,rec_state->last_ack);
    return;
  }
  
  /* Create a new node in the receive packet list */
  packet_buf_t *new_pkt = (packet_buf_t *)malloc(sizeof(packet_buf_t));
  new_pkt->packet = (packet_t *)malloc(sizeof(packet_t));
  
  memcpy(new_pkt->packet,pkt,sizeof(packet_t));
  new_pkt->next = NULL;

  /* Find the correct position for the newly received packet in the recieve packet list */
  packet_buf_t *head = rec_state->buf;
  packet_buf_t *prev = NULL;
  
  while(head)
  {
    if (ntohl(head->packet->seqno) >= seqno) break;
    prev = head;
	head = head->next;
  }
  
  /* Already seen this packet, drop */
  if (head && ntohl(head->packet->seqno) == seqno)
  {
    rel_output(r);
	return;
  }

  if (prev)
  {
    prev->next = new_pkt;
	new_pkt->next = head;
  } 
  else /* Receive buffer is empty or has only one packet*/
  {
	rec_state->buf = new_pkt;
	new_pkt->next = head;
  }
  /* Try and drain the receive buffer */
  rel_output(r);
  return;
}

/**
  Handler for acknowledgement packets. Called from rel_recvpkt()
  
  Walk down the in-flight packet queue and discard all packets whose 
  sequence numbers are less than the currently acknowledged packet (cumulative ack)
  
  If this means that a partial packet that was in-flight is acked, then set partial_packet_out to false 
  
  Transmit waiting packets
  */
void handle_ackpackets(rel_t *r, uint32_t ackno)
{
  send_state_t *send_state = r->send_st;

  /* partial packet acked, free to send another one */
  if (ackno > send_state->partial_packet_seqno)
    send_state->partial_packet_out = false;

  bool ack_valid = false;
  
  /* Walk down the in-flight queue until you reach a packet that has a seqno greater than 
	 or equal to the ack received (cumulative ack). Free up in flight packet acknowledged. */
  while(send_state->in_flight_buf)
  {
     in_flight_packet_buf_t  *in_fl_buf = send_state->in_flight_buf;
     if (ntohl(in_fl_buf->current->packet->seqno) >= ackno) break; 

     ack_valid = true;
	 send_state->in_flight_buf = in_fl_buf->next;
     destroy_in_flight_packet(in_fl_buf->current);
	 free(in_fl_buf);
  }
  
  /* Try transmitting waiting packets */
  if (ack_valid)
  {
    send_state->last_ack = ackno;
    transmit_waiting_packets(r);
  }
  return;
}

/**
  This function is used to check if connection should be terminated and calls rel_destroy() is so.
  Called from rel_timer().  
  The following conditions must hold for termination:
  (a) Send state terminate condition is set. This means we saw an EOF from stdin
  (b) Receive state terminate condition is set. This means we received an EOF from the other side
  (c) No packets in flight, none waiting, and input buffer is empty
  (d) No packets in receive buffer, or waiting in output_buffer for conn_bufspace to clear
  (e) A certain FIN_WAIT period has passed since the terminate conditions were set
  */
void  terminate_connection(rel_t *s)
{
  send_state_t * send_state = s->send_st;
  receive_state_t * receive_state = s->rec_st;
  
  if (!send_state->terminate_connection || !receive_state->terminate_connection) return;

  if (send_state->in_flight_buf || send_state->waiting_buf || send_state->in_buf->valid_bytes > 0) return;
  if (receive_state->buf || receive_state->out_buf->valid_bytes > 0) return;
  
  struct timeval current_time;
  gettimeofday(&current_time, NULL);
  
  /* Wait for FIN_WAIT_TIME */
  long current_time_ms = current_time.tv_sec*1000 + current_time.tv_usec/1000;
  long send_terminate_time_ms = send_state->terminate_time.tv_sec*1000 + send_state->terminate_time.tv_usec/1000;
  long rec_terminate_time_ms = receive_state->terminate_time.tv_sec*1000 + receive_state->terminate_time.tv_usec/1000;
  long terminate_waittime_ms = send_state->config->timeout*FIN_WAIT_MULTIPLIER;
  
  if (current_time_ms - send_terminate_time_ms < terminate_waittime_ms || 
				  current_time_ms - rec_terminate_time_ms < terminate_waittime_ms) return;
  rel_destroy(s);
}

/**
  Handler for receiving packets. Checks checksum and passes on to 
  appropriate handlers if valid.
  */
void
rel_recvpkt (rel_t *r, packet_t *pkt, size_t n)
{
  if (n < PACKET_MINSIZE) return;
  if(!verify_cksum(pkt)) return;
  uint16_t len = ntohs(pkt->len);
  /* Always handle ack no. Might be piggybacked */
  handle_ackpackets(r,ntohl(pkt->ackno));
  
  /* If data packet */
  if (len > sizeof(ack_packet_t))
    handle_datapackets(r, pkt);  
}

/**
  This function is used to process data in the input buffer. 
  Called from rel_read() whenever new data is read from conn_input()
  */
void process_input_buffer(rel_t *s)
{
  packetize_buffer(s, false);
  transmit_waiting_packets(s);
}

/**
  Called from rel_read() when EOF read from input. 
  Set terminate condition for send state. 
  Make a packet out of whatever data is left in the input buffer.
  Make an empty packet to signal EOF to the other side. 
  Try and transmit these packets
  */
void process_end_of_file(rel_t *s)
{
  s->send_st->terminate_connection = true;
  gettimeofday(&(s->send_st->terminate_time), NULL);
  packetize_buffer(s, false);
  packetize_buffer(s, true);
  transmit_waiting_packets(s);
}

/**
  Used to read data from stdin. Calls appropriate handlers
  */
void
rel_read (rel_t *s)
{ 
  /* If EOF already read, don't accept any more data */
  if (s->send_st->terminate_connection) return;
  
  /* Read data into input buffer */
  input_buf_t *in_buf = s->send_st->in_buf;
  int buf_capacity = DATA_CHUNK_SIZE - in_buf->valid_bytes;
  int read_bytes = conn_input(s->c, in_buf->buf + in_buf->valid_bytes, buf_capacity);

  if (read_bytes == 0) return;
  
  /* Read some data */
  else if (read_bytes > 0)
  {
    in_buf->valid_bytes += read_bytes;
    process_input_buffer(s);
  }
  /* Handle EOF */ 
  else
    process_end_of_file(s);
}

/**
  This function is used to output data from received packets to stdout
  Drains as many packets as possible from the recieve buffer.
  Any overflow is kept in an output buffer to be emptied as soon as conn_bufspace clears. 
  */
void
rel_output (rel_t *r)
{
  receive_state_t *rec_state = r->rec_st;
  /* Check if the output buffer has any leftover bytes, if so, drain these first */
  if (rec_state->out_buf->valid_bytes > 0)
  {
	int write_bytes = conn_output(r->c, rec_state->out_buf->buf, rec_state->out_buf->valid_bytes);
	rec_state->out_buf->valid_bytes -= write_bytes;

	/* If the output buffer still couldn't be drained completely, return */
	if (rec_state->out_buf->valid_bytes > 0)
	{
	  memmove(rec_state->out_buf->buf, rec_state->out_buf->buf+write_bytes,rec_state->out_buf->valid_bytes);
	  return;
	}
  }

  /* seq no of the last packet drained. Keeping with convention, this is the next expected packet */
  uint32_t last_drained = rec_state->last_ack;
  while(rec_state->buf)
  {
    packet_buf_t *buf = rec_state->buf;
	uint32_t current_seqno = ntohl(buf->packet->seqno);
	
	/* if this packet is not the next expected packet, stop */
	if (current_seqno > last_drained) break;

	uint32_t data_length = ntohs(buf->packet->len) - DATA_PACKET_MINSIZE;
	
	/* Update last_drained */
	last_drained = current_seqno + 1;
	
	int write_bytes = conn_output(r->c, buf->packet->data, data_length);
	
	rec_state->buf = rec_state->buf->next;
	
	/* If you couldn't drain the whole packet, write remainder to output buffer and stop */
	
    memcpy(rec_state->out_buf, buf->packet->data + write_bytes, data_length - write_bytes);
	free(buf->packet);
	free(buf);
	if (write_bytes < data_length)
      break;
	
	/* Received EOF indicator packet. Set terminate condition for the receive side */
	if (data_length == 0)
	{
	  rec_state->terminate_connection = true;
	  gettimeofday(&(rec_state->terminate_time),NULL);
	}
  }

  /* Send acknowledgement for all packets drained in the current run */
  if (last_drained != rec_state->last_ack)
    send_acknowledgement(r,last_drained);
  
  return;  
}

/**
  Walk through list of in-flight packets and re-send those which have timed out.
  Called from rel_timer()
  */
void retry_sending(rel_t *r)
{
  send_state_t *send_state = r->send_st;

  if (!send_state->in_flight_buf) return;
  
  struct timeval current_time;
  in_flight_packet_buf_t *current_node = send_state->in_flight_buf;
  
  while(current_node)
  {
	in_flight_packet_t *pkt = current_node->current;
	gettimeofday(&current_time,NULL);
    long current_time_ms = current_time.tv_sec*1000 + current_time.tv_usec/1000;
	long sent_time_ms = pkt->sent_time.tv_sec*1000 + pkt->sent_time.tv_usec/1000;

	if (current_time_ms - sent_time_ms > send_state->config->timeout)
	{
	  /* Re transmit and update sent time */
	  conn_sendpkt(r->c, pkt->packet, ntohs(pkt->packet->len));
      pkt->sent_time = current_time;
	}
    current_node = current_node->next;
  }
  return;
}

void
rel_timer ()
{
  /* Retransmit any packets that need to be retransmitted */
  rel_t *current = rel_list;
  rel_t *prev = current;
  while(current)
  {
    retry_sending(current);
	prev = current;
	current = current->next;
	
	/* Check if terminate condition is set */
	terminate_connection(prev);
  }
  return;  
}
