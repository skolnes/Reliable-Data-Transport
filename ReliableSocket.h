/*
 * File: ReliableSocket.h
 *
 * Header / API file for library that provides reliable data transport over an
 * unreliable link.
 *
 */

enum RDTMessageType : uint8_t {RDT_SYN, RDT_SYNACK, RDT_FIN, RDT_FINACK, RDT_ACK, RDT_DATA};

/**
 * Format for the header of a segment send by our reliable socket.
 */
struct RDTHeader {
	uint32_t 		sequence_number;
	uint32_t 		ack_number;
	RDTMessageType 	type;
};

enum connection_status { INIT, SYN, SYN_ACK, ACK_EST, ESTABLISHED, FIN_STATE, RECV_ACK, RECV_FIN, SEND_ACK, CLOSED };

/**
 * Class that represents a socket using a reliable data transport protocol.
 * This socket uses a stop-and-wait protocol so your data is sent at a nice,
 * leisurely pace.
 */
class ReliableSocket {
public:
	/*
	 * You probably shouldn't add any more public members to this class.
	 * Any new functions or fields you need to add should be private.
	 */
	
	// These are constants for all reliable connections
	static const int MAX_SEG_SIZE  = 1400;
	static const int MAX_DATA_SIZE = MAX_SEG_SIZE - sizeof(RDTHeader);

	/**
	 * Basic Constructor, setting estimated RTT to 100 and deviation RTT to 10.
	 */
	ReliableSocket();

	/**
	 * Connects to the specified remote hostname on the given port.
	 *
	 * @param hostname Name of the remote host to connect to.
	 * @param port_num Port number of remote host.
	 */
	void connect_to_remote(char *hostname, int port_num);

	/**
	 * Waits for a connection attempt from a remote host.
	 *
	 * @param port_num The port number to listen on.
	 */
	void accept_connection(int port_num);

	/**
	 * Send data to connected remote host.
	 *
	 * @param buffer The buffer with data to be sent.
	 * @param length The amount of data in the buffer to send.
	 */
	void send_data(const void *buffer, int length);

	/**
	 * Receives data from remote host using a reliable connection.
	 *
	 * @param buffer The buffer where received data will be stored.
	 * @return The amount of data actually received.
	 */
	int receive_data(char buffer[MAX_DATA_SIZE]);

	/**
	 * Closes an connection.
	 */
	void close_connection();

	/**
	 * Returns the estimated RTT.
	 * 
	 * @return Estimated RTT for connection (in milliseconds)
	 */
	uint32_t get_estimated_rtt();

private:
	// Private member variables are initialized in the constructor
	int 				sock_fd;
	uint32_t 			sequence_number;
	uint32_t 			expected_sequence_number;
	float 				estimated_rtt;
	float 				dev_rtt;
	int					curr_rtt;
	connection_status 	state;

	// In the (unlikely?) event you need a new field, add it here.

	/**
	 * Sets the timeout length of this connection.
	 *
	 * @note Setting this to 0 makes the timeout length indefinite (i.e. could
	 * wait forever for a message).
	 *
	 * @param timeout_length_ms Length of timeout period in milliseconds.
	 */
	void set_timeout_length(uint32_t timeout_length_ms);

	/*
	 * Add new member functions (i.e. methods) after this point.
	 * Remember that only the comment and header line goes here. The
	 * implementation should be in the .cpp file.
	 */
	bool receiver_handshake(char received_segment[MAX_SEG_SIZE]);
	
	/*
	 * The sender handshake part of opening up an intial connectioin, needed
	 * to 
	 *
	 */
	bool sender_handshake();
	
	/*
	 * Updates the estimated_rtt by updating it with a new value
	 *
	 */ 
	void set_estimated_rtt();
	
	/*
	 * Sends a segment with header and waits to receive something back or
	 * indicates a time out 
	 *
	 * @param send_segment The char array that is being sent to the receiver
	 * @param send_seg_size The specified segment size to be sent 
	 * @param recv_segment The receive char array that will receive from the
	 * 					   sender
	 * @return True if send and wait worked, False if a timeout occured
	 *
	 */
	bool send_and_wait(char send_segment[], int send_seg_size, char *recv_segment); 

	bool send_and_timeout(char send_segment[], int seg_size);

	/*
	 * The sender part of closing the connection between sender and receiver
	 *
	 */
	void sender_close_handshake();

	/*
	 * The receiver part of closing the connection between the sender and
	 * receiver
	 *
	 */
	void receiver_close_handshake();
};
