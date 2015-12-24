#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>


void
nuse_vif_vde_read(struct nuse_vif *vif, struct SimDevice *dev)
{
	int sock = vif->sock;
	char buf[8192];
	ssize_t size;

	while (1) {
		// I'm not sure if I got this right, but it seems like this call
		// is only getting the size, the memcp command is executed in 
		// nuse_dev_rx. This means we need to lower the size value by
		// 2 and also apply a offset of 2.
		size = host_read(sock, buf, sizeof(buf));
		if (size < 0) {
			perror("read");
			host_close(sock);
			return;
		} else if (size == 0) {
			host_close(sock);
			return;
		}
		// TODO: change size and buf(offset) accordingly.
		nuse_dev_rx(dev, buf, size);
	}
}

void
nuse_vif_vde_write(struct nuse_vif *vif, struct SimDevice *dev,
		    unsigned char *data, int len)
{
	int sock = vif->sock;
	// TODO:
	//unsigned char vde_data = padding + data
	//int vde_len = len + 2 Byte
	int ret = host_write(sock, data, len);

	if (ret == -1)
		perror ("write");
}

void *
nuse_vif_vde_create(const char *pipepath)
{
	// TODO: Implement for vde, e.g. is pipe.
	// maybe work with vdelib here.

	// int sock;
	// struct nuse_vif *vif;

	// sock = named_pipe_alloc(pipepath);
	// if (sock < 0) {
	// 	printf ("failed to create named pipe \"%s\"\n", pipepath);
	// 	return NULL;
	// }

	// vif = malloc (sizeof(struct nuse_vif));
	// vif->sock = sock;
	// vif->type = NUSE_VIF_PIPE;

	// return vif;
}

void
nuse_vif_vde_delete(struct nuse_vif *vif)
{
	//TODO: Implement. E.g. is for pipees.
	// int sock = vif->sock;
	// free(vif);
	// host_close(sock);
}

static struct nuse_vif_impl nuse_vif_vde = {
	.read = nuse_vif_vde_read,
	.write = nuse_vif_vde_write,
	.create = nuse_vif_vde_create,
	.delete = nuse_vif_vde_delete,
};

extern struct nuse_vif_impl *nuse_vif[NUSE_VIF_MAX];

int __attribute__((constructor))
nuse_vif_vde_init(void)
{
	nuse_vif[NUSE_VIF_VDE] = &nuse_vif_vde;
	return 0;
}