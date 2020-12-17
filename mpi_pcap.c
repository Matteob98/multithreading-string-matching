#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pcap.h>
#include <netinet/ip.h>
#include <netinet/if_ether.h>


// PCAP packet struct
typedef struct {
	char data[6553];
} Payload;

/* UDP header struct */
struct UDP_hdr {
	u_short	uh_sport;		/* source port */
	u_short	uh_dport;		/* destination port */
	u_short	uh_ulen;		/* datagram length */
	u_short	uh_sum;		/* datagram checksum */
};

/* TCP structs */

/* Ethernet header */
struct sniff_ethernet {
	u_char ether_dhost[ETHER_ADDR_LEN]; /* Destination host address */
	u_char ether_shost[ETHER_ADDR_LEN]; /* Source host address */
	u_short ether_type; /* IP? ARP? RARP? etc */
};

/* IP header */
struct sniff_ip {
	u_char ip_vhl;		/* version << 4 | header length >> 2 */
	u_char ip_tos;		/* type of service */
	u_short ip_len;	/* total length */
	u_short ip_id;		/* identification */
	u_short ip_off;	/* fragment offset field */
	#define IP_RF 0x8000		/* reserved fragment flag */
	#define IP_DF 0x4000		/* don't fragment flag */
	#define IP_MF 0x2000		/* more fragments flag */
	#define IP_OFFMASK 0x1fff	/* mask for fragmenting bits */
	u_char ip_ttl;		/* time to live */
	u_char ip_p;		/* protocol */
	u_short ip_sum;		/* checksum */
	struct in_addr ip_src,ip_dst; /* source and dest address */
};
#define IP_HL(ip)		(((ip)->ip_vhl) & 0x0f)
#define IP_V(ip)		(((ip)->ip_vhl) >> 4)

/* TCP header */
typedef u_int tcp_seq;

struct sniff_tcp {
	u_short th_sport;	/* source port */
	u_short th_dport;	/* destination port */
	tcp_seq th_seq;	/* sequence number */
	tcp_seq th_ack;	/* acknowledgement number */
	u_char th_offx2;	/* data offset, rsvd */
	#define TH_OFF(th)	(((th)->th_offx2 & 0xf0) >> 4)
	u_char th_flags;
	#define TH_FIN 0x01
	#define TH_SYN 0x02
	#define TH_RST 0x04
	#define TH_PUSH 0x08
	#define TH_ACK 0x10
	#define TH_URG 0x20
	#define TH_ECE 0x40
	#define TH_CWR 0x80
	#define TH_FLAGS (TH_FIN|TH_SYN|TH_RST|TH_ACK|TH_URG|TH_ECE|TH_CWR)
	u_short th_win;		/* window */
	u_short th_sum;		/* checksum */
	u_short th_urp;		/* urgent pointer */
};

#define UDP 0
#define TCP 1

const unsigned char* dump_TCP_packet(const unsigned char *packet);
const unsigned char* dump_UDP_packet(const unsigned char *packet, struct timeval ts, unsigned int capture_len);
void problem_pkt(struct timeval ts, const char *reason);
void too_short(struct timeval ts, const char *truncated_hdr);

void Get_Size(int *n, char filename[], int my_rank, int* flag);

int main (int argc, char *argv[]){
	int my_rank, comm_sz;
	MPI_Init(NULL, NULL);
	MPI_Comm_rank(MPI_COMM_WORLD, &my_rank);
	MPI_Comm_size(MPI_COMM_WORLD, &comm_sz);
	
	/* Building MPI_Payload Datatype */
	MPI_Datatype MPI_Payload;
	int array_of_blocklengths[] = {6553};
	MPI_Aint array_of_displacements[] = {0};
	MPI_Datatype array_of_types[] = {MPI_CHAR};
	MPI_Type_create_struct(1, array_of_blocklengths, array_of_displacements, array_of_types, &MPI_Payload);
	MPI_Type_commit(&MPI_Payload);
	
	/* Getting packet type from input */
	int packet_type;
	if (argc == 3) {
		if (strcmp(argv[2], "udp") == 0) {
			packet_type = UDP;
		}
		else if (strcmp(argv[2], "tcp") == 0) {
			packet_type = TCP;
		}
		else {
			printf("USAGE ./serial <file.pcap> [tcp/udp]\n");
			exit(1);
		}
	}	
	else {
		printf("USAGE: ./serial <file.pcap> [tcp/udp]\n");
		exit(1);
	}
	
	int num_payloads, flag = 0;
	Get_Size(&num_payloads, argv[1], my_rank, &flag);
	//we check for error in pcap file opening
	if (flag == -1) {
		MPI_Finalize();
		return 0;
	}
	
	/* At this point, we have the total size, so we can allocate local arrays */
	int *local_size = malloc(comm_sz*sizeof(int)); //array that stores the number of packets that each process must have
	int *displ = malloc(comm_sz*sizeof(int)); //array of displacement for Scatterv 
	
	for (int i = 0; i < comm_sz; i++) {
		local_size[i] = num_payloads/comm_sz;
	} local_size[0] += num_payloads%comm_sz;
	
	int offset = 0;
	for (int i = 0; i < comm_sz; i++) {
		displ[i] = offset;
		offset += local_size[i];
	}
	
	Payload *local_buff = malloc(local_size[my_rank]*sizeof(Payload));
	Payload *a = NULL;
	if (my_rank == 0){ //rank 0 is in charge of gathering all payloads
		a = malloc(num_payloads*sizeof(Payload));
		char errbuff[PCAP_ERRBUF_SIZE];
		struct pcap_pkthdr header;
		pcap_t *pcap = pcap_open_offline(argv[1], errbuff);
		const unsigned char *packet;
		int i = 0;
		while ((packet = pcap_next(pcap, &header)) != NULL) {
			const unsigned char* payload;
			if(packet_type == UDP) 
				payload = dump_UDP_packet(packet, header.ts, header.caplen); // Getting the payload
			else //tcp
				payload = dump_TCP_packet(packet); // Getting the payload
			strcpy(a[i].data, (char *) payload); //we store the payload in the array of payloads
			i++;
		}
	}
	MPI_Scatterv(a, local_size, displ, MPI_Payload, local_buff, local_size[my_rank], MPI_Payload, 0, MPI_COMM_WORLD);
	free(a);
	
	for (int i = 0; i < local_size[my_rank]; i++) {
		printf("%s\n", local_buff[i].data);
	}
	
	MPI_Type_free(&MPI_Payload);
	MPI_Finalize();
	return 0;
}

void Get_Size(int *n, char file_name[], int my_rank, int* flag) {
	if (my_rank == 0) {
		char errbuff[PCAP_ERRBUF_SIZE];
		struct pcap_pkthdr header;
		pcap_t *pcap = pcap_open_offline(file_name, errbuff);
		if (pcap == NULL) {	//check error in pcap file opening
			fprintf(stderr, "error reading pcap file: %s\n", errbuff);
			*flag = -1;
		}
		else {
			const unsigned char *packet;
			int count = 0;
			while ((packet = pcap_next(pcap, &header)) != NULL) {
				count++; //we count the number of packets in the pcap file
			}
			*n = count;
			pcap_close(pcap);
		}
	}
	MPI_Bcast(n, 1, MPI_INT, 0, MPI_COMM_WORLD);
	MPI_Bcast(flag, 1, MPI_INT, 0, MPI_COMM_WORLD);
}

void problem_pkt(struct timeval ts, const char *reason) {
	fprintf(stderr, "error: %s\n", reason);

}

void too_short(struct timeval ts, const char *truncated_hdr) { 
	fprintf(stderr, "packetis truncated and lacks a full %s\n", truncated_hdr); 
}

const unsigned char* dump_UDP_packet(const unsigned char *packet, struct timeval ts, unsigned int capture_len) {
	struct ip *ip; //from netinet/ip.h
	//struct UDP_hdr *udp_header;
	unsigned int IP_header_length;

	/* For simplicity, this program assumes Ethernet encapsulation. */

	if (capture_len < sizeof(struct ether_header)) { //if the capture_len is too short
		too_short(ts, "Ethernet header");
		return NULL;
	}

	packet += sizeof(struct ether_header); // Move the packet pointer after the ether_header
	
	capture_len -= sizeof(struct ether_header); // Decrease the capture len yet to be read

	if (capture_len < sizeof(struct ip)) {  //if the capture len is too short to contain the ip
		
		too_short(ts, "IP header");
		return NULL;
	}

	ip = (struct ip*) packet;
	IP_header_length = ip->ip_hl * 4;	// ip_hl is in 4-byte words

	if (capture_len < IP_header_length) {
		too_short(ts, "IP header with options");
		return NULL;
	}

	// now we can check if it's a udp packet
	if (ip->ip_p != IPPROTO_UDP) {
		problem_pkt(ts, "non-UDP packet");
		return NULL;
	}

	
	packet += IP_header_length; //Move the packet pointer after the ip header
	
	capture_len -= IP_header_length; //Decrease the capture len yet to be read

	if (capture_len < sizeof(struct UDP_hdr)) {
		too_short(ts, "UDP header");
		return NULL;
	}

	/*	According to the UDP standard the header is 32 bit lenght
	*	Adding 32 bit to the packet we have a pointer to the payload */
	const unsigned char* payload = packet + 32;
	
	//printf("payload= \n%s\n", payload);
	
	return payload;
}

const unsigned char* dump_TCP_packet(const unsigned char *packet) {
	/* ethernet headers are always exactly 14 bytes */
	#define SIZE_ETHERNET 14

	//const struct sniff_ethernet *ethernet; // The ethernet header 
	const struct sniff_ip *ip; // The IP header 
	const struct sniff_tcp *tcp; // The TCP header 
	const unsigned char* payload; // Packet payload 

	u_int size_ip;
	u_int size_tcp;


	//ethernet = (struct sniff_ethernet*)(packet);
	packet += SIZE_ETHERNET; //move packet pointer adding the ethernet size to get the ip pointer
	
	ip = (struct sniff_ip*)(packet); 
	size_ip = IP_HL(ip)*4;
	if (size_ip < 20) {
		printf("   * Invalid IP header length: %u bytes\n", size_ip);
		return NULL;
	}
	
	packet += size_ip; //move packet pointer adding the ethernet size to get the tcp pointer
	tcp = (struct sniff_tcp*)(packet);
	size_tcp = TH_OFF(tcp)*4;
	if (size_tcp < 20) {
		printf("   * Invalid TCP header length: %u bytes\n", size_tcp);
		return NULL;
	}
	
	packet += size_tcp; //move packet pointer adding the tcp size to get the payload pointer
	payload = (u_char *)(packet);
	
	return payload;

}

	
	
	
		
		
	
		
	
