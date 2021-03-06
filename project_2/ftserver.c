/*
* CS372: Project 2
* FT Server: ftserve.c
* Skyler Wilson
* description:
	* Design and implement a simple FTP service between a server and a client 
* requirements:
*       1. ftserver starts on host A and validates command-line parameters (<SERVER_PORT>)
*       2. ftserver waits on <PORTNUM> for a client request
*       3. ftclient starts on Host B, and validates any pertinent command-line parameters 
	(<SERVER_HOST>, <SERVER_PORT>, <COMMAND>, <FILENAME>, <DATA_PORT>, etc...)
*       4. ftserver and ftclient establist a TCP 'control' connection on <SERVER_PORT>. (call this connection C)
*	5. ftserver waits on connection P for ftclient to send command
*	6. ftclient sends command (-l (list) or -g <FILENAME> (get)) on connection C
*	7. ftserver receives command on connection C
		* if ftclient sent an invalid command:
			* ftserver sends an error message to ftclient on connection C, and
			ftclient displays the message on-screen
		* otherwise:
			* ftserver initiates a TCP 'data' connection with ftclient on
			<DATA_PORT> (call this connection D)
			* if ftclient has sent the -l command, ftserver sends its directory to 
			ftclient on connection D, and ftclient displays the directory on-screen
			* if ftclient has sent -g <FILENAME> command, ftserver validates FILENAME
			and either:
				* sends the contents of FILENAME on connection D, ftclient saves
				the file in the current default directory (handling "duplicate file
				name" error if necessary) and displays a "transfer complete" message
				on-screen
			* or:
				* sends an appropriate error message ("File not found", etc.) to ftclient
				on connection C and ftclient displays the message on-screen
			* ftserver closes connection D (don't leave open sockets!)
* 	8. ftclient closes connection C (don't leave open sockets!)
*	9. ftserver repeats from 2 (above) until terminated by a supervisor (SIGINT)  

        * ftclient is implemented in python
        * ftserver is implemented in c
* references:
       * see the included README.txt file for documentation of the program (chatclient.c)
*/

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define c_portno_min		1024
#define c_portno_max		65535
#define ARG_LEN			8	//number of bytes for command line <COMMAND> argument (-l or -g)
#define MAX_PACK_PAYLOAD_LEN	512	//max number of bytes in packet payload

//The backlog argument specifies the maximum number of queued connections and should be at least 0; 
//the maximum value is system-dependent (usually 5), the minimum value is forced to 0
#define BACKLOG		5



void checkArgs(int agrc, char** argv, int* s_portno);
int checkPortArgInt(char* portArg, int* s_portno);
void ftp(int s_portno);
void intSigHandler(int signal);
int controlConnection(int c_controlfd, char* commandArg, int* dataConnPort, char* filename);
void recvPack(int sockfd, char* option, char* data);
void recvFile(int sockfd, void* buf, int size);
int dataConnection(int controlfd, int datafd, char* commandArg, char* filename);
char** listFiles(char* dirname, int* numFiles);
void sendPack(int sockfd, char* option, char* data);
void sendFile(int sockfd, void* buf, int presetSize);


int main(int argc, char** argv){
	int s_portno;
	checkArgs(argc, argv, &s_portno);
	printf("debug: portno = %d\n", s_portno);
	ftp(s_portno);	

	return 0;
}

/* void checkArgs function: 
	* inputs:
		* int argc -- holds the number of command line argument passed in by user
		* char** argv -- a pointer to pointers of type char -- an array of char pointers (c strings) -- an array of command line arguments
		* int s_portno -- holds the <SERVER_PORT> upon conversion to integer
	* outputs:
		* on success -- void
		* on failure -- exit(1)
	* calls: 
		* int checkPortArg(char* portArg, int* s_portno)
	* purpose:
		* checks the number of command line arguments
		* checks that argument 1 <SERVER_PORT> is an integer 
		* checks that argument 1 <SERVER_PORT> is within range
*/
void checkArgs(int argc, char** argv, int* s_portno){
	if(argc != 2){
		fprintf(stderr, "usage: ./ftserver <SERVER_PORT>\n");
		exit(1);
	}
	
	// check if s_portno argument is an integer
	if(!checkPortArgInt(argv[1], s_portno)){
		fprintf(stderr, "error: ftserver port number must be an integer\n");
		exit(1);
	}

	// check s_portno range [1024, 65535]
	if(*s_portno < c_portno_min || *s_portno > c_portno_max){
		fprintf(stderr, "error: ftserver port number must be between 1024-65535\n");
		exit(1);
	}
}

/* int checkPortArg function: 
	* inputs:
		* char* portArg -- holds the <SERVER_PORT> argument from command line
		* int* s_portno -- pointer to variable that holds <SERVER_PORT> arugment from command line upon conversion to integer
	* outputs:
		* returns 1 if portArg is an integer; returns 0 if portArg is not an integer
	* purpose:
		* to ensure that the command line argument, portArg, is an integer and therefore can be assigned to the server socket
*/
int checkPortArgInt(char* portArg, int* s_portno){
	// this var holds a potential trailing non-whitespace char
	char strCheck;
	
	// sscanf returns the number of variables filled; in case of input failure before any data could be successfully read, EOF is returned
	// in this case, if portno argument is an integer value 1 is returned (1 int variable filled) else 0 returned (0 variables filled)
	int matches = sscanf(portArg, "%d %c", s_portno, &strCheck);

	return matches == 1;
}

/* void ftp(int s_portno)
	* inputs:
		* int s_portno -- holds the <SERVER_PORT> argument from command line upon conversion to integer
	* outputs:
		* on success -- 
		* on failure -- perrer(<error_message>) exit(1)
	* calls:
		* void intSigHandler(int signal)
	* purpose:
		* create server socket; bind server socket with server address and server port number
		* listen for incoming connections on socket
		* provide FTP for client connections
			* create/maintain Control connection
			* create Data connection
		* end FTP on interrupt signal
*/
void ftp(int s_portno){
	//file descriptor for server socket
	int s_sockfd;
	//server socket address
	//The variable serv_addr is a structure of type struct sockaddr_in. This structure has four fields
	struct sockaddr_in serv_addr;
	
		
	//creating the socket
	//arg1 = domain (AF_INET for IP); arg2 = transport protocol type (UDP = SOCK_DGRAM, TCP = SOCK_STREAM); arg3 = protocol (IP = 0)
	//check if socket was created
	if((s_sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1){
		perror("socket creation");
		exit(1);
	}

	// --initialize serv_addr struct: 4 fields -- //

	//zero out serv_addr
	bzero((char*) &serv_addr, sizeof(serv_addr));	

	//The first field is short sin_family, which contains a code 
	//for the address family. It should always be set to the symbolic constant AF_INET: IPv4
	serv_addr.sin_family = AF_INET;
	
	//set port number of socket to portno arg
	//The second field is an unsigned short: sin_port
	//The htons(hostshort) function converts the unsigned short integer hostshort from host byte order to network byte order (i.e., Big Endian conversion)
	serv_addr.sin_port = htons(s_portno);
	
	//The third field is a structure of type struct in_addr which contains only a single field: unsigned long s_addr
	//field: unsigned long s_addr contains the IP address of the host; for a serverthis will always be the IP address of the machine on which the server is running, and there
	//is a symbolic constant INADDR_ANY which gets the address of localhost
	serv_addr.sin_addr.s_addr = INADDR_ANY;

	//bind server socket with host address and server portno
	//int status is an integer that holds return value of bind, listen, connect socket functions for error checking
	int status = bind(s_sockfd, (struct sockaddr*) &serv_addr, sizeof(serv_addr));
	if(status == -1){
		perror("socket binding");
		exit(1);
	}
	
	//listen for incoming connections
	//listen() puts the socket into server mode
	//Listen for connections made to the socket. The backlog argument specifies the maximum number of queued connections and should be at least 0; 
	//the maximum value is system-dependent (usually 5), the minimum value is forced to 0 --  this is a global variable defined a top of document
	status = listen(s_sockfd, BACKLOG);
	if(status == -1){
		perror("server listen");
		exit(1);
	}
	
	
	//use sigaction() to register a signal handling function (that I've created for a specific set of signals)
	//int sigaction(int signo, struct sigaction* newaction, struct sigaction* origaction) -- this is a pointer function field within the sigaction structure
		//1st parameter: the signal type of interest (i.e., that triggers a specific handling function)
		//2nd parameter: a pointer to a special sigaction structure -- describes the action to be taken upon receipt of signal (1st parameter)
			//this sigaction structure has a field called sa.handler (a pointer to a function) and it is assigned the address of the handler function
		//3rd parameter: a pointer to another sigaction structure -- the sigaction() function will use this pointer to write out what the settings were before the change was requested
		// returns 0 on success; -1 on failure -- (this is an assumption)
	//struct sigaction{ void (*sa_handler)(int); sigset_t sa_mask; int sa_flags; void (*sa_sigaction)(int, siginfo_t*, void*); -- sigset_t is a signal set (a list of signal types)
		// void (*sa_handler)(int): 3 possible values:
			// SIG_DFL -- take the default action
			// SIG_IGN -- ignore the signal
			// &<someHandlerFunction> -- a pointer to a function that should be called when this signal is received
		// sigset_t sa_mask: includes what signals should be blocked while the signal handler is executing
			// blocked means that the signals are put on hold until your signal handler is done executing
			// you need to pass this a signal set
		// int sa_flags:  provides additional options (flags)
			// SA_RESTHAND -- resets the signal handler to SIG_DFL (default action) after the first signal has been received and handled 
			// SA_SIGINFO -- tells the kernel to call the function specified in the sigaction struct's 4th field (sa_sigaction), instead of the first field (sa_handler)
				//more detailed information is passed to this function
			// make sure that you set sa_flags to 0 if you aren't planning to set any flags
		// void(*sa_sigaction)(int, siginfo_t*, void*)
			// *sig_action specifies an alternative signal handler function to be called; this attribute will only be used if the SA_SIGINFO flag is set in sa_flags
			// the signinfo_t structure contains information such as which process sent you the signal
			// most of the time you will use sa_handler and not sa_sigaction

	//handles interrupt signals
	struct sigaction interrupt;
	//set handler function to be called (via function pointer)
	interrupt.sa_handler = &intSigHandler;
	//setting NO FLAGS (i.e., 0)
	interrupt.sa_flags = 0;	

	// int sigemptyset(sigset_t* set): initializes the signal set pointed to by parameter 'set', such that all signals signals defined in this document are excluded
		// returns 0 on success; -1 on failure (errno set)
	sigemptyset(&interrupt.sa_mask);
	
	// SIGINT: signal interrupt
		// 1st parameter: setting interrup signal as signal of interest (i.e., signal that triggers handler)
		// 2nd parameter: pointer to the specific sigaction structure which was initialized above
		// 3rd parameter: pointer to another sigaction structure (in this case it is not used; set to 0)
	status = sigaction(SIGINT, &interrupt, 0);
	if(status == -1){
		perror("sigaction");
		exit(1);
	}
		

	//initiate FTP services upon client connection to server socket
	printf("Server open on %d\n", s_portno);
	while(1){
		// initialize client socket vars
		char* clientIP;		//holds client IPv4 dd address
		socklen_t clilen;	//length of sockaddr_in struct client_addr 
		struct sockaddr_in client_addr;
		int c_controlfd, c_datafd;
		int dataConnPort;

		//ftp vars
		char commandArg[ARG_LEN + 1];			//buffer for <COMMAND> arg (-l || -g)
		char filename[MAX_PACK_PAYLOAD_LEN + 1];	//buffer for <FILENAME> arg

		//create FTP control connection
		clilen = sizeof(client_addr);
		c_controlfd = accept(s_sockfd, (struct sockaddr*) &client_addr, &clilen);
		if(c_controlfd == -1){
			perror("accept");
			exit(1);
		}

		//inet_ntoa(struct in_addr in): converts the internet host address called 'in' (clientIP) given in network byte order(Big Endian), to a string in IPv4 dotted-decimal notation. 
			//the string is returned in a statically allocated buffer which subsequent calls will overwrite
		clientIP = inet_ntoa(client_addr.sin_addr);
		printf("control connection established with %s\n", clientIP);
		
		//enable/maintain basic communication via control connection with client
		status = controlConnection(c_controlfd, commandArg, &dataConnPort, filename);

		//once control connection maintained, start FTP services
		if(status != -1){
			int numFTPs; 

			//create data connection
			c_datafd = socket(AF_INET, SOCK_STREAM, 0);
			if(c_datafd == -1){
				perror("c_datafd");
				exit(1);
			}	
			printf("data connection established with %s\n", clientIP);
			
			//start FTP with client
			dataConnection(c_controlfd, c_datafd, commandArg, filename);
			
			//accept ACK from client
			recvPack(c_controlfd, NULL, NULL);

			//close FTP data connection
			status = close(c_datafd);
			if(status == -1){
				perror("close");
				exit(1);
			}
			printf("ftserver: FTP data connection closed\n");
		}
	}	

}

/* void intSigHandler(int signal)
        * inputs:
                * int signal -- holds the signal number/id
        * outputs:
                * on success -- void -- restore signal handling to default behavior; send an interrup signal to force default behavior
                * on failure -- perrer(<error_message>) exit(1)
        * calls:
		* sigaction(int signo, struct sigaction* newaction, struct sigaction* origaction)
		* raise(int signal)
        * purpose:
		* callback function
		* displays feedback before terminating ftserver program due to interrupt signal
		* restore signal handling to default action; send an interrup signal to force default action
			* via setting *sa_handler = SIG_DFL which directs handler to take default action

*/
void intSigHandler(int signal){
	int status;
	struct sigaction interrupt;

	printf("ftserver closed due to interrupt signal\n");

	//restore signal (interrupt) handling to default action via SIG_DFL
	interrupt.sa_handler = SIG_DFL;
	status = sigaction(SIGINT, &interrupt, 0);
	if(status == -1){
		perror("sigaction in handler");
		exit(1);
	}
	
	//send an interrupt signal to force default action -- to be acted on by the newly set default action for this signal
	status = raise(SIGINT);
	if(status == -1){
		perror("raise");
		exit(1);
	}
}

/* int controlConnection(int c_controlfd, char* commandArg, int* dataConnPort, char* filename)
	* inputs:
		* int c_controlfd -- file descriptor of control socket
		* char* commandArg -- pointer/c string (static array) holding <COMMAND> argument from command line (-l || -g) 
		* int* dataConnPort -- holds data connection port file descriptor
		* char* filename -- pointer/c string (static array) holding <FILENAME> argument from command line
	* outputs:
		* 0 on success
			* modifies commandArg, dataConnPort, filename
		* -1 on failure
	* calls:
		* recvPack(int sockfd, char* cmdArg, char* fileArg)
		* sendPack()
	* purpose:
		* read in from client and store command arugments in appropriate static arrays 
		* send feedback to client 
		* establish && maintain basic control connection between server and client
*/
int controlConnection(int c_controlfd, char* commandArg, int* dataConnPort, char* filename){
	char readinFilename[MAX_PACK_PAYLOAD_LEN + 1];		//holds read in client <FILENAME> argument -- input packet payload
	char readinCommand[ARG_LEN + 1];			//holds read in client <COMMAND> argument
	char readinDataPort[MAX_PACK_PAYLOAD_LEN + 1];
	char readoutData[MAX_PACK_PAYLOAD_LEN + 1];
	char readoutOption[ARG_LEN + 1];

	//read in data connection port from client
	recvPack(c_controlfd, readinCommand, readinDataPort);
	//if the command line option held in readinCommand == DPORT, then convert string held in readinDataPort to integer and assign to dataConnPort
	if(strcmp(readinCommand, "DPORT") == 0){
		*dataConnPort = atoi(readinDataPort);
	}
		

	//read in command from client
	recvPack(c_controlfd, readinCommand, readinFilename);
	strcpy(commandArg, readinCommand);
	strcpy(filename, readinFilename);

	//error check
	if(strcmp(readinCommand, "LIST") != 0 && strcmp(readinCommand, "GET") != 0){
		strcpy(readoutOption, "ERROR");
		strcpy(readoutData, "command usage: -l || -g <FILENAME>");
		sendPack(c_controlfd, readoutOption, readoutData);
		return -1;
	}else{
		strcpy(readoutOption, "valid command");
		sendPack(c_controlfd, readoutOption, "");
		return 0;
	}		
}

/* void recvPack(int sockfd, char* option, char* data){
	* inputs:
		* int sockfd -- file descriptor of socket connection to be used for communication
		* char* option -- a string that holds an option from client command line to be received
		* char* data -- a string that holds the data from the client command line to be received
	* outputs:
		* char* option, char* data modified and now hold data read in from client
	* calls:
		* recvFile()
	* purpose:
		* receives data packets from given client socket connection: ref: Beej's Guide to Network Programming: section 7.5, page 53
			* len (1 byte, unsigned) -- total length of the packet, counting the 8-byte command option (-l || -g) and n-bytes data
			* command option (8-bytes)
			* client data (n-bytes)

*/			
void recvPack(int sockfd, char* option, char* data){
	unsigned short packLen;		//holds client packet size
	unsigned short dataLen;		//holds client data (payload) size
	char optionBuf[ARG_LEN + 1];
	char dataBuf[MAX_PACK_PAYLOAD_LEN + 1];

	//read in client packet size: packLen passed by reference because this var will be modified within the recvFile()
	recvFile(sockfd, &packLen, sizeof(&packLen));
	//ntohs(uint16_t netshort): function that converts the unsigned short integer called netshort from network byte (Big Endian) order to host byte order (little Endian)
	packLen = ntohs(packLen);	

	//read in client command 
	recvFile(sockfd, optionBuf, ARG_LEN);
	//set string terminator character to end of optionBuf to mark end of option string
	optionBuf[ARG_LEN] = '\0';
	if(option != NULL){
		strcpy(option, optionBuf);
	}	
	
	//receive data payload from client: dataLen = total packet size - command option - sizeof packLen
	dataLen = packLen - ARG_LEN - sizeof(packLen);
	recvFile(sockfd, dataBuf, dataLen);
	//set string terminator character to end of dataBuf to mark end of data payload string
	dataBuf[dataLen] = '\0';
	if(data != NULL){
		strcpy(data, dataBuf);
	}

}

/* void recvFile(int sockfd, void* buf, int size)
	* inputs: 
		* int sockfd -- file descriptor of the socket connection to be used for FTP
		* void* buf -- this is a generic pointer type which can be converted to any other pointer type without explicit cast: must convert to complete data type before dereferencing or pointer arithmetic
			    -- this buffer will be used to store the client's data
		* int size -- pre determined number of bytes to accept/receive from client
	* outputs:	
		* on success -- client data stored in void* buf
		* on failure -- perror(<error_message>) exit(1)
	* calls:
		* ssize_t recv(int sockfd, void* buf, size_t len, int flags) 
			* sockfd -- specifies the socket file descriptor
			* buffer -- points to a buffer where the message should be stored
			* length -- specifies the length in bytes of the buffer pointed to by the buffer argument
			* flags -- specifies the type of message reception. Values of this argument are formed by logically OR'ing zero or more of specific values 
			* returns -- on success number of bytes received ; on failure -1
				* more data may be coming as long as the return value is greater than 0
				* recv will block if the connection is open but no data is available
				* this function is called until return value is 0 (i.e., all data received from client)
	* purpose:
		* receive client data (either client command of filename to retrieve)
*/
void recvFile(int sockfd, void* buf, int presetSize){
	int recvReturnBytes;		//holds return value of recv()
	int totalRecvBytes = 0;		//holds total number of bytes received from client

	//receive passed in number of bytes from client
	while(totalRecvBytes < presetSize){
		recvReturnBytes = recv(sockfd, buf + totalRecvBytes, presetSize - totalRecvBytes, 0);

		//check recv error; else sum totalRecvBytes
		if(recvReturnBytes == -1){
			perror("recv");
			exit(1);
		}else{
			totalRecvBytes += recvReturnBytes;
		}
	}
}

/* int dataConnection(int controlfd, int datafd, char* commandAgr, char* filename)
	* inputs:
		* int controlfd -- file descriptor of control socket connection
		* int datafd -- file descriptor of data socket connection
		* char* commandArg -- c string that holds the client's command argument
		* char* filename -- c string that holds the client's requested filename
	* outputs:
		* on success -- 0
		* on failure -- -1
	* calls:
		* listFiles()
		* sendPack()
		* fopen(filename, "r/w/rw")
		* fread()
	* purpose:
		* allow file transfer between server and client
*/
int dataConnection(int controlfd, int datafd, char* commandArg, char* filename){
	//an array of c strings that holds the files in the current directory
	char** fileList; 			
	int numFiles;	//number of files in current directory
	int check = 0;
	int i;

	//fill fileList array with current directory files
	fileList = listFiles(".", &numFiles);
	
	//check client's command argument
	//handle <COMMAND> == -l
	if(strcmp(commandArg, "LIST") == 0){
		//transfer each filename of current directory (".") in a packet
		for(i = 0; i < numFiles; i++){
			sendPack(datafd, "FNAME", fileList[i]);
		}
	}else if(strcmp(commandArg, "GET") == 0){
		do{
			//buffer for file contents
			char fileBuf[MAX_PACK_PAYLOAD_LEN + 1];
			int fileBytes;
			int fileStatus;
			//FILE* points to client specified file
			FILE* clifile;
			
			//search the files in current dir
			fileStatus = 0;
			for(i = 0; i < numFiles && !fileStatus; i++){
				if(strcmp(filename, fileList[i]) == 0){
					fileStatus = 1;
				}
			}
			
			//check if <FILENAME> is in current directory
			if(!fileStatus){
				printf("File not found\n");
				sendPack(controlfd, "ERROR", "File not found");
				check = -1;
				break;
			}

			//open file 
			clifile = fopen(filename, "r");
			if(clifile == NULL){
				printf("file read error\n");
				sendPack(controlfd, "ERROR", "File wont open");
				check = -1; 
				break;
			}
				
			//FT the filename
			sendPack(datafd, "FILE", filename);

			//FT the file 
			printf("FT in process\n");
			do{
				fileBytes = fread(fileBuf, sizeof(char), MAX_PACK_PAYLOAD_LEN, clifile);
				//put string terminator at end of fileBuf
				fileBuf[fileBytes] = '\0';
				//send data in fileBuf
				sendPack(datafd, "FILE", fileBuf);
			}while(fileBytes > 0);
			if(ferror(clifile)){
				perror("fread");
				check = -1;
			}
			fclose(clifile);
		}while(0);
	}else{
		check = -1;
	}
	
	//place done tag at end of data to indicate FTP complete 
	sendPack(datafd, "DONE", "");

	//indicate that the control connection is to be closed
	sendPack(controlfd, "CLOSE", "");
		
	//deallocate fileList array
	for(i = 0; i < numFiles; i++){
		free(fileList[i]);
	}
	free(fileList);
	return check;

}

/* char** listFiles(char* dirname, int* numFiles)
	* inputs:
		* char* dirname -- c string directory name
		* int* numFiles -- number of files in the <dirname> directory
	* outputs:
		* char* array of filenames of <dirname> directory
	* calls:
		* DIR* opendir(const char* dirname)
			* open a directory stream corresponding to the directory named by the dirname
			arugment.The directory stream is positioned at the first entry. If the type of DIR
			is implemented using a file descriptor, apps shall only be able to open up to a 
			total of {OPEN_MAX} files and directories.
			* returns a pointer to an object of type DIR; otherwise a null pointer 
			shall be returned and errno set to indicate the error
		* DIR* closedir(DIR* dirp)
			* shall close the directory stream referred to by the argument dirp. Upon
			return, the value of dirp may no longer point to an accessible object of the
			type DIR. If a file descriptor is used to implement type DIR, that file descriptor 
			shall be closed.
			* returns 0 on success; -1 on failure
		* struct dirent* readdir(DIR* dirp)
			* returns a pointer to an object of type struct dirent on success; null pointer 
			and errno set on error
		* int stat(const char* restrict path, struct stat* restrict buf)
			* shall obtain info about the named fil and write it to the area pointed to by the
			buf argument.
			* path -- points to a pathname naming a file
			* buf -- is a pointer to a stat structure, into which info is placed concerning the
			file
		* S_ISDIR(mode_t m) 
			* MACRO returns non-zero if the file is a directory
	* purpose:
		* lists all files in the specified directory
*/
char** listFiles(char* dirname, int* numFiles){
	char** fileList;
	DIR* dir;			//points to open directory
	struct dirent* entry;		//entry within a directory
	struct stat info;		//entry's info		
	*numFiles = 0;
	fileList = NULL;
	
	//open specified directory
	dir = opendir(dirname);
	if(dir == NULL){
		fprintf(stderr, "ftserver: unable to open %s\n", dirname);
		exit(1);
	}

	//store file's in fileList* array
	while((entry = readdir(dir)) != NULL){
		//do not open subdirectories
		stat(entry->d_name, &info);
		if(S_ISDIR(info.st_mode)){
			continue;
		}
		
		//push current filename to fileList array
		{
			if(fileList == NULL){
				//build initial list
				fileList = malloc(sizeof(char*));
			}else{
				//dynamic array growth
				fileList = realloc(fileList, (*numFiles + 1) * sizeof(char*));
			}
			//check fileList array first index error
			assert(fileList != NULL);	
			fileList[*numFiles] = malloc((strlen(entry->d_name) + 1) * sizeof(char));
			//check fileList array last index error
			assert(fileList[*numFiles] != NULL);

			//copy filename into fileList
			strcpy(fileList[*numFiles], entry->d_name);

			//update number of files in fileList
			(*numFiles)++;
		}
	}

	closedir(dir);
	return fileList;
}

/* void sendPack(int sockfd, char* option, char* data);
	* intputs: 
		* int sockfd -- file descriptor for socket connection
		* char* option -- c string for client command option
		* char* data -- c string for client data payload
	* outputs:
		* none
	* calls:
		* sendFile()
	* purpose:
		* sends a packet from the specified socket

*/
void sendPack(int sockfd, char* option, char* data){
        unsigned short packLen;         //holds client packet size
        char optionBuf[ARG_LEN + 1];

	//send packLen
        //ntohs(uint16_t netshort): function that converts the unsigned short integer called netshort from network byte (Big Endian) order to host byte order (little Endian)
        packLen = ntohs(sizeof(packLen) + ARG_LEN + strlen(data));
	sendFile(sockfd, &packLen, sizeof(packLen));
	
	//send command option
	//set the last index of optionBuf to string terminator
	memset(optionBuf, '\0', ARG_LEN);
	strcpy(optionBuf, option);
	sendFile(sockfd, optionBuf, ARG_LEN);

	//send data
	sendFile(sockfd, data, strlen(data));
}

/* void sendFile(int sockfd, void* buffer, int presetSize){
	* inputs:
                * int sockfd -- file descriptor of the socket connection to be used for FTP
                * void* buf -- this is a generic pointer type which can be converted to any other pointer type without explicit cast: must convert to complete data type before dereferencing or pointer arithmetic
                            -- this buffer will be used to store the client's data
                * int size -- pre determined number of bytes to accept/receive from client
	* outputs:
		* none
	* calls:
		* send() -- shall initiate transmission of a message from the specified socket to its peer'
			* returns number of bytes sent on success, -1 on failure 
	* purpose:
		* send data until bytes sent is = to the preset size

*/
void sendFile(int sockfd, void* buffer, int presetSize){
	int sentReturnBytes;            //holds return value of recv()
        int totalSentBytes = 0;         //holds total number of bytes received from client
	
	while(totalSentBytes < presetSize){
		sentReturnBytes = send(sockfd, buffer + totalSentBytes, presetSize - totalSentBytes, 0);
		
		//check error
		if(sentReturnBytes == -1){
			perror("sendFile");
			exit(1);
		}else{
			//update total sent bytes
			totalSentBytes += sentReturnBytes;
		}	
	}
}
