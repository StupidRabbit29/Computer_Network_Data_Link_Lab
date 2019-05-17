#define MAX_SEQ 7				// should be 2^n-1
#define NR_BUFS((MAX_SEQ+1)/2)	// size of sliding window
/*
In this protocol, we have dropped the assumption that
the network layer always has an infinite supply of packets to send.
*/
typedef enum { frame_arrival, cksum_err, timeout, network_layer_ready, ack_timeout }event_type;
#include "protocol.h"
boolean no_nak = true;			// no nak has been sent yet
seq_nr oldest_fream = MAX_SEQ + 1;// initial value is only for the simulator

static boolean between(seq_nr a, seq_nr b, seq_nr c)
{
	// same as protocol5, but shorter and more obscure
	return ((a <= b) && (b < c)) || ((c < a) && (a <= b)) || ((b < c) && (c < a));
}
static void send_frame(frame_kind fk, seq_nr frame_nr, seq_nr frame_expected, packet buffer[])
{
	// construct and send a data, ack or nak frame
	frame s;					// scratch variable

	s.kind = fk;				// kind == data, ack, nak
	if (fk == data)
		s.info = buffer[frame_nr % NR_BUFS];
	s.seq_nr = frame_nr;		// only meaningful for data frames
	s.ack = (frame_expected + MAX_SEQ) % (MAX_SEQ + 1);
	if (fk == nak)				// one nak per frame
		no_nak = false;
	to_physcial_layer(&S);		// transmit the frame
	if (fk == data)
		start_timer(frame_nr % NR_BUFS);
	stop_ack_timer();			// no need for separate ack frame
}

/* A PROTOCOL USING SELECTIVE REPEAT */
void protocol6(void)
{
	seq_nr ack_expected;				// (S)lower edge of sender's window
	seq_nr next_frame_to_send;			// (S)upper edge of sender's window + 1
	seq_nr frame_expected;				// (R)lower edge of reciever's window
	seq_nr too_far;						// (R)upper edge of reciever's window + 1
	int i;								// index into buffer pool
	frame r;							// scratch variable
	packet out_buf[NR_BUFS];			// (S)buffer for the outbound stream
	packet in_buf[NR_BUFS];				// (R)buffer for the inbound stream
	// Associated with each buffer is a bit (arrived) telling whether the buffer is full or empty
	boolean arrived[NR_BUFS];			// (R)inbound bit map
	seq_nr nbuffered;					// (S)how many output buffers currently used
	event_type event;

	enable_network_layer();				// initialize
	ack_expected = 0;					// (R)next ack expected in the inbound stream
	next_frame_to_send = 0;				// (S)number of the next outgoing frame
	frame_expected = 0;					// (R)
	too_far = NR_BUFS;
	nbuffered = 0;						// (S)initially no packets are buffered
	for (i = 0; i < NR_BUFS; i++)
		arrived[i] = false;

	while (true)
	{
		wait_for_event(&event);			// 5 possibilities: frame_arrival, cksum_err, timeout, network_layer_ready, ack_timeout
		switch (event)
		{

			// When the network layer has a packet it wants to send, it can cayse a 'network_layer_ready' event to happen
		case network_layer_ready:	// (S)accept, save, and transmit a new frame
			nbuffered = nbuffered + 1;	// expand the window
			from_network_layer(&out_buf[next_frame_to_send % NR_BUFS]);
			send_frame(data, next_frame_to_send, frame_expected, out_buf);
			inc(next_frame_to_send);
			break;

		case frame_arrival:			// (R)a data or control frame has arrived
			from_physical_layer(&r);// (R)fetch incoming frame from physical layer
			if (r.kind == data)
			{
				// (R)an undamaged frame has arrived
				if (r.seq != frame_expected) && no_nak)
				// (R)frame out of sequence
					send_frame(nak, 0, frame_expected, out_buf);	// sen nak to stimulate retransmission
					/*
					NAKs IMPROVE PERFORMANCE:
					If the NAK get lost, eventually the sender will time out for the very frame
					and send it (and only it) of its own accord, but that may be quite a while later.
					*/
				else
					start_ack_timer();
				if (between(frame_expected, r.seq, too_far) && (arrived[r.seq % NR_BUFS] == false))
				{
					// frames may be accepeted in any order
					arrived[r.seq % NR_BUFS] = true;		// mark buffer as full
					in_buf[r.seq % NR_BUFS] = r.info;		// insert data into buffer
					while (arrived[frame_expected % NR_BUFS] = true)
					{
						// pass frames and advance window
						to_network_layer(&in_buf[frame_expected % NR_BUFS]);
						no_nak = true;
						arrived[frame_expected % NR_BUFS] = false;
						inc(frame_expected);			// advance lower edge of reciever's window
						inc(too_far);					// advance upper edge of reciever's window
						start_ack_timer();				// to see if separate ack is needed
					}
				}
			}
			if ((r.kind == nak) && between(ack_expected, (r.ack + 1) % (MAX_SEQ + 1), next_frame_to_send))
				send_frame(data, (r.ack + 1) % (MAX_SEQ + 1), frame_expected, out_buf);

			while (between(ack_expected, r.ack, next_frame_to_send))
			{
				nbuffered = nbuffered - 1;				// handle piggybacked ack
				stop_timer(ack_expected % NR_BUFS);		// frame arrived intact
				inc(ack_expected);						// advance lower edge of sender's window
			}
			break;

		case cksum_err:
			if (no_nak)
				send_frame(nak, 0, frame_expected, out_buf);	// damaged frame
			break;

		case timeout:
			send_frame(data, oldest_fream, frame_expected, out_buf); // timed out
			break;

		case ack_timeout:
			send_frame(ack, 0, frame_expected, out_buf);		// ack timer expired; send ack

		}
		if (nbuffered < NR_BUFS)
			enable_network_layer();
		else
			disable_network_layer();
	}
}
