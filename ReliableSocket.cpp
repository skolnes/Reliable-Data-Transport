/*
 * File: ReliableSocket.cpp
 *
 * Reliable data transport (RDT) library implementation.
 *
 * Author(s):
 * Robert (Kalikar) De Brum - rdebrum@sandiego.edu
 * Scott Kolnes - skolnes@sandiego.edu
 *
 */

// C++ library includes
#include <iostream>
#include <cstring>

// OS specific includes
#include <unistd.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "ReliableSocket.h"
#include "rdt_time.h"

using std::cerr;
using std::memcpy;
using std::memset;
/*
 * NOTE: Function header comments shouldn't go in this file: they should be put
 * in the ReliableSocket header file.
 */

ReliableSocket::ReliableSocket() {
	this->sequence_number 			= 0;
	this->expected_sequence_number 	= 0;
	this->estimated_rtt 			= 100;
	this->dev_rtt 					= 10;

	//creates socket file descriptor
	this->sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (this->sock_fd < 0) {
		perror("socket");
		exit(EXIT_FAILURE);
	}

	this->state = INIT;
}

void ReliableSocket::accept_connection(int port_num) {
	if (this->state != INIT) {
		cerr << "Cannot call accept on used socket\n";
		exit(EXIT_FAILURE);
	}
	
	// Bind specified port num using our local IPv4 address.
	// This allows remote hosts to connect to a specific port.
	struct sockaddr_in addr; 
	addr.sin_family 		= AF_INET;
	addr.sin_port 			= htons(port_num);
	addr.sin_addr.s_addr 	= INADDR_ANY;

	if (bind(this->sock_fd, (struct sockaddr*)&addr, sizeof(addr))) {
		perror("bind");
	}

	// Wait for a segment to come from a remote host
	char segment[MAX_SEG_SIZE];
	memset(segment, 0, MAX_SEG_SIZE);

	struct sockaddr_in fromaddr;
	unsigned int addrlen 	= sizeof(fromaddr);
	int recv_count 			= recvfrom(this->sock_fd, segment, MAX_SEG_SIZE, 0, 
	(struct sockaddr*)&fromaddr, &addrlen);
	if (recv_count < 0) {
		perror("accept recvfrom");
		exit(EXIT_FAILURE);
	}

	/*
	 * UDP isn't connection-oriented, but calling connect here allows us to
	 * remember the remote host (stored in fromaddr).
	 * This means we can then use send and recv instead of the more complex
	 * sendto and recvfrom.
	 */
	if (connect(this->sock_fd, (struct sockaddr*)&fromaddr, addrlen)) {
		perror("accept connect");
		exit(EXIT_FAILURE);
	}

	// TODO: You should implement a handshaking protocol to make sure that
	// both sides are correctly connected (e.g. what happens if the RDT_CONN
	// message from the other end gets lost?)
	// Note that this function is called by the connection receiver/listener.
	if(this->receiver_handshake(segment)) {
		// Returned true so we are connected
		cerr << "Connection Established\n";
		this->state = ESTABLISHED;
	} else {
		cerr << "Connection not Established\n";
	}
}

bool ReliableSocket::receiver_handshake(char received_segment[MAX_SEG_SIZE]) {
	// Get SYN message
	// Check that segment was the right type of message, namely a RDT_SYN
	// message to indicate that the remote host wants to start a new
	// connection with us.
	RDTHeader* hdr = (RDTHeader*)received_segment;
	if (hdr->type != RDT_SYN) {
		cerr << "ERROR: Didn't get the expected RDT_SYN type.\n";
		return false;
	}
	cerr << "Received RDT_SYN.\n";

	// Send an RDT_SYNACK message to remote host to initiate an RDT connection.
	char send_segment[MAX_SEG_SIZE];
	char recv_segment[MAX_SEG_SIZE];
	
	hdr 					= (RDTHeader*)send_segment;
	hdr->ack_number 		= htonl(0);
	hdr->sequence_number 	= htonl(0);
	hdr->type 				= RDT_SYNACK;
	
	// This call will fill out the recv_segment
	cerr << "Sending RDT_SYNACK.\n";
	bool result = true;
	do
	{
		set_timeout_length(this->estimated_rtt + (4 * this->dev_rtt));
		this->send_and_wait(send_segment, sizeof(RDTHeader), recv_segment);
		cerr << "Sent RDT_SYNACK.\n";

		// Check that segment was the right type of message, namely a RDT_ACK
		// message to indicate that the remote host wants to start a new
		// connection with us.
		hdr 	= (RDTHeader*)recv_segment;
		result 	= (hdr->type == RDT_ACK);

		if (result) {
			cerr << "Received RDT_ACK boi!\n";
		} else {
			cerr << "ERROR: Didn't get the expected RDT_ACK type. Trying again.\n";
		}
	} while (!result);
	
	return true;
}

bool ReliableSocket::send_and_wait(char send_segment[], int send_seg_size, char *recv_segment) {
	int bytes_received = -1;
	
	do {
		// Send the segment
		int start_time = current_msec();
		if (send(this->sock_fd, send_segment, send_seg_size, 0) < 0) {
			perror("syn1 send");
		}

		// Get ready to receive the segment
		memset(recv_segment, 0, MAX_SEG_SIZE);
		// cerr << "Receiving...\n";
		bytes_received = recv(this->sock_fd, recv_segment, MAX_SEG_SIZE, 0);
	
		if (bytes_received < 0 && errno != EAGAIN) { 
			// Means some other error than timeouts
			perror("ACK not received");
			exit(EXIT_FAILURE);
		}

		// RTT calculated here; if timeout occured, then curr_rtt will get
		// overwritten
		this->curr_rtt = current_msec() - start_time;
	} while (bytes_received < 0); // keeps going for timeouts; breaks when data is received

	// Calculate the RTT and set the estimated value (based on the last rtt
	// value	
	this->set_estimated_rtt();
	return true;
}

bool ReliableSocket::send_and_timeout(char send_segment[], int seg_size) {
	bool keep_going = true;
	char recv_segment[MAX_SEG_SIZE];

	do {
		if (send(this->sock_fd, send_segment, sizeof(RDTHeader), 0) < 0) {
			// Not all data sent
			cerr << "send() error\n";
			continue;
		}
		cerr << "Segment sent. Timing out\n";
		
		memset(recv_segment, 0, MAX_SEG_SIZE);
		this->set_timeout_length(this->estimated_rtt + (this->dev_rtt * 4));
		if (recv(this->sock_fd, recv_segment, MAX_SEG_SIZE, 0) > 0) {
			cerr << "Received Packet.\n";
			keep_going = true;
		} else {
			// Timeout reached
			keep_going = false;
		}
	} while (keep_going);
	
	return true;
}

void ReliableSocket::connect_to_remote(char *hostname, int port_num) {
	if (this->state != INIT) {
		cerr << "Cannot call connect_to_remote on used socket\n";
		return;
	}
	
	// set up IPv4 address info with given hostname and port number
	struct sockaddr_in addr; 
	addr.sin_family = AF_INET; 	// use IPv4
	addr.sin_addr.s_addr = inet_addr(hostname);
	addr.sin_port = htons(port_num); 

	/*
	 * UDP isn't connection-oriented, but calling connect here allows us to
	 * remember the remote host (stored in fromaddr).
	 * This means we can then use send and recv instead of the more complex
	 * sendto and recvfrom.
	 */
	if(connect(this->sock_fd, (struct sockaddr*)&addr, sizeof(addr))) {
		perror("connect");
	}

	// TODO: Again, you should implement a handshaking protocol for the
	// connection setup.
	// Note that this function is called by the connection initiator.
	this->sender_handshake();
	
	this->state = ESTABLISHED;
	cerr << "INFO: Connection ESTABLISHED\n";
}

bool ReliableSocket::sender_handshake() {
	// Send an RDT_SYN message to remote host to initiate an RDT connection.
	char send_segment[MAX_SEG_SIZE];
	char recv_segment[MAX_SEG_SIZE];
	
	RDTHeader* hdr 			= (RDTHeader*)send_segment;
	hdr->ack_number 		= htonl(0);
	hdr->sequence_number 	= htonl(0);
	hdr->type 				= RDT_SYN;
	
	// Fill out the recv_segment
	this->send_and_wait(send_segment, sizeof(RDTHeader), recv_segment);
	cerr << "Sent the RDT_SYN.\n";
	// Check that the segment was a SYNACK type
	memset(hdr, 0, sizeof(RDTHeader));
	hdr = (RDTHeader*)recv_segment;
	if (hdr->type != RDT_SYNACK) {
		// This was not a SYNACK response
		perror("Message received was not a SYNACK");
		return false;	
	}

	cerr << "Received RDT_SYNACK.\n";	
	
	// Send ACK
	memset(send_segment, 0, sizeof(RDTHeader));
	hdr 					= (RDTHeader*)send_segment;
	hdr->ack_number 		= htonl(0);
	hdr->sequence_number 	= htonl(0);
	hdr->type 				= RDT_ACK;

	// Send the segment
	this->send_and_timeout(send_segment, sizeof(RDTHeader));
	cerr << "ACK Sent.\n";

	return true;
}

// You should not modify this function in any way.
uint32_t ReliableSocket::get_estimated_rtt() {
	return this->estimated_rtt;
}

void ReliableSocket::set_estimated_rtt(){
	// Caculate the RTT
	this->estimated_rtt *= 0.5;	
	this->estimated_rtt += this->curr_rtt * 0.5;	

	// Now calculated the DEV RTT
	this->dev_rtt *= 0.5;
	int abs_value = (this->curr_rtt > this->estimated_rtt) ? 
					 this->curr_rtt - this->estimated_rtt :
					 this->estimated_rtt - this->curr_rtt;

	this->dev_rtt += (0.5 * abs_value);
	
	//cerr << "EST_RTT: " << this->estimated_rtt << "\n";
	//cerr << "DEV_RTT: " << this->dev_rtt << "\n";

	this->set_timeout_length(this->estimated_rtt + 4 * this->dev_rtt);
}

// You shouldn't need to modify this function in any way.
void ReliableSocket::set_timeout_length(uint32_t timeout_length_ms) {
	cerr << "INFO: Setting timeout to " << timeout_length_ms << " ms\n";
	struct timeval timeout;
	msec_to_timeval(timeout_length_ms, &timeout);

	if (setsockopt(this->sock_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
					sizeof(struct timeval)) < 0) {
		perror("setsockopt");
		exit(EXIT_FAILURE);
	}
}

void ReliableSocket::send_data(const void *data, int length) {
	if (this->state != ESTABLISHED) {
		cerr << "INFO: Cannot send: Connection not established.\n";
		return;
	}

 	// Create the segment, which contains a header followed by the data.
	char send_segment[MAX_SEG_SIZE];
	char recv_segment[MAX_SEG_SIZE];

	// Fill in the header
	RDTHeader *hdr 			= (RDTHeader*)send_segment;
	hdr->sequence_number 	= htonl(this->sequence_number);
	hdr->ack_number 		= htonl(0);
	hdr->type 				= RDT_DATA;

	// Copy the user-supplied data to the spot right past the 
	// 	header (i.e. hdr+1).
	memcpy(hdr + 1, data, length);
	
	// TODO: This assumes a reliable network. You'll need to add code that
	// waits for an acknowledgment of the data you just sent, and keeps
	// resending until that ack comes.
	// Utilize the set_timeout_length function to make sure you timeout after
	// a certain amount of waiting (so you can try sending again).
	
	bool keep_going = true;
	do {
		memset(recv_segment, 0, MAX_SEG_SIZE);
		cerr << "Sending Sequence Number: #" << this->sequence_number << ".\n";
		send_and_wait(send_segment, sizeof(RDTHeader) + length, recv_segment);

		hdr = (RDTHeader*)recv_segment;
		if (hdr->type == RDT_ACK) {
			if (ntohl(hdr->ack_number) == this->sequence_number) {
				// Correct ACK Received
				keep_going = false;
			} else {
				// Out of Order ACK
				cerr << "Out of order ACK: " << ntohl(hdr->ack_number) << ". Supposed to be: " << this->sequence_number << ".\n";
			}
		} else {
			cerr << "Received segment was not an ACK." << hdr->type << "\n";
			keep_going = true;
			/*
			memset(send_segment, 0, MAX_SEG_SIZE);
			RDTHeader *hdr 			= (RDTHeader*)send_segment;
			hdr->sequence_number 	= htonl(this->sequence_number);
			hdr->ack_number 		= htonl(0);
			hdr->type 				= RDT_DATA;
			*/
		}
	}
	while (keep_going);
	cerr << "Received ACK Number: #" << ntohl(hdr->ack_number) << ".\n";
	
	this->sequence_number++;
}


int ReliableSocket::receive_data(char buffer[MAX_DATA_SIZE]) {
	int recv_count = 0;
	bool keep_going = true;
	while (keep_going) {
		cerr << "RECV\n";
		keep_going = false;
		if (this->state != ESTABLISHED) {
			cerr << "INFO: Cannot receive: Connection not established.\n";
			return -1;
		}
	
		char received_segment[MAX_SEG_SIZE];
		char send_segment[sizeof(RDTHeader)];
		memset(received_segment, 0, MAX_SEG_SIZE);
	
		// Set up pointers to both the header (hdr) and data (data) portions of
		// the received segment.
		RDTHeader* hdr = (RDTHeader*)received_segment;	
		void *data = (void*)(received_segment + sizeof(RDTHeader));
	
		// receive the data and check for timeouts/errors
		this->set_timeout_length(this->estimated_rtt + (this->dev_rtt * 4));
		recv_count = recv(this->sock_fd, received_segment, MAX_SEG_SIZE, 0);
		if (recv_count < 0 && errno != EAGAIN) {
			perror("receive_data recv");
			exit(EXIT_FAILURE);
		} else if (recv_count < 0 && errno == EAGAIN) {
			// Timeout
				cerr << "recv timed out.\n";
			
			keep_going = true;
			continue;
		}
	
		// TODO: You should send back some sort of acknowledment that you
		// received some data, but first you'll need to make sure that what you
		// received is the type you want (RDT_DATA) and has the right sequence
			// number.
	
		cerr << "INFO: Received segment. " 
			 << "seq_num = "<< ntohl(hdr->sequence_number) << ", "
			 << "ack_num = "<< ntohl(hdr->ack_number) << ", "
				 << ", type = " << hdr->type << "\n";
	
		uint32_t received_seq_num = hdr->sequence_number;	
		cerr << "Actual Sequence number is #" << this->sequence_number << "\n";
		cerr << "Received Sequence number is #" << ntohl(received_seq_num) << "\n";
		
		if (hdr->type == RDT_FIN) {
			// Sender trying to finish the conversation
			cerr << "Received FIN.\n";
			hdr 					= (RDTHeader*)send_segment;
			hdr->sequence_number 	= htonl(0);
			hdr->ack_number 		= htonl(0);
			hdr->type 				= RDT_FINACK;
			
			// Send the FINACK
			this->send_and_timeout(send_segment, sizeof(RDTHeader));
			cerr << "FINACK Sent.\n";
			
			this->state = FIN_STATE;
			return 0;
		} else {
			// No matter what, we ack the data that we just received
			hdr 					= (RDTHeader*)send_segment;
			hdr->ack_number 		= received_seq_num;
			hdr->sequence_number 	= received_seq_num;
			hdr->type 				= RDT_ACK;
		
			// Send the Ack
			do {
				cerr << "Sending ACK.\n";
			} while (send(this->sock_fd, send_segment, sizeof(RDTHeader), 0) < 0);
			cerr << "ACKed segment number #" << ntohl(hdr->sequence_number) << "\n";
			
			if (ntohl(received_seq_num) == this->sequence_number) {
				// Sequence number was as expected so we can fill the buffer
				// pointer
				++this->sequence_number;
				memcpy(buffer, data, recv_count - sizeof(RDTHeader));
			} else {
				// Sequence number failed so clear the data
				cerr << "\nOut of order data packet.\n\n";
				keep_going = true;
			}	
		}
	}

	return recv_count - sizeof(RDTHeader);
}


void ReliableSocket::close_connection() {
	// Construct a RDT_CLOSE message to indicate to the remote host that we
	// want to end this connection.
	char send_segment[sizeof(RDTHeader)];
	
	RDTHeader* hdr 			= (RDTHeader*)send_segment;
	hdr->sequence_number 	= htonl(0);
	hdr->ack_number 		= htonl(0);
	hdr->type 				= RDT_FIN;

	if (this->state == FIN_STATE) {
		cerr << "Receiver.\n";
		this->receiver_close_handshake();	
	} else {
		cerr << "Sender.\n";
		this->sender_close_handshake();
	}
	
	// At this point we received stuff from sending the correct header type
	// Now we need to check what we are getting back and see if we can finish

	// TODO: As with creating a connection, you need to add some reliability
	// into closing the connection to make sure both sides know that the
	// connection has been closed.
	

	// For connection teardown here, we need to send FIN signal, as well as ACK
	// that fin signal on the receiver side as well as sending own FIN to
	// finally ACK that on the initator side
	
	// Probably need to make this a function
	this->state = CLOSED;
	if (close(this->sock_fd) < 0) {
		perror("close_connection close");
	}
}

void ReliableSocket::sender_close_handshake() {

	// This function is just like the sender handshake but for closing the
	// connection
	char send_segment[MAX_SEG_SIZE];
	char recv_segment[MAX_SEG_SIZE];
	
	RDTHeader* hdr 			= (RDTHeader*)send_segment;
	hdr->ack_number 		= htonl(0);
	hdr->sequence_number 	= htonl(0);
	hdr->type 				= RDT_FIN;
	
	do
	{
		memset(recv_segment, 0, MAX_SEG_SIZE);
		this->send_and_wait(send_segment, sizeof(RDTHeader), recv_segment);
		cerr << "Sent the RDT_FIN.\n";
	
		// Check that the segment was a FINACK type
		hdr = (RDTHeader*)recv_segment;
	} while (hdr->type != RDT_FINACK);

	cerr << "Received RDT_FINACK.\n";	
	this->state = FIN_STATE;
	
	cerr << "Waiting for FIN.\n";
	int recv_count = 0;
	do
	{
		memset(recv_segment, 0, MAX_SEG_SIZE);
		recv_count = recv(this->sock_fd, recv_segment, MAX_SEG_SIZE, 0);
		if (recv_count < 0) {
			// Timeout, keep waiting
			continue;
		} else if (recv_count < 0 && errno != EAGAIN) {
			// Something other than timeout
			perror("Waiting for FIN error");
			exit(EXIT_FAILURE);
		}

		hdr = (RDTHeader*)recv_segment;
	} while (hdr->type != RDT_FIN);

	cerr << "Received FIN.\n";

	// Send FINACK and enter time_wait state for a little bit before closing
	hdr = (RDTHeader*)send_segment;
	hdr->type = RDT_FINACK;
	
	cerr << "Sending FINACK.\n";
	bool keep_going = false;
	do {
		if (send(this->sock_fd, send_segment, sizeof(RDTHeader), 0) < 0) {
			keep_going = true;
			continue;
		}
		cerr << "Sent FINACK. Timing out\n";
		
		memset(recv_segment, 0, MAX_SEG_SIZE);
		this->set_timeout_length(500);
		if (recv(this->sock_fd, recv_segment, MAX_SEG_SIZE, 0) > 0) {
			cerr << "Received Packet.\n";
			hdr = (RDTHeader*)recv_segment;
			if (hdr->type == RDT_FIN) {
				cerr << "FINACK lost. Sending FINACK again.\n";
				keep_going = true;
			}
		} else {
			cerr << "Timed out.\n";
			keep_going = false;
		}
	} while (keep_going == true);
	cerr << "Sent FINACK. Timeout complete.\n";
}

void ReliableSocket::receiver_close_handshake() {
	
	// This function is just like the sender handshake but for closing the
	// connection
	char send_segment[MAX_SEG_SIZE];
	char recv_segment[MAX_SEG_SIZE];
	
	RDTHeader* hdr 			= (RDTHeader*)send_segment;
	hdr->ack_number 		= htonl(0);
	hdr->sequence_number 	= htonl(0);
	hdr->type 				= RDT_FIN;
	
	do
	{
		cerr << "Sending FIN message.\n";
		memset(recv_segment, 0, MAX_SEG_SIZE);
		this->send_and_wait(send_segment, sizeof(RDTHeader), recv_segment);
		hdr = (RDTHeader*)recv_segment;
	} while (hdr->type != RDT_FINACK);

	cerr << "Received FINACK. Connection Closed.\n";
}
