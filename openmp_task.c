/* 	Compilation: gcc -g -Wall -fopenmp openmp_task.c -o openmp_task -lpcap
	Usage: ./openmp_task <file.pcap> thread_number [tcp/udp]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pcap.h>
#include <netinet/ip.h>
#include <netinet/if_ether.h>
#include "timer.h"
#include <omp.h>

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

/* Reports a problem with dumping the packet with the given timestamp. */
void problem_pkt(struct timeval ts, const char *reason);

/* Reports the specific problem of a packet being too short. */
void too_short(struct timeval ts, const char *truncated_hdr);

/* Procedure that reads a UDP packet and prints its payload content */
const unsigned char* dump_UDP_packet(const unsigned char *packet);

/* Procedure that reads a TCP packet and prints its payload content */
const unsigned char* dump_TCP_packet(const unsigned char *packet);

/*Knuth-Morris-Pratt String Matching Algorithm's functions.*/
int kmp_matcher (char text[], char pattern[]);
void kmp_prefix (char pattern[], int *prefix); 

	

int main(int argc, char *argv[]) {
	pcap_t *pcap;	//pointer to the pcap file
	const unsigned char *packet;
	char errbuf[PCAP_ERRBUF_SIZE];
	struct pcap_pkthdr *header;
	char *filepath;
	int thread_count;
	int packet_type = UDP; //default udp
	
	if (argc==3 || argc ==4) { 
		filepath = argv[1]; //get filename from command-line
		thread_count = atoi(argv[2]); //get thread number from command-line
		
		if(argc == 4) { //get packet type from command-line
			if(strcmp(argv[3], "udp") == 0)
				packet_type=UDP;
			else if (strcmp(argv[3], "tcp") == 0)
				packet_type=TCP;
			else {
				printf("USAGE ./serial <file.pcap> thread_number [tcp/udp]\n");
				exit(1);
			}
		}
	}
	else {
		printf("USAGE: ./serial <file.pcap> [tcp/udp]\n");
		exit(1);
	}

	pcap = pcap_open_offline(filepath, errbuf);	//opening the pcap file
	if (pcap == NULL) {	//check error in pcap file
		fprintf(stderr, "error reading pcap file: %s\n", errbuf);
		exit(1);
	}

	int count = 0; //actual number of payloads
	//char **array_of_payloads = malloc(sizeof(char *));
	int array_of_payloads_length = 100; //keeps track of the size of the array of payloads
	char *array_of_payloads[array_of_payloads_length];
	char *S[] = {"http", "Linux", "NOTIFY", "LOCATION"}; //Strings we want to find
	int size_S = 4;
	int *string_count = calloc(size_S, sizeof(int)); //using calloc because we want to initialize every member to 0
	
	int exit_flag = 0;
	int packet_count=0;
	int *private_string_count;
	unsigned char * data_copy; //copy of data object
	int i;
	
	double start = omp_get_wtime();
	
	#pragma omp parallel num_threads(thread_count)
	{
		#pragma omp single //solo il thread 0
		{
			while (exit_flag==0) { //finchè non ho un segnale di uscita
			
				packet_count = 0;
				
				//Per 100 pacchetti o finché non sono finiti i pacchetti
				while (	packet_count<array_of_payloads_length &&  (i = pcap_next_ex(pcap,&header,&packet)) >=0) {
				
					/*
					* Sezione in cui leggo il payload
					*/
					const unsigned char* payload;
					data_copy = malloc(header->len); //allocate memory to copy packet data
					memcpy(data_copy, packet, header->len); 
					if(packet_type == UDP) //udp
						payload = dump_UDP_packet(data_copy); //getting the payload
					else //tcp
						payload = dump_TCP_packet(data_copy); //getting the payload
						
					if(payload != NULL) { //we store it in array of payloads
			
						array_of_payloads[packet_count] = malloc(strlen((char *)payload)+1); //we have to allocate memory for storing this payload
						memcpy(array_of_payloads[packet_count], payload,  strlen((char *)payload)); //copy payload into array
						count++;
					}
					else { // If the packet is not valid we save a " " message into array of payloads
						array_of_payloads[packet_count] = malloc(1);
						memcpy(array_of_payloads[packet_count], " ", 1);
					}
					packet_count++; //aumenta il contatore
														
				}

				
				#pragma omp task firstprivate(array_of_payloads, packet_count) private(private_string_count) shared(string_count, size_S, S)
				{

				 	private_string_count = calloc(size_S, sizeof(int)); // Using calloc because we want to initialize every member to 0
				 	
				 	//Utilizzo l'algoritmo per ogni pacchetto che mi è stato passato
				 	for (int k = 0; k < packet_count; k++) //for every payload
						for (int i =0 ; i < size_S; i++) //for every string 
							private_string_count[i] += kmp_matcher((char*)array_of_payloads[k],S[i]);
								
	
				 	// Merge private string count into shared string count array
					#pragma omp critical
					{
					for (int i = 0; i < size_S; i++)
						string_count[i]+=private_string_count[i];
					}
			 	
				 	free(private_string_count);
				}
				
				//se i pacchetti erano finiti
				if (packet_count<100)
					exit_flag = 1;
					
			} //end of while cicle
		} //end of single pragma
	} //end of parallel pragma
	
	double finish = omp_get_wtime();
	
	/* Now we print the output */
	printf("Printing the number of appereances of each string throughout the entire pcap file:\n");
	for (int i = 0; i < size_S; i++)
		printf("%s: %d times!\n", S[i], string_count[i]);
		
	// Now we print performance evaluation 
	printf("Elapsed time = %f seconds\n", finish-start);

	return 0;
}


void problem_pkt(struct timeval ts, const char *reason) {
	//fprintf(stderr, "error: %s\n", reason);

}

void too_short(struct timeval ts, const char *truncated_hdr) { 
	fprintf(stderr, "packetis truncated and lacks a full %s\n", truncated_hdr); 
}

const unsigned char* dump_UDP_packet(const unsigned char *packet) {

	const unsigned char* payload = packet + 42;
	
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


int kmp_matcher (char text[], char pattern[]) {
	int text_len = strlen(text);
	int pattern_len = strlen(pattern);
	if (text_len < pattern_len) //no point trying to match things
		return 0;
	int *prefix_array = malloc(pattern_len*sizeof(int));
	kmp_prefix(pattern, prefix_array);
	int i = 0; 
	int j = 0;
	int occurrences = 0; //counter for the number of occurrences of pattern in text
	while (i < text_len) {
		if (pattern[j] == text[i]) {
			j++;
			i++;
		}
		if (j == pattern_len) { //we have a match
			occurrences++;
			j = prefix_array[j-1]; //look for next match
		}
		else if (i < text_len && pattern[j] != text[i]) {
			if (j != 0)
				j = prefix_array[j-1];
			else
				i++;
		}
	}
	
	free(prefix_array);
	return occurrences;
}

void kmp_prefix (char pattern[], int *prefix) {
	int pattern_len = strlen(pattern);
	int j = 0;
	prefix[0] = 0; //first letter does not have any prefix
	int i = 1;
	while (i < pattern_len) {
		if (pattern[i] == pattern[j]){
			prefix[i] = j + 1;
			j++;
			i++;
		}
		else if (j != 0) {
			j = prefix[j-1];
		}
		else {
			prefix[i] = 0;
			i++;
		}
	}
}