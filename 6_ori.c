/*
In this protocol, we have dropped the assumption that
the network layer always has an infinite supply of packets to send.
*/

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "protocol.h"
#include "datalink.h"

#define MAX_SEQ 15			// should be 2^n-1
#define NR_BUFS ((MAX_SEQ+1)/2)	// size of sliding window
#define DATA_TIMER  5000
#define ACK_TIMER 350

bool DEBUG = false;

#define inc(k) if (k < MAX_SEQ) k = k + 1; else k = 0;

//typedef enum { frame_arrival, cksum_err, timeout, network_layer_ready, ack_timeout }event_type;

typedef enum { data = 1, ack = 2, nak = 3 } frame_kind;
typedef struct { unsigned char data[PKT_LEN]; } packet;

typedef struct FRAME
{
	unsigned char kind; /* FRAME_DATA */
	unsigned char ack;
	unsigned char seq;
	packet data;
	unsigned int  padding;//padding字段用来存放crc校验位
}frame;

bool no_nak = true;			// no nak has been sent yet
//int oldest_fream = MAX_SEQ + 1;// initial value is only for the simulator****
int phl_ready = 0;

static bool between(seq_nr a, seq_nr b, seq_nr c)
{
	// same as protocol5, but shorter and more obscure
	return ((a <= b) && (b < c)) || ((c < a) && (a <= b)) || ((b < c) && (c < a));
}

static void put_frame(unsigned char *frame, int len)
{
	*(unsigned int *)(frame + len) = crc32(frame, len);//将crc函数计算得到的结果（4字节int）存放在padding字段
	send_frame(frame, len + 4);
	phl_ready = 0;
}

static void Send_Frame(frame_kind fk, seq_nr frame_nr, seq_nr frame_expected, packet buffer[])
{
	// construct and send a data, ack or nak frame
	frame s;					// scratch variable

	s.kind = fk;				// kind == data, ack, nak
	if (fk == FRAME_DATA)
	{
		s.data = buffer[frame_nr % NR_BUFS];
		s.seq = frame_nr;		// only meaningful for data frames
		s.ack = (seq_nr)((frame_expected + MAX_SEQ) % (MAX_SEQ + 1));
		dbg_frame("Send DATA %d %d, ID %d\n", s.seq, s.ack, *(short*)(s.data.data));
		put_frame((unsigned char*)& s, 3 + PKT_LEN);
		start_timer(frame_nr/* % NR_BUFS*/, DATA_TIMER);
	}
	if (fk == FRAME_NAK)			// one nak per frame
	{
		s.seq = frame_nr;		// only meaningful for data frames
		s.ack = (seq_nr)((frame_expected + MAX_SEQ) % (MAX_SEQ + 1));
		no_nak = false;
		dbg_frame("Send NAK  %d\n", s.ack);
		put_frame((unsigned char*)& s, 2);
	}
	// to_physcial_layer(&s);		// transmit the frame
	if (fk == FRAME_ACK)
	{
		s.seq = frame_nr;		// only meaningful for data frames
		s.ack = (seq_nr)((frame_expected + MAX_SEQ) % (MAX_SEQ + 1));
		dbg_frame("Send ACK  %d\n", s.ack);
		put_frame((unsigned char*)& s, 2);
	}

	stop_ack_timer();			// no need for separate ack frame
}

/* A PROTOCOL USING SELECTIVE REPEAT */
int main(int argc, char **argv)
{
	seq_nr ack_expected;				// (S)lower edge of sender's window
	seq_nr next_frame_to_send;			// (S)upper edge of sender's window + 1
	seq_nr frame_expected;				// (R)lower edge of reciever's window
	seq_nr too_far;						// (R)upper edge of reciever's window + 1
	int i;								// index into buffer pool
	struct FRAME f;							// scratch variable
	packet out_buf[NR_BUFS];			// (S)buffer for the outbound stream
	packet in_buf[NR_BUFS];				// (R)buffer for the inbound stream
	// Associated with each buffer is a bit (arrived) telling whether the buffer is full or empty
	bool arrived[NR_BUFS];			// (R)inbound bit map
	seq_nr nbuffered;					// (S)how many output buffers currently used

	disable_network_layer();				// initialize
	ack_expected = 0;					// (R)next ack expected in the inbound stream
	next_frame_to_send = 0;				// (S)number of the next outgoing frame
	frame_expected = 0;					// (R)
	too_far = NR_BUFS;
	nbuffered = 0;						// (S)initially no packets are buffered
	for (i = 0; i < NR_BUFS; i++)
		arrived[i] = false;

	int event, arg;
	int len = 0;

	protocol_init(argc, argv);
	lprintf("Designed by 223, build: " __DATE__"  "__TIME__"\n");

	while (true)
	{
		event = wait_for_event(&arg);	// 5 possibilities: frame_arrival, cksum_err, timeout, network_layer_ready, ack_timeout
		
		switch (event)
		{
			// When the network layer has a packet it wants to send, it can cayse a 'network_layer_ready' event to happen
		case NETWORK_LAYER_READY:	// (S)accept, save, and transmit a new frame
			/*if (DEBUG)
			{
				printf("*******************************************\n");
				printf("ack_ep:%d  next_f_send:%d  frame_ep:%d  too_far:%d  nbuffered:%d\n", ack_expected, next_frame_to_send, frame_expected, too_far, nbuffered);
				printf("0/8\t1/9\t2/10\t3/11\t4/12\t5/13\t6/14\t7/15\n");
				for (int i = 0; i < NR_BUFS; i++)
					printf("%d\t", arrived[i]);
				printf("\n*******************************************\n");
			}*/

			nbuffered = nbuffered + 1;	// expand the window
			get_packet(out_buf[next_frame_to_send % NR_BUFS].data);
			Send_Frame(data, next_frame_to_send, frame_expected, out_buf);
			inc(next_frame_to_send);
			break;

		case PHYSICAL_LAYER_READY:
			phl_ready = 1;
			break;

		case FRAME_RECEIVED:			// (R)a data or control frame has arrived
			len = recv_frame((unsigned char *)&f, sizeof(f));
			//from_physical_layer(&r);// (R)fetch incoming frame from physical layer
			if (len < 5 || crc32((unsigned char *)&f, len) != 0) 
				//计算整个帧的crc，含padding字段，故crc校验结果理应为0
			{
				dbg_event("**** Receiver Error, Bad CRC Checksum\n");
				if (no_nak)
					Send_Frame(nak, 0, frame_expected, out_buf);
				break;
			}
					
			if (f.kind == FRAME_ACK)
				dbg_frame("Recv ACK  %d\n", f.ack);
			if (f.kind==FRAME_NAK)
				dbg_frame("Recv NAK  %d\n", f.ack);

			if (f.kind == FRAME_DATA)
			{
				/*if (DEBUG)
				{
					printf("*******************************************\n");
					printf("ack_ep:%d  next_f_send:%d  frame_ep:%d  too_far:%d  nbuffered:%d\n", ack_expected, next_frame_to_send, frame_expected, too_far, nbuffered);
					printf("0/8\t1/9\t2/10\t3/11\t4/12\t5/13\t6/14\t7/15\n");
					for (int i = 0; i < NR_BUFS; i++)
						printf("%d\t", arrived[i]);
					printf("\n*******************************************\n");
				}*/

				dbg_frame("Recv DATA %d %d, ID %d\n", f.seq, f.ack, *(short *)(f.data.data));
				// (R)an undamaged frame has arrived
				if (f.seq != frame_expected && no_nak)
				// (R)frame out of sequence
					Send_Frame(nak, 0, frame_expected, out_buf);	// sen nak to stimulate retransmission
					/*
					NAKs IMPROVE PERFORMANCE:
					If the NAK get lost, eventually the sender will time out for the very frame
					and send it (and only it) of its own accord, but that may be quite a while later.
					*/
				else
					start_ack_timer(ACK_TIMER);

				if (between(frame_expected, f.seq, too_far) && (arrived[f.seq % NR_BUFS] == false))
				{
					// frames may be accepeted in any order
					arrived[f.seq % NR_BUFS] = true;		// mark buffer as full
					in_buf[f.seq % NR_BUFS] = f.data;		// insert data into buffer
					
					while (arrived[frame_expected % NR_BUFS] == true)
					{
						// pass frames and advance window
						//to_network_layer(&in_buf[frame_expected % NR_BUFS]);
						put_packet(in_buf[frame_expected % NR_BUFS].data, len - 7);
						no_nak = true;
						arrived[frame_expected % NR_BUFS] = false;
						inc(frame_expected);			// advance lower edge of reciever's window
						inc(too_far);					// advance upper edge of reciever's window
						start_ack_timer(ACK_TIMER);				// to see if separate ack is needed
					}
				}
			}

			if ((f.kind == FRAME_NAK) && between(ack_expected, (f.ack + 1) % (MAX_SEQ + 1), next_frame_to_send))
				Send_Frame(data, (f.ack + 1) % (MAX_SEQ + 1), frame_expected, out_buf);

			while (between(ack_expected, f.ack, next_frame_to_send))
			{
				nbuffered = nbuffered - 1;				// handle piggybacked ack
				stop_timer(ack_expected/* % NR_BUFS*/);		// frame arrived intact
				inc(ack_expected);						// advance lower edge of sender's window
			}

			/*if (DEBUG)
			{
				printf("*******************************************\n");
				printf("ack_ep:%d  next_f_send:%d  frame_ep:%d  too_far:%d  nbuffered:%d\n", ack_expected, next_frame_to_send, frame_expected, too_far, nbuffered);
				printf("0/8\t1/9\t2/10\t3/11\t4/12\t5/13\t6/14\t7/15\n");
				for (int i = 0; i < NR_BUFS; i++)
					printf("%d\t", arrived[i]);
				printf("\n*******************************************\n");
			}*/

			break;

		//case cksum_err:
		//	if (no_nak)
		//		Send_Frame(nak, 0, frame_expected, out_buf);	// damaged frame
		//	break;

		case DATA_TIMEOUT:
			/*if (DEBUG)
			{
				printf("*******************************************\n");
				printf("ack_ep:%d  next_f_send:%d  frame_ep:%d  too_far:%d  nbuffered:%d\n", ack_expected, next_frame_to_send, frame_expected, too_far, nbuffered);
				printf("0/8\t1/9\t2/10\t3/11\t4/12\t5/13\t6/14\t7/15\n");
				for (int i = 0; i < NR_BUFS; i++)
					printf("%d\t", arrived[i]);
				printf("\n*******************************************\n");
			}*/

			dbg_event("---- DATA %d timeout\n", arg);
			Send_Frame(data, arg, frame_expected, out_buf); // timed out
			break;

		case ACK_TIMEOUT:
			/*if (DEBUG)
			{
				printf("*******************************************\n");
				printf("ack_ep:%d  next_f_send:%d  frame_ep:%d  too_far:%d  nbuffered:%d\n", ack_expected, next_frame_to_send, frame_expected, too_far, nbuffered);
				printf("0/8\t1/9\t2/10\t3/11\t4/12\t5/13\t6/14\t7/15\n");
				for (int i = 0; i < NR_BUFS; i++)
					printf("%d\t", arrived[i]);
				printf("\n*******************************************\n");
			}*/

			Send_Frame(ack, 0, frame_expected, out_buf);		// ack timer expired; send ack
			break;
		}

		if (nbuffered < NR_BUFS && phl_ready)
			enable_network_layer();
		else
			disable_network_layer();
	}
}

/*void printstatus(void)
{
	if (DEBUG)
	{
		printf("*******************************************\n");
		printf("ack_ep:%d  next_f_send:%d  frame_ep:%d  too_far:%d  nbuffered:%d\n", ack_expected, next_frame_to_send, frame_expected, too_far, nbuffered);
		printf("0/8\t1/9\t2/10\t3/11\t4/12\t5/13\t6/14\t7/15\n");
		for (int i = 0; i < NR_BUFS; i++)
			printf("%d\t", arrived[i]);
		printf("\n*******************************************\n");
	}
}*/
