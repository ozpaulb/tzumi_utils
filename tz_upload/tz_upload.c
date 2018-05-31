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

#define	TZ_UPDATE_SERVER_IPADDR_DEFAULT		"192.168.1.1"
#define	TZ_UPDATE_SERVER_PORT_DEFAULT		"6667"

#define	TZ_UPDATE_SERVER_RESP_CONNECT_STR	"SerOK"
#define	TZ_UPDATE_SERVER_RESP_UPLOAD_READY_STR	"READY"
#define	TZ_UPDATE_SERVER_RESP_UPLOAD_SHA_GOOD	"CHECK"
#define	TZ_UPDATE_SERVER_RESP_UPLOAD_SHA_BAD	"UCHEC"


#define	TZ_UPDATE_CLIENT_RECV_TIMEOUT_SECONDS	5
#define	TZ_UPDATE_CLIENT_SEND_TIMEOUT_SECONDS	5

#define	SHA1SUM_STRING_LENGTH		40

//
// Update client flow:
//
// - connect()
// - send "INFO>filename>filesize>shasum"
// - receive "READY"
// - upload file 1024 bytes at a time
// - receive "CHECK" if shasum OK, "UCHEC" if invalid
// - receive output of install.sh command or "0" when complete
//
//

int	show_server_response(int sockfd);
int	tz_connect(const char *ipaddr, const char *socket);
int	tz_get_server_response(int sockfd, unsigned char *p_buf, size_t sz_buf);
int	tz_upload_file(int sockfd, const char *fname_in, const char *fname_target);

int
main(int argc, char *argv[])
{
	int	sockfd = -1;

	if ((sockfd = tz_connect(NULL, NULL)) < 0) {
		fprintf(stderr, "failed to connect()!\n");
		return 1;
	}

	printf("Connected OK\n");
	
	if (tz_upload_file(sockfd, "./installer.tgz", NULL) < 0) {
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
	unsigned char	io_buf[1024+1];

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
	if (strcmp(io_buf, TZ_UPDATE_SERVER_RESP_CONNECT_STR)) {
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
tz_upload_file(int sockfd, const char *fname_in, const char *fname_target)
{
	struct stat	st_buf;
	size_t	filesize;
	unsigned char	io_buf[1024+1];
	char	sha_buf[SHA1SUM_STRING_LENGTH+1];
	FILE	*fh = NULL;
	int	n;
	unsigned char	*p_filebuf = NULL;
	int	retval;

	if (!fname_target) fname_target = "upload.tgz";

	if (stat(fname_in, &st_buf) < 0) {
		fprintf(stderr, "stat('%s') error (file not found?)!\n", fname_in);
		goto exit_err;
	}
	filesize = st_buf.st_size;
	if (!(fh = fopen(fname_in, "r"))) {
		fprintf(stderr, "fopen() failed!\n");
		goto exit_err;
	}
	if (!(p_filebuf = (unsigned char *)malloc(filesize))) {
		fprintf(stderr, "malloc() failed!\n");
		goto exit_err;
	}
	if (fread((void *)p_filebuf, filesize, 1, fh) != 1) {
		fprintf(stderr, "fread() failed!\n");
		goto exit_err;
	}
	fclose(fh); fh = NULL;

	sprintf(io_buf, "sha1sum %s", fname_in);

	if (!(fh = popen(io_buf, "r"))) {
		fprintf(stderr, "popen() failed!\n");
		goto exit_err;
	}
	io_buf[0] = '\0';
	if (!fgets(io_buf, sizeof(io_buf)-1, fh)) {
		fprintf(stderr, "fgets() error!\n");
		goto exit_err;
	}
	fclose(fh); fh = NULL;

	if (strlen(io_buf) < SHA1SUM_STRING_LENGTH) {
		fprintf(stderr, "sha1sum result error!\n");
		goto exit_err;
	}
	strncpy(sha_buf, io_buf, SHA1SUM_STRING_LENGTH);
	sha_buf[SHA1SUM_STRING_LENGTH] = '\0';
	
	sprintf(io_buf, "INFO<%s<%lu<%s", fname_target, filesize, sha_buf);

	if (send(sockfd, (void *)io_buf, strlen((void *)io_buf), 0) != strlen((void *)io_buf)) {
		fprintf(stderr, "send(INFO) error!\n");
		goto exit_err;
	}
	if (tz_get_server_response(sockfd, (void *)io_buf, sizeof(io_buf)-1) < 0) {
		fprintf(stderr, "server failed to respond on INFO!\n");
		goto exit_err;
	}
	if (strcmp(io_buf, TZ_UPDATE_SERVER_RESP_UPLOAD_READY_STR)) {
		fprintf(stderr, "server failed to respond with '%s' response!\n", TZ_UPDATE_SERVER_RESP_UPLOAD_READY_STR);
		goto exit_err;
	}

	printf("sending file...\n");
	if (send(sockfd, (void *)p_filebuf, filesize, 0) != filesize) {
		fprintf(stderr, "send(FILE) error!\n");
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
	}
	printf("fail for now\n");
	goto exit_err;

exit_ok:
	retval = 0;
	goto exit;
exit_err:
	retval = -1;
	// fallthru
exit:
	if (p_filebuf) {
		free((void *)p_filebuf); p_filebuf = NULL;
	}
	if (fh) {
		fclose(fh); fh = NULL;
	}
	return retval;
}
