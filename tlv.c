#include <string.h>
#include "pbuffer.h"
#include "tlv.h"
#include "logging.h"

#define DB(fmt, args...) debug(3, "[tlv]: " fmt, ##args)
#define DBERR(fmt, args...) debug(0, "[tlv]: " fmt, ##args)

const char *T_NAMES[T_NUM] = {
	[T_TAG] = "TAG",
	[T_PROTOCOL] = "PROTOCOL",
	[T_SRC] = "SOURCE",
	[T_DST] = "DESTINATION",
	[T_PAYLOAD] = "PAYLOAD",
};

const char *PT_NAMES[PT_NUM] = {
	[PT_FAMILY] = "FAMILY",
	[PT_IPADDR] = "IP ADDRESS",
	[PT_PORT] = "PORT",
};

static unsigned char extract_byte(pbuffer *b)
{
	unsigned char holder;
	pbuffer_safe_extract(b, &holder, 1);
	return holder;
}

static void ip_to_buffer(pbuffer *b, unsigned char *addr, size_t len)
{
	size_t i;
	pbuffer_assure(b, b->length + len);
	for (i = 0; i < len; i++) {
		pbuffer_add(b, &addr[i], 1);
	}
}

static void extract_ip(struct psockaddr *psa, pbuffer *b, size_t len)
{
	if (len == 4) {
	} else if (len == 16) {
	} else {
		DBERR("Cannot convert IP address (length != 4/16)");
	}
}

static size_t psockaddr_to_tlv(struct psockaddr *psa, pbuffer *b)
{
	struct tlv *tlv = tlv_init();
	unsigned char holder;
	size_t length = b->length;

	if (psa->af) {
		tlv->type = PT_FAMILY;
		tlv->length = 1;
		holder = psa->af & 0xff;
		pbuffer_add(tlv->value, &holder, 1);
		tlv_to_buffer(tlv, b);
		tlv_clear(tlv);
	}

	if (psa->af == AF_INET) {
		tlv->type = PT_IPADDR;
		tlv->length = 4;
		ip_to_buffer(tlv->value, (unsigned char *)&psa->v4.sin_addr,
			     tlv->length);
		tlv_to_buffer(tlv, b);
	} else if (psa->af == AF_INET6) {
		tlv->type = PT_IPADDR;
		tlv->length = 16;
		ip_to_buffer(tlv->value, (unsigned char *)&psa->v6.sin6_addr,
			     tlv->length);
		tlv_to_buffer(tlv, b);
	}
	tlv_clear(tlv);

	if (psa->v6.sin6_port) {
		tlv->type = PT_PORT;
		tlv->length = sizeof(uint16_t);
		pbuffer_add(tlv->value, &(psa->v6.sin6_port),
			    tlv->length);
		tlv_to_buffer(tlv, b);
		tlv_clear(tlv);
	}

	return b->length - length;
}

static void tlv_to_psockaddr(pbuffer *b, struct psockaddr *psa)
{
	struct tlv *tlv = tlv_init();
	while (b->length) {
		buffer_to_tlv(b, tlv);
		switch (tlv->type) {
		case PT_FAMILY:
			psa->af = extract_byte(tlv->value);
			break;
		case PT_IPADDR:
		case PT_PORT:
			break;
		}
	}
	tlv_free(tlv);
	return;
}

void tlv_parse_tags(pbuffer *b, struct forward_header *fh)
{
	struct tlv *tlv = tlv_init();
	while (b->length) {
		buffer_to_tlv(b, tlv);
		switch (tlv->type) {
		case T_TAG:
			if (tlv->value->length > MAX_TAG)
				return;
			strncpy(fh->tag, tlv->value->data, MAX_TAG);
			DB("Found tag: %s", fh->tag);
			break;
		case T_PROTOCOL:
			fh->protocol = extract_byte(tlv->value);
			break;
		case T_SRC:
			tlv_to_psockaddr(tlv->value, &fh->src);
			break;
		case T_DST:
			tlv_to_psockaddr(tlv->value, &fh->dst);
			break;
		case T_PAYLOAD:
			/* found payload */
			break;
		}
	}
	tlv_free(tlv);
}

void tlv_generate_tags(struct forward_header *fh, pbuffer *b)
{
	struct tlv *tlv = tlv_init();

	if (fh->tag) {
		tlv->type = T_TAG;
		tlv->length = strlen(fh->tag);
		pbuffer_add(tlv->value, fh->tag, tlv->length);
		tlv_to_buffer(tlv, b);
		tlv_clear(tlv);
	}

	if (fh->protocol) {
		tlv->type = T_PROTOCOL;
		tlv->length = 1;
		pbuffer_add(tlv->value, &fh->protocol, 1);
		tlv_to_buffer(tlv, b);
		tlv_clear(tlv);
	}

	if (fh->src.af) {
		tlv->type = T_SRC;
		tlv->length = psockaddr_to_tlv(&fh->src, tlv->value);
		tlv_to_buffer(tlv, b);
		tlv_clear(tlv);
	}

	if (fh->payload->length) {
		tlv->type = T_PAYLOAD;
		tlv->length = fh->payload->length;
		pbuffer_copy(tlv->value, fh->payload, tlv->length);
		tlv_to_buffer(tlv, b);
		tlv_clear(tlv);
	}
	tlv_free(tlv);
}

static unsigned int count_shift(unsigned int num)
{
	unsigned int count = 0;
	while (num > (TLV_EXTEND - 1)) {
		count++;
		num = num >> 7;
	}
	return count;
}

int tlv_to_buffer(struct tlv *tlv, pbuffer *buffer)
{
	char holder = 0;
	unsigned int origlen = tlv->length;
	unsigned int t_shift, l_shift;

	t_shift = count_shift(tlv->type);
	l_shift = count_shift(tlv->length);
	/* set type */
	while (t_shift > 0) {
		holder |= ((tlv->type >> (7*t_shift)) | TLV_EXTEND);
		pbuffer_add(buffer, &holder, 1);
		holder = 0;
		t_shift--;
	}
	holder |= tlv->type & (TLV_EXTEND - 1);
	pbuffer_add(buffer, &holder, 1);

	/* set length */
	while (l_shift > 0) {
		holder |= ((tlv->length >> (7*l_shift)) | TLV_EXTEND);
		pbuffer_add(buffer, &holder, 1);
		holder = 0;
		l_shift--;
	}
	holder = tlv->length;
	pbuffer_add(buffer, &holder, 1);

	pbuffer_add(buffer, tlv->value->data, origlen);
	return 0;
}

/* extract the type or value from buffer into dest */
size_t extract_torv(pbuffer *buffer, unsigned int *dest)
{
	unsigned int tmp = 0;
	unsigned int holder = 0;
	size_t bytes = 0;
	while ((holder = extract_byte(buffer))) {
		bytes++;
		if (holder & TLV_EXTEND) {
			holder &= ~TLV_EXTEND;
			tmp |= holder;
			tmp = tmp << 7;
		} else {
			tmp |= holder;
			break;
		}
	}
	*dest = tmp;
	return bytes;
}

void buffer_to_tlv(pbuffer *buffer, struct tlv *tlv)
{
	size_t bytes;
	bytes = extract_torv(buffer, &tlv->type);
	pbuffer_shift(buffer, bytes);

	bytes = extract_torv(buffer, &tlv->length);
	pbuffer_shift(buffer, bytes);

	tlv->value = buffer;
}

struct tlv *tlv_init(void)
{
	struct tlv *tmp;
	tmp = malloc(sizeof(struct tlv));
	tmp->value = pbuffer_init();
	return tmp;
}

void tlv_free(struct tlv *tlv)
{
	pbuffer_free(tlv->value);
	free(tlv);
}
