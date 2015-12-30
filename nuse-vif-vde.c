#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <libvdeplug.h>
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
		size = host_read(sock, buf, sizeof(buf));
		if (size < 0) {
			perror("read");
			host_close(sock);
			return;
		} else if (size == 0) {
			host_close(sock);
			return;
		}
		// Cutting the VDE length information (2 Byte at start)
		nuse_dev_rx(dev, buf + 2, size - 2);
	}
}

void
nuse_vif_vde_write(struct nuse_vif *vif, struct SimDevice *dev,
		    unsigned char *data, int len)
{
	int sock = vif->sock;
	// Sending the length information before each packet.
	// This is expected by VDE and must not be omitted.
	unsigned char vde_len[2];
	vde_len[0] = (len >> 8) & 0xFF;
	vde_len[1] = (len) & 0xFF;
	int ret1 = host_write(sock, vde_len, 2);
	int ret2 = host_write(sock, data, len);

	if (ret1 == -1 || ret2 = -1)
		perror ("write");
}

void *
nuse_vif_vde_create(const char *vdepath)
{
	// TODO: Implement for vde, e.g. is pipe.
	// maybe work with vdelib here.

	int sock;
	struct nuse_vif *vif;

	// Create VDE-connection
	// sock = ;
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