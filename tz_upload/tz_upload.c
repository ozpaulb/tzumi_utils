#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "sha1.h"

#define	TZ_UPDATE_SERVER_IPADDR_DEFAULT		"192.168.1.1"
#define	TZ_UPDATE_SERVER_PORT_DEFAULT		"6667"

#define	TZ_UPDATE_SERVER_RESP_CONNECT_STR	"SerOK"
#define	TZ_UPDATE_SERVER_RESP_UPLOAD_READY_STR	"READY"
#define	TZ_UPDATE_SERVER_RESP_UPLOAD_SHA_GOOD	"CHECK"
#define	TZ_UPDATE_SERVER_RESP_UPLOAD_SHA_BAD	"UCHEC"

#define	TZ_UPDATE_SERVER_UPLOAD_CHUNK_SIZE	1024

#define	TZ_UPDATE_CLIENT_RECV_TIMEOUT_SECONDS	5
#define	TZ_UPDATE_CLIENT_SEND_TIMEOUT_SECONDS	5

#define	IOBUF_LEN			4096

//
// Update client flow:
//
// - connect()
// - send "INFO>filename>filesize>shasum"
// - receive "READY"
// - upload file (server recv's 1024 bytes at a time)
// - receive "CHECK" if shasum OK, "UCHEC" if invalid
// - receive output of install.sh command or "0" when complete
//
//

int	show_server_response(int sockfd);
int	tz_connect(const char *ipaddr, const char *socket);
int	tz_get_server_response(int sockfd, unsigned char *p_buf, size_t sz_buf);
int	tz_upload_file(int sockfd, const char *fname_in, const char *fname_target_in);

int
main(int argc, char *argv[])
{
	int	sockfd = -1;
	const char	*fname_to_send = NULL;
	const char	*fname_on_device = NULL;

	if (argc < 2) {
		fprintf(stderr, "Usage: %s filename.tgz [filename_on_device]\n", argv[0]);
		return 1;
	}
	fname_to_send = argv[1];
	if (argc > 2) {
		fname_on_device = argv[2];
	}

	if ((sockfd = tz_connect(NULL, NULL)) < 0) {
		fprintf(stderr, "failed to connect()!\n");
		return 1;
	}

	printf("Connected OK\n");
	
	if (tz_upload_file(sockfd, fname_to_send, fname_on_device) < 0) {
		fprintf(stderr, "file upload failed!\n");
		close(sockfd);
		return 1;
	}

	printf("File upload success!\n");

	close(sockfd);
	return 0;
}


int
tz_connect(const char *ipaddr_str, const char *port_str)
{
	unsigned int	portnum;
	int	sockfd = -1;
	struct sockaddr_in	serv_addr;
	struct timeval	tv;
	unsigned char	io_buf[IOBUF_LEN+1];

	if (!ipaddr_str) ipaddr_str = TZ_UPDATE_SERVER_IPADDR_DEFAULT;
	if (!port_str) port_str = TZ_UPDATE_SERVER_PORT_DEFAULT;
	
	portnum = strtoul((void *)port_str, NULL, 0);

	if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		fprintf(stderr, "socket() error!\n");
		return 1;
	}

	tv.tv_sec = TZ_UPDATE_CLIENT_RECV_TIMEOUT_SECONDS;
	tv.tv_usec = 0;
	setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));

	tv.tv_sec = TZ_UPDATE_CLIENT_SEND_TIMEOUT_SECONDS;
	tv.tv_usec = 0;
	setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));


	memset((void *)&serv_addr, 0, sizeof(serv_addr));

	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = htons(portnum);

	if (inet_pton(AF_INET, ipaddr_str, &serv_addr.sin_addr) <= 0) {
		fprintf(stderr, "invalid ipaddr_str!\n");
		return -11;
	}

	if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
		fprintf(stderr, "connect() error!\n");
		return -1;
	}
	if (tz_get_server_response(sockfd, (void *)io_buf, sizeof(io_buf)-1) < 0) {
		fprintf(stderr, "server failed to respond on connect()!\n");
		return -1;
	}
	if (strcmp((void *)io_buf, TZ_UPDATE_SERVER_RESP_CONNECT_STR)) {
		fprintf(stderr, "server failed to respond with '%s' response!\n", TZ_UPDATE_SERVER_RESP_CONNECT_STR);
		return -1;
	}
	return sockfd;
}

int
tz_get_server_response(int sockfd, unsigned char *p_buf, size_t sz_buf)
{
	int	n;

	memset((void *)p_buf, 0, sz_buf);

	n = recv(sockfd, p_buf, sz_buf-1, 0);
	if (n > 0) {
		p_buf[n] = '\0';
	}
	return n;
}

int
tz_upload_file(int sockfd, const char *fname_in, const char *fname_target_in)
{
	struct stat	st_buf;
	size_t	filesize;
	unsigned char	io_buf[IOBUF_LEN+1];
	FILE	*fh = NULL;
	int	n;
	int	retval;
	char	fname_target[IOBUF_LEN+1];
	char	sha1_digest_str[SHA1_DIGEST_STRING_LEN+1];
	unsigned char	file_io_chunk[TZ_UPDATE_SERVER_UPLOAD_CHUNK_SIZE];

	if (fname_target_in) {
		// if target filename specified, prepend "../" to get around
		// the fact that the server will prepend "/tmp/" to it.
		//
		sprintf(fname_target, "../%s", fname_target_in);
	} else {
		strcpy(fname_target, "upload.tgz");
	}


	if (stat(fname_in, &st_buf) < 0) {
		fprintf(stderr, "stat('%s') error (file not found?)!\n", fname_in);
		goto exit_err;
	}
	filesize = st_buf.st_size;

	// calculate the SHA1 digest of the file we're sending
	printf("calculate file '%s' sha1sum...\n", fname_in);
	if ((retval = SHA1_file(fname_in, sha1_digest_str)) < 0) {
		fprintf(stderr, "sha1sum('%s') error)!\n", fname_in);
		goto exit_err;
	}

	if (!(fh = fopen(fname_in, "r"))) {
		fprintf(stderr, "fopen() failed!\n");
		goto exit_err;
	}

	sprintf((void *)io_buf, "INFO<%s<%lu<%s", fname_target, filesize, sha1_digest_str);

	if (send(sockfd, (void *)io_buf, strlen((void *)io_buf), 0) != strlen((void *)io_buf)) {
		fprintf(stderr, "send(INFO) error!\n");
		goto exit_err;
	}
	if (tz_get_server_response(sockfd, (void *)io_buf, sizeof(io_buf)-1) < 0) {
		fprintf(stderr, "server failed to respond on INFO!\n");
		goto exit_err;
	}
	if (strcmp((void *)io_buf, TZ_UPDATE_SERVER_RESP_UPLOAD_READY_STR)) {
		fprintf(stderr, "server failed to respond with '%s' response!\n", TZ_UPDATE_SERVER_RESP_UPLOAD_READY_STR);
		goto exit_err;
	}

	printf("sending file '%s' (size=%lu, sha1sum='%s'... ", fname_in, filesize, sha1_digest_str); fflush(stdout);
	while(1) {
		n = fread((void *)file_io_chunk, 1, sizeof(file_io_chunk), fh);
		if (n <= 0) {
			break;
		}
		printf("."); fflush(stdout);
		if (send(sockfd, (void *)file_io_chunk, n, 0) != n) {
			fprintf(stderr, "send(FILE) error!\n");
			goto exit_err;
		}
	}
	printf("\n"); fflush(stdout);

	fclose(fh); fh = NULL;

	if (n < 0) {
		fprintf(stderr, "file read error!\n");
		goto exit_err;
	}

	if (tz_get_server_response(sockfd, (void *)io_buf, sizeof(io_buf)-1) < 0) {
		fprintf(stderr, "server failed to respond on FILE!\n");
		goto exit_err;
	}
	printf("server response: '%s'\n", io_buf);
	if (!strcmp((void *)io_buf, TZ_UPDATE_SERVER_RESP_UPLOAD_SHA_GOOD)) {
		printf("File upload success!\n");
		if (tz_get_server_response(sockfd, (void *)io_buf, sizeof(io_buf)-1) < 0) {
			fprintf(stderr, "server failed to respond on FILE!\n");
			goto exit_err;
		}
		printf("server response after installer: '%s'\n", io_buf);
		goto exit_ok;
	} else if (!strcmp((void *)io_buf, TZ_UPDATE_SERVER_RESP_UPLOAD_SHA_BAD)) {
		printf("File upload failed!\n");
		goto exit_err;
	} else {
		printf("Unknown server response: '%s'\n", io_buf);
	}
	goto exit_err;

exit_ok:
	retval = 0;
	goto exit;
exit_err:
	retval = -1;
	// fallthru
exit:
	if (fh) {
		fclose(fh); fh = NULL;
	}
	return retval;
}
