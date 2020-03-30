
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>

#include "msg.h"
#include "tworker.h"

static struct logFile *log;
static int cmdSock;
static int txSock;
static struct addrinfo hints;
static time_t maxResponseTime = 0, maxPollTime = 0;
static int32_t delay = 0;

inline static void setState(uint32_t state) {
	log->log.txState = state;
}

inline static uint32_t currState() {
	return log->log.txState;
}

static void usage(char *cmd) {
	printf("usage: %s  cmdportNum txportNum\n",
		   cmd);
}

static void setup(int argc, char **argv) {
	if (argc != 3) {
		usage(argv[0]);
		exit(EXIT_FAILURE);
	}
	unsigned long cmdPort;
	unsigned long txPort;

	char *end;
	cmdPort = strtoul(argv[1], &end, 10);
	if (argv[1] == end) {
		printf("Command port conversion error\n");
		exit(EXIT_FAILURE);
	}
	txPort = strtoul(argv[2], &end, 10);
	if (argv[2] == end) {
		printf("Transaction port conversion error\n");
		exit(EXIT_FAILURE);
	}

	char logFileName[128];
	/* got the port number create a logfile name */
	snprintf(logFileName, sizeof(logFileName), "TXworker_%lu.log", cmdPort);

	int logfileFD;

	logfileFD = open(logFileName, O_RDWR | O_CREAT | O_SYNC, S_IRUSR | S_IWUSR);
	if (logfileFD < 0) {
		char msg[256];
		snprintf(msg, sizeof(msg), "Opening %s failed", logFileName);
		perror(msg);
		exit(EXIT_FAILURE);
	}

	// check the logfile size
	struct stat fstatus;
	if (fstat(logfileFD, &fstatus) < 0) {
		perror("Filestat failed");
		exit(EXIT_FAILURE);
	}

	// Let's see if the logfile has some entries in it by checking the size
	if (fstatus.st_size < sizeof(struct logFile)) {
		// Just write out a zeroed file struct
		printf("Initializing the log file size\n");
		struct logFile tx;
		bzero(&tx, sizeof(tx));
		tx.initialized = 1;
		tx.log.txState = WTX_NOTACTIVE;
		if (write(logfileFD, &tx, sizeof(tx)) != sizeof(tx)) {
			printf("Writing problem to log\n");
			exit(EXIT_FAILURE);
		}
	}

	// Now map the file in.
	log = mmap(NULL, 512, PROT_READ | PROT_WRITE, MAP_SHARED, logfileFD, 0);
	if (log == NULL) {
		perror("Log file could not be mapped in:");
		exit(EXIT_FAILURE);
	}

	// Some demo data
	// strncpy(log->txData.IDstring, "Hi there!! ¯\\_(ツ)_/¯", IDLEN);
	// log->txData.A = 10;
	// log->txData.B = 100;
	// log->log.oldA = 83;
	// log->log.newA = 10;
	// log->log.oldB = 100;
	// log->log.newB = 1023;
	// log->initialized = -1;
	// flush();

	// Some demo data
	// strncpy(log->log.newIDstring, "1234567890123456789012345678901234567890", IDLEN);

	cmdSock = socket(AF_INET, SOCK_DGRAM, 0);
	if (cmdSock < 0) {
		perror("Could not open socket");
		exit(EXIT_FAILURE);
	}

	txSock = socket(AF_INET, SOCK_DGRAM, 0);
	if (txSock < 0) {
		perror("Could not open socket");
		exit(EXIT_FAILURE);
	}

	struct sockaddr_in addr;

	memset(&addr, 0, sizeof(addr));
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(cmdPort);
	if (bind(cmdSock, (struct sockaddr*) &addr, sizeof(addr)) < 0) {
		perror("Could not bind socket");
		exit(EXIT_FAILURE);
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(txPort);
	if (bind(txSock, (struct sockaddr*) &addr, sizeof(addr)) < 0) {
		perror("Could not bind socket");
		exit(EXIT_FAILURE);
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_family = AF_INET;
	hints.ai_protocol = IPPROTO_UDP;

	printf("Command port:  %lu\n", cmdPort);
	printf("TX port:       %lu\n", txPort);
	printf("Log file name: %s\n", logFileName);
}

// Flush changes to the log file.
static void flushLog() {
	if (msync(log, sizeof(struct logFile), MS_SYNC | MS_INVALIDATE)) {
		perror("Msync problem");
		exit(-1);
	}
}

static void* receivePacket(int sockfd, void* buf, int buflen, struct sockaddr* sender, socklen_t* addrLen) {
	int res = recvfrom(sockfd, buf, buflen, MSG_DONTWAIT, sender, addrLen);
	if (res == buflen) return buf;
	if (res != -1) printf("Received packet with invalid size: %d\n", res);
	else if (errno != EAGAIN && errno != EWOULDBLOCK) perror("Receive packet error");
	return NULL;
}

/**
 * Check for incoming messages in a non-blocking fashion.
 * Returns a pointer to the command, or NULL if none was present.
 */
static const managerType* receiveMessage() {
	static managerType message;
	// todo: check sender?
	return receivePacket(txSock, &message, sizeof(message), NULL, NULL);
}

/**
 * Check for incoming commands in a non-blocking fashion.
 * Returns a pointer to the command, or NULL if none was present.
 */
static const msgType* receiveCommand() {
	static msgType command;
	return receivePacket(cmdSock, &command, sizeof(command), NULL, NULL);
}

static const char* getManagerTypeString(uint32_t msgType) {
	static const char* names[] = {
		"TXMSG_BEGIN",
		"TXMSG_JOIN",
		"TXMSG_TID_OK",
		"TXMSG_TID_IN_USE",
		"TXMSG_COMMIT_REQUEST",
		"TXMSG_COMMIT_CRASH_REQUEST",
		"TXMSG_ABORT_REQUEST",
		"TXMSG_ABORT_CRASH_REQUEST",
		"TXMSG_PREPARE_TO_COMMIT",
		"TXMSG_VOTE_COMMIT",
		"TXMSG_VOTE_ABORT",
		"TXMSG_COMMITTED",
		"TXMSG_ABORTED"
	};
	return names[msgType - TXMSG_BEGIN];
}

static const char* getCommandTypeString(uint32_t msgType) {
	static const char* names[] = {
		"BEGINTX",
		"JOINTX",
		"NEW_A",
		"NEW_B",
		"NEW_ID",
		"DELAY_RESPONSE",
		"CRASH",
		"COMMIT",
		"COMMIT_CRASH",
		"ABORT",
		"ABORT_CRASH",
		"VOTE_ABORT"
	};
	return names[msgType - BEGINTX];
}

static void printCommand(const msgType* command) {
	printf("[COMMAND] ");
	printf("%s: ", getCommandTypeString(command->msgID));
	printf("tid: %u, ", command->tid);
	printf("port: %u, ", command->port);
	printf("newVal: %d, ", command->newValue);
	printf("delay: %d, ", command->delay);
	printf("str: %s\n", command->strData.newID);
}

static void printMessage(const managerType* msg) {
	printf("[MESSAGE] ");
	printf("%s: ", getManagerTypeString(msg->type));
	printf("tid: %u\n", msg->tid);
}

static void sendMessage(const managerType* msg) {
	struct sockaddr* addr = (struct sockaddr*) &log->log.transactionManager;
	int addrSize = sizeof(log->log.transactionManager);
	if (sendto(txSock, msg, sizeof(*msg), 0, addr, addrSize) != sizeof(*msg)) {
		perror("Error sending message: ");
	}
}

static void initiateTransaction(const msgType* command) {
	static struct addrinfo* serverInfo = NULL;

	if (log->log.txState != WTX_NOTACTIVE) {
		printf("Another transaction is currently ongoing (current status: %d).\n", log->log.txState);
		return;
	}

	char port[10];
	snprintf(port, 10, "%u", command->port);

	if (serverInfo) freeaddrinfo(serverInfo);
	if (getaddrinfo(command->strData.hostName, port, &hints, &serverInfo)) {
		perror("Couldn't look up hostname\n");
		exit(EXIT_FAILURE);
	}

	log->log.txID = command->tid;
	setState(WTX_INITIATED);
	log->log.transactionManager = *((struct sockaddr_in *) serverInfo->ai_addr);

	uint32_t msgType = command->msgID == BEGINTX ? TXMSG_BEGIN : TXMSG_JOIN;
	managerType msg = {command->tid, msgType};
	sendMessage(&msg);
	maxResponseTime = time(NULL) + RESPONSE_TIMEOUT;
}

static void abortTransaction() {
	if (log->log.oldSaved & 1) {
		memcpy(&log->txData.IDstring, &log->log.oldIDstring, IDLEN);
	}
	if ((log->log.oldSaved >> 1) & 1) {
		memcpy(&log->txData.B, &log->log.oldB, sizeof(int));
	}
	if ((log->log.oldSaved >> 2) & 1) {
		memcpy(&log->txData.A, &log->log.oldA, sizeof(int));
	}
	maxResponseTime = 0;
	maxPollTime = 0;
	log->log.txState = WTX_NOTACTIVE;
	log->log.oldSaved = 0;
	flushLog();
}

static void handleCommand(const msgType* command) {
	if (!command) return;
	const int msgType = command->msgID;
	if (msgType < BEGINTX || msgType > VOTE_ABORT) {
		printf("Received invalid command type: %d\n", msgType);
		return;
	}
	printCommand(command);
	switch (command->msgID) {
		case BEGINTX:
			initiateTransaction(command);
			break;
		case JOINTX:
			initiateTransaction(command);
			break;
		case NEW_A:
			break;
		case NEW_B:
			break;
		case NEW_IDSTR:
			break;
		case DELAY_RESPONSE:
			delay = command->delay;
			break;
		case CRASH:
			_exit(EXIT_SUCCESS);
			break;
		case COMMIT:
			break;
		case COMMIT_CRASH:
			break;
		case ABORT:
			break;
		case ABORT_CRASH:
			break;
		case VOTE_ABORT:
			break;
	}
}

static void handleMessage(const managerType* msg) {
	if (!msg) return;
	if (msg->type < TXMSG_BEGIN || msg->type > TXMSG_ABORTED) {
		printf("Received invalid message type: %u\n", msg->type);
		return;
	}
	if (msg->tid != log->log.txID) {
		printf("Mismatching tids - expected: %lu, got: %u", log->log.txID, msg->tid);
		return;
	}
	printMessage(msg);
	switch (msg->type) {
		case TXMSG_TID_OK:
			if (currState() == WTX_INITIATED) {
				maxResponseTime = 0;
				setState(WTX_IN_PROGRESS);
			}
			break;
		case TXMSG_TID_IN_USE:
			if (currState() == WTX_INITIATED) {
				maxResponseTime = 0;
				// todo: do we do something w/ log here?
				printf("TID %u already in use.\n", msg->tid);
				exit(EXIT_FAILURE);
			}
			break;
		case TXMSG_PREPARE_TO_COMMIT:
			if (currState() == WTX_IN_PROGRESS) {
				maxResponseTime = 0;
				// todo: handle delays - push on to queue..
			}
			break;
		case TXMSG_COMMITTED:
			break;
		case TXMSG_ABORTED:
			break;
		default:
			printf("Unexpected message type received from manager.\n");
	}
}

static void checkTimers() {
	const time_t now = time(NULL);
	if (maxResponseTime) {
		if (now > maxResponseTime) {
			printf("Response timeout for transaction %lu.\n", log->log.txID);
			maxResponseTime = 0;
			if (currState() == WTX_INITIATED) {
				setState(WTX_NOTACTIVE);
				// todo not sure if more needs to be done here
			} else if (currState() == WTX_IN_PROGRESS) {
				abortTransaction();
			}
		}
	}
}

int main(int argc, char **argv) {
	setup(argc, argv);

	while (1) {
		handleCommand(receiveCommand());
		handleMessage(receiveMessage());
		checkTimers();
	}
}
