#ifndef GUARD_CRYPTO_H
#define GUARD_CRYPTO_H

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <ntp_fp.h>
#include <ntp.h>
#include <ntp_stdlib.h>
#include <ntp_md5.h>	/* provides OpenSSL digest API */
#include "utilities.h"

#define LEN_PKT_MAC	LEN_PKT_NOMAC + sizeof(uint32_t)

struct key {
	struct key *next;
	int key_id;
	int key_len;
	char type[10];
	char key_seq[64];
};

int auth_init(const char *keyfile, struct key **keys);
void get_key(int key_id, struct key **d_key);
int make_mac(char *pkt_data, int pkt_size, int mac_size, struct key *cmp_key, char *digest);
bool auth_md5(char *pkt_data, int pkt_size, int mac_size, struct key *cmp_key);

#endif
