#include "bencode.h"
#include <stdio.h>
#include <sys/uio.h>
#include <unistd.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <ctype.h>
#include <json-glib/json-glib.h>
#include "helpers.h"

/* set to 0 for alloc debugging, e.g. through valgrind */
#define BENCODE_MIN_BUFFER_PIECE_LEN	512

#define BENCODE_HASH_BUCKETS		31 /* prime numbers work best */

#define BENCODE_ALLOC_ALIGN		8

struct __bencode_buffer_piece {
	char *tail;
	unsigned int left;
	struct __bencode_buffer_piece *next;
	char buf[0];
};
struct __bencode_free_list {
	void *ptr;
	free_func_t func;
	struct __bencode_free_list *next;
};
struct __bencode_hash {
	struct bencode_item *buckets[BENCODE_HASH_BUCKETS];
};





static bencode_item_t __bencode_end_marker = {
	.type = BENCODE_END_MARKER,
	.iov = {
		{
			.iov_base = "e",
			.iov_len = 1,
		},
	},
	.iov_cnt = 1,
	.str_len = 1,
};




static bencode_item_t *__bencode_decode(bencode_buffer_t *buf, const char *s, const char *end);



static void __bencode_item_init(bencode_item_t *item) {
	item->last_child = item->parent = item->child = item->sibling = NULL;
}

static void __bencode_container_init(bencode_item_t *cont) {
	cont->iov[0].iov_len = 1;
	cont->iov[1].iov_base = "e";
	cont->iov[1].iov_len = 1;
	cont->iov_cnt = 2;
	cont->str_len = 2;
}

static void __bencode_dictionary_init(bencode_item_t *dict) {
	dict->type = BENCODE_DICTIONARY;
	dict->iov[0].iov_base = "d";
	dict->value = 0;
	__bencode_container_init(dict);
}

static void __bencode_list_init(bencode_item_t *list) {
	list->type = BENCODE_LIST;
	list->iov[0].iov_base = "l";
	__bencode_container_init(list);
}

static struct __bencode_buffer_piece *__bencode_piece_new(unsigned int size) {
	struct __bencode_buffer_piece *ret;

	if (size < BENCODE_MIN_BUFFER_PIECE_LEN)
		size = BENCODE_MIN_BUFFER_PIECE_LEN;
	ret = BENCODE_MALLOC(sizeof(*ret) + size + BENCODE_ALLOC_ALIGN);
	if (!ret)
		return NULL;

	ret->tail = ret->buf;
	ret->left = size;
	ret->next = NULL;

	return ret;
}

int bencode_buffer_init(bencode_buffer_t *buf) {
	buf->pieces = __bencode_piece_new(0);
	if (!buf->pieces)
		return -1;
	buf->free_list = NULL;
	buf->error = 0;
	return 0;
}

void *bencode_buffer_alloc(bencode_buffer_t *buf, unsigned int size) {
	struct __bencode_buffer_piece *piece;
	void *ret;
	unsigned int align_size = ((size + BENCODE_ALLOC_ALIGN - 1) / BENCODE_ALLOC_ALIGN) * BENCODE_ALLOC_ALIGN;

	if (!buf)
		return NULL;
	if (buf->error)
		return NULL;

	piece = buf->pieces;

	if (size <= piece->left)
		goto alloc;

	piece = __bencode_piece_new(size);
	if (!piece) {
		buf->error = 1;
		return NULL;
	}
	piece->next = buf->pieces;
	buf->pieces = piece;

	assert(size <= piece->left);

alloc:
	if (piece->left >= align_size)
		piece->left -= align_size;
	else
		piece->left = 0;
	ret = piece->tail;
	piece->tail += align_size;
	return ret;
}

void bencode_buffer_free(bencode_buffer_t *buf) {
	struct __bencode_free_list *fl;
	struct __bencode_buffer_piece *piece, *next;

	for (fl = buf->free_list; fl; fl = fl->next)
		fl->func(fl->ptr);

	for (piece = buf->pieces; piece; piece = next) {
		next = piece->next;
		BENCODE_FREE(piece);
	}
}

static bencode_item_t *__bencode_item_alloc(bencode_buffer_t *buf, unsigned int payload) {
	bencode_item_t *ret;

	ret = bencode_buffer_alloc(buf, sizeof(struct bencode_item) + payload);
	if (!ret)
		return NULL;
	ret->buffer = buf;
	__bencode_item_init(ret);
	return ret;
}

bencode_item_t *bencode_dictionary(bencode_buffer_t *buf) {
	bencode_item_t *ret;

	ret = __bencode_item_alloc(buf, 0);
	if (!ret)
		return NULL;
	__bencode_dictionary_init(ret);
	return ret;
}

bencode_item_t *bencode_list(bencode_buffer_t *buf) {
	bencode_item_t *ret;

	ret = __bencode_item_alloc(buf, 0);
	if (!ret)
		return NULL;
	__bencode_list_init(ret);
	return ret;
}

static void __bencode_container_add(bencode_item_t *parent, bencode_item_t *child) {
	if (!parent)
		return;
	if (!child)
		return;

	assert(child->parent == NULL);
	assert(child->sibling == NULL);

	child->parent = parent;
	if (parent->last_child)
		parent->last_child->sibling = child;
	parent->last_child = child;
	if (!parent->child)
		parent->child = child;

	while (parent) {
		parent->iov_cnt += child->iov_cnt;
		parent->str_len += child->str_len;
		parent = parent->parent;
	}
}

static bencode_item_t *__bencode_string_alloc(bencode_buffer_t *buf, const void *base,
		size_t str_len, size_t iov_len, unsigned int iov_cnt, bencode_type_t type)
{
	bencode_item_t *ret;
	int len_len;

	assert(str_len <= 99999);
	ret = __bencode_item_alloc(buf, 7);
	if (!ret)
		return NULL;
	len_len = sprintf(ret->__buf, "%zu:", str_len);

	ret->type = type;
	ret->iov[0].iov_base = ret->__buf;
	ret->iov[0].iov_len = len_len;
	ret->iov[1].iov_base = (void *) base;
	ret->iov[1].iov_len = iov_len;
	ret->iov_cnt = iov_cnt + 1;
	ret->str_len = len_len + str_len;

	return ret;
}

bencode_item_t *bencode_string_len_dup(bencode_buffer_t *buf, const char *s, size_t len) {
	char *sd = bencode_buffer_alloc(buf, len);
	if (!sd)
		return NULL;
	memcpy(sd, s, len);
	return bencode_string_len(buf, sd, len);
}

bencode_item_t *bencode_string_len(bencode_buffer_t *buf, const char *s, size_t len) {
	return __bencode_string_alloc(buf, s, len, len, 1, BENCODE_STRING);
}

bencode_item_t *bencode_integer(bencode_buffer_t *buf, long long int i) {
	bencode_item_t *ret;
	int alen, rlen;

	alen = 8;
	while (1) {
		ret = __bencode_item_alloc(buf, alen + 3);
		if (!ret)
			return NULL;
		rlen = snprintf(ret->__buf, alen, "i%llde", i);
		if (rlen < alen)
			break;
		alen <<= 1;
	}

	ret->type = BENCODE_INTEGER;
	ret->iov[0].iov_base = ret->__buf;
	ret->iov[0].iov_len = rlen;
	ret->iov[1].iov_base = NULL;
	ret->iov[1].iov_len = 0;
	ret->iov_cnt = 1;
	ret->str_len = rlen;
	ret->value = i;

	return ret;
}

bencode_item_t *bencode_dictionary_add_len(bencode_item_t *dict, const char *key, size_t keylen, bencode_item_t *val) {
	bencode_item_t *s;

	if (!dict || !val)
		return NULL;
	assert(dict->type == BENCODE_DICTIONARY);

	s = bencode_string_len(dict->buffer, key, keylen);
	if (!s)
		return NULL;
	__bencode_container_add(dict, s);
	__bencode_container_add(dict, val);
	return val;
}

bencode_item_t *bencode_list_add(bencode_item_t *list, bencode_item_t *item) {
	if (!list || !item)
		return NULL;
	assert(list->type == BENCODE_LIST);
	__bencode_container_add(list, item);
	return item;
}

static int __bencode_iovec_cpy(struct iovec *out, const struct iovec *in, int num) {
	memcpy(out, in, num * sizeof(*out));
	return num;
}

static int __bencode_str_cpy(char *out, const struct iovec *in, int num) {
	char *orig = out;

	while (--num >= 0) {
		memcpy(out, in->iov_base, in->iov_len);
		out += in->iov_len;
		in++;
	}
	return out - orig;
}

static int __bencode_iovec_dump(struct iovec *out, bencode_item_t *item) {
	bencode_item_t *child;
	struct iovec *orig = out;

	assert(item->iov[0].iov_base != NULL);
	out += __bencode_iovec_cpy(out, &item->iov[0], 1);

	child = item->child;
	while (child) {
		out += __bencode_iovec_dump(out, child);
		child = child->sibling;
	}

	if (item->iov[1].iov_base)
		out += __bencode_iovec_cpy(out, &item->iov[1], 1);

	assert((out - orig) == item->iov_cnt);
	(void) orig;
	return item->iov_cnt;
}

static size_t __bencode_str_dump(char *out, bencode_item_t *item) {
	char *orig = out;
	bencode_item_t *child;

	assert(item->iov[0].iov_base != NULL);
	out += __bencode_str_cpy(out, &item->iov[0], 1);

	child = item->child;
	while (child) {
		out += __bencode_str_dump(out, child);
		child = child->sibling;
	}

	if (item->iov[1].iov_base)
		out += __bencode_str_cpy(out, &item->iov[1], 1);

	assert((out - orig) == item->str_len);
	(void) orig;
	*out = '\0';
	return item->str_len;
}

struct iovec *bencode_iovec(bencode_item_t *root, int *cnt, unsigned int head, unsigned int tail) {
	struct iovec *ret;

	if (!root)
		return NULL;
	assert(cnt != NULL);
	assert(root->iov_cnt > 0);

	ret = bencode_buffer_alloc(root->buffer, sizeof(*ret) * (root->iov_cnt + head + tail));
	if (!ret)
		return NULL;
	*cnt = __bencode_iovec_dump(ret + head, root);
	return ret;
}

char *bencode_collapse(bencode_item_t *root, size_t *len) {
	char *ret;
	size_t l;

	if (!root)
		return NULL;
	assert(root->str_len > 0);

	ret = bencode_buffer_alloc(root->buffer, root->str_len + 1);
	if (!ret)
		return NULL;
	l = __bencode_str_dump(ret, root);
	if (len)
		*len = l;
	return ret;
}

char *bencode_collapse_dup(bencode_item_t *root, size_t *len) {
	char *ret;
	int l;

	if (!root)
		return NULL;
	assert(root->str_len > 0);

	ret = BENCODE_MALLOC(root->str_len + 1);
	if (!ret)
		return NULL;

	l = __bencode_str_dump(ret, root);
	if (len)
		*len = l;
	return ret;
}

static unsigned int __bencode_hash_str_len(const unsigned char *s, int len) {
	unsigned long ul;
	unsigned int ui;
	unsigned short us;

	if (len >= sizeof(ul)) {
		memcpy(&ul, s, sizeof(ul));
		return ul % BENCODE_HASH_BUCKETS;
	}
	if (len >= sizeof(ui)) {
		memcpy(&ui, s, sizeof(ui));
		return ui % BENCODE_HASH_BUCKETS;
	}
	if (len >= sizeof(us)) {
		memcpy(&us, s, sizeof(us));
		return us % BENCODE_HASH_BUCKETS;
	}
	if (len >= sizeof(*s))
		return *s % BENCODE_HASH_BUCKETS;

	return 0;
}

static unsigned int __bencode_hash_str(bencode_item_t *s) {
	assert(s->type == BENCODE_STRING);
	return __bencode_hash_str_len(s->iov[1].iov_base, s->iov[1].iov_len);
}

static void __bencode_hash_insert(bencode_item_t *key, struct __bencode_hash *hash) {
	unsigned int bucket, i;

	i = bucket = __bencode_hash_str(key);

	while (1) {
		if (!hash->buckets[i]) {
			hash->buckets[i] = key;
			break;
		}
		i++;
		if (i >= BENCODE_HASH_BUCKETS)
			i = 0;
		if (i == bucket)
			break;
	}
}

static bencode_item_t *__bencode_decode_dictionary(bencode_buffer_t *buf, const char *s, const char *end) {
	bencode_item_t *ret, *key, *value;
	struct __bencode_hash *hash;

	if (*s != 'd')
		return NULL;
	s++;

	ret = __bencode_item_alloc(buf, sizeof(*hash));
	if (!ret)
		return NULL;
	__bencode_dictionary_init(ret);
	ret->value = 1;
	hash = (void *) ret->__buf;
	memset(hash, 0, sizeof(*hash));

	while (s < end) {
		key = __bencode_decode(buf, s, end);
		if (!key)
			return NULL;
		s += key->str_len;
		if (key->type == BENCODE_END_MARKER)
			break;
		if (key->type != BENCODE_STRING)
			return NULL;
		__bencode_container_add(ret, key);

		if (s >= end)
			return NULL;
		value = __bencode_decode(buf, s, end);
		if (!value)
			return NULL;
		s += value->str_len;
		if (value->type == BENCODE_END_MARKER)
			return NULL;
		__bencode_container_add(ret, value);

		__bencode_hash_insert(key, hash);
	}

	return ret;
}

static bencode_item_t *__bencode_decode_list(bencode_buffer_t *buf, const char *s, const char *end) {
	bencode_item_t *ret, *item;

	if (*s != 'l')
		return NULL;
	s++;

	ret = __bencode_item_alloc(buf, 0);
	if (!ret)
		return NULL;
	__bencode_list_init(ret);

	while (s < end) {
		item = __bencode_decode(buf, s, end);
		if (!item)
			return NULL;
		s += item->str_len;
		if (item->type == BENCODE_END_MARKER)
			break;
		__bencode_container_add(ret, item);
	}

	return ret;
}

static bencode_item_t *__bencode_decode_integer(bencode_buffer_t *buf, const char *s, const char *end) {
	long long int i;
	const char *orig = s;
	char *convend;
	bencode_item_t *ret;

	if (*s != 'i')
		return NULL;
	s++;

	if (s >= end)
		return NULL;

	if (*s == '0') {
		i = 0;
		s++;
		goto done;
	}

	i = strtoll(s, &convend, 10);
	if (convend == s)
		return NULL;
	s += (convend - s);

done:
	if (s >= end)
		return NULL;
	if (*s != 'e')
		return NULL;
	s++;

	ret = __bencode_item_alloc(buf, 0);
	if (!ret)
		return NULL;
	ret->type = BENCODE_INTEGER;
	ret->iov[0].iov_base = (void *) orig;
	ret->iov[0].iov_len = s - orig;
	ret->iov[1].iov_base = NULL;
	ret->iov[1].iov_len = 0;
	ret->iov_cnt = 1;
	ret->str_len = s - orig;
	ret->value = i;

	return ret;
}

static bencode_item_t *__bencode_decode_string(bencode_buffer_t *buf, const char *s, const char *end) {
	unsigned long int sl;
	char *convend;
	const char *orig = s;
	bencode_item_t *ret;

	if (*s == '0') {
		sl = 0;
		s++;
		goto colon;
	}

	sl = strtoul(s, &convend, 10);
	if (convend == s)
		return NULL;
	s += (convend - s);

colon:
	if (s >= end)
		return NULL;
	if (*s != ':')
		return NULL;
	s++;

	if (s + sl > end)
		return NULL;

	ret = __bencode_item_alloc(buf, 0);
	if (!ret)
		return NULL;
	ret->type = BENCODE_STRING;
	ret->iov[0].iov_base = (void *) orig;
	ret->iov[0].iov_len = s - orig;
	ret->iov[1].iov_base = (void *) s;
	ret->iov[1].iov_len = sl;
	ret->iov_cnt = 2;
	ret->str_len = s - orig + sl;

	return ret;
}

static bencode_item_t *__bencode_decode(bencode_buffer_t *buf, const char *s, const char *end) {
	if (s >= end)
		return NULL;

	switch (*s) {
		case 'd':
			return __bencode_decode_dictionary(buf, s, end);
		case 'l':
			return __bencode_decode_list(buf, s, end);
		case 'i':
			return __bencode_decode_integer(buf, s, end);
		case 'e':
			return &__bencode_end_marker;
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
			return __bencode_decode_string(buf, s, end);
		default:
			return NULL;
	}
}

bencode_item_t *bencode_decode(bencode_buffer_t *buf, const char *s, size_t len) {
	assert(s != NULL);
	return __bencode_decode(buf, s, s + len);
}


static int __bencode_dictionary_key_match(bencode_item_t *key, const char *keystr, size_t keylen) {
	assert(key->type == BENCODE_STRING);

	if (keylen != key->iov[1].iov_len)
		return 0;
	if (memcmp(keystr, key->iov[1].iov_base, keylen))
		return 0;

	return 1;
}

bencode_item_t *bencode_dictionary_get_len(bencode_item_t *dict, const char *keystr, size_t keylen) {
	bencode_item_t *key;
	unsigned int bucket, i;
	struct __bencode_hash *hash;

	if (!dict)
		return NULL;
	if (dict->type != BENCODE_DICTIONARY)
		return NULL;

	/* try hash lookup first if possible */
	if (dict->value == 1) {
		hash = (void *) dict->__buf;
		i = bucket = __bencode_hash_str_len((const unsigned char *) keystr, keylen);
		while (1) {
			key = hash->buckets[i];
			if (!key)
				return NULL; /* would be there, but isn't */
			assert(key->sibling != NULL);
			if (__bencode_dictionary_key_match(key, keystr, keylen))
				return key->sibling;
			i++;
			if (i >= BENCODE_HASH_BUCKETS)
				i = 0;
			if (i == bucket)
				break; /* fall back to regular lookup */
		}
	}

	for (key = dict->child; key; key = key->sibling->sibling) {
		assert(key->sibling != NULL);
		if (__bencode_dictionary_key_match(key, keystr, keylen))
			return key->sibling;
	}

	return NULL;
}

void bencode_buffer_destroy_add(bencode_buffer_t *buf, free_func_t func, void *p) {
	struct __bencode_free_list *li;

	if (!p)
		return;
	li = bencode_buffer_alloc(buf, sizeof(*li));
	if (!li)
		return;
	li->ptr = p;
	li->func = func;
	li->next = buf->free_list;
	buf->free_list = li;
}

static ssize_t __bencode_string(const char *s, ssize_t offset, size_t len) {
	size_t pos;
	unsigned long sl;
	char *end;

	for (pos = offset; s[pos] != ':' && isdigit(s[pos]) && pos < len; ++pos);
	if (pos == len)
		return -1;

	sl = strtoul(s + offset, &end, 10);
	if (s + offset == end || end != s + pos)
		return -2;

	if (pos + sl > len)
		return -1;

	return pos + sl + 1;
}

static ssize_t __bencode_integer(const char *s, ssize_t offset, size_t len) {
	size_t pos;

	if (s[offset + 1] == '-') {
		if (offset + 3 < len && s[offset + 2] == '0' && s[offset + 3] == 'e') {
			return -2;
		}
		++offset;
	}

	if (s[offset + 1] == 'e')
		return -2;

	if (s[offset + 1] == '0') {
		if (offset + 2 < len && s[offset + 2] == 'e')
			return offset + 3;
		return -2;
	}

	for (pos = offset + 1; s[pos] != 'e' && pos < len; ++pos) {
		if (s[pos] < '0' || s[pos] > '9')
			return -2;
	}

	if (pos == len)
		return -1;

	return pos + 1;
}

static ssize_t __bencode_next(const char *s, ssize_t offset, size_t len);

static ssize_t __bencode_list(const char *s, ssize_t offset, size_t len) {
	for (++offset; s[offset] != 'e' && offset < len;) {
		offset = __bencode_next(s, offset, len);
		if (offset < 0)
			return offset;
	}

	if (offset == len)
		return -1;

	return offset + 1;
}

static ssize_t __bencode_dictionary(const char *s, ssize_t offset, size_t len) {
	for (++offset; s[offset] != 'e' && offset < len;) {
		offset = __bencode_string(s, offset, len);
		if (offset < 0)
			return offset;
		offset = __bencode_next(s, offset, len);
		if (offset < 0)
			return offset;
	}

	if (offset == len)
		return -1;

	return offset + 1;
}

static ssize_t __bencode_next(const char *s, ssize_t offset, size_t len) {
	if (offset >= len)
		return -1;
	switch(s[offset]) {
		case 'i':
			return __bencode_integer(s, offset, len);
		case 'l':
			return __bencode_list(s, offset, len);
		case 'd':
			return __bencode_dictionary(s, offset, len);
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
			return __bencode_string(s, offset, len);
	}
	return -2;
}

ssize_t bencode_valid(const char *s, size_t len) {
	return __bencode_next(s, 0, len);
}



static bencode_item_t *bencode_convert_json_node(bencode_buffer_t *buf, JsonNode *node);

static bencode_item_t *bencode_convert_json_dict(bencode_buffer_t *buf, JsonNode *node) {
	JsonObject *obj = json_node_get_object(node);
	if (!obj)
		return NULL;
	bencode_item_t *dict = bencode_dictionary(buf);
	if (!dict)
		return NULL;

	JsonObjectIter iter;
	json_object_iter_init(&iter, obj);
	const char *key;
	JsonNode *value;
	while (json_object_iter_next(&iter, &key, &value)) {
		if (!key || !value)
			return NULL;
		bencode_item_t *b_val = bencode_convert_json_node(buf, value);
		if (!b_val)
			return NULL;
		bencode_dictionary_add(dict, key, b_val);
	}
	return dict;
}

static bencode_item_t *bencode_convert_json_array(bencode_buffer_t *buf, JsonNode *node) {
	JsonArray *arr = json_node_get_array(node);
	if (!arr)
		return NULL;
	bencode_item_t *list = bencode_list(buf);
	if (!list)
		return NULL;
	guint len = json_array_get_length(arr);
	for (guint i = 0; i < len; i++) {
		JsonNode *el = json_array_get_element(arr, i);
		if (!el)
			return NULL;
		bencode_item_t *it = bencode_convert_json_node(buf, el);
		if (!it)
			return NULL;
		bencode_list_add(list, it);
	}
	return list;
}

static bencode_item_t *bencode_convert_json_value(bencode_buffer_t *buf, JsonNode *node) {
	GType type = json_node_get_value_type(node);
	switch (type) {
		case G_TYPE_STRING:;
			const char *s = json_node_get_string(node);
			if (!s)
				return NULL;
			return bencode_string(buf, s);
		case G_TYPE_INT:
		case G_TYPE_UINT:
		case G_TYPE_LONG:
		case G_TYPE_ULONG:
		case G_TYPE_INT64:
		case G_TYPE_UINT64:
		case G_TYPE_BOOLEAN:;
			gint64 i = json_node_get_int(node);
			return bencode_integer(buf, i);
		// everything else is unsupported
	}
	return NULL;
}

static bencode_item_t *bencode_convert_json_node(bencode_buffer_t *buf, JsonNode *node) {
	JsonNodeType type = json_node_get_node_type(node);
	switch (type) {
		case JSON_NODE_OBJECT:
			return bencode_convert_json_dict(buf, node);
		case JSON_NODE_ARRAY:
			return bencode_convert_json_array(buf, node);
		case JSON_NODE_VALUE:
			return bencode_convert_json_value(buf, node);
		default:
			return NULL;
	}
}

bencode_item_t *bencode_convert_json(bencode_buffer_t *buf, JsonParser *json) {
	JsonNode *root = json_parser_get_root(json);
	if (!root)
		return NULL;
	return bencode_convert_json_node(buf, root);
}



gboolean bencode_collapse_json_item(bencode_item_t *item, JsonBuilder *builder);

gboolean bencode_collapse_json_list(bencode_item_t *item, JsonBuilder *builder) {
	json_builder_begin_array(builder);
	for (bencode_item_t *el = item->child; el; el = el->sibling) {
		if (!bencode_collapse_json_item(el, builder))
			return FALSE;
	}
	json_builder_end_array(builder);
	return TRUE;
}

gboolean bencode_collapse_json_string(bencode_item_t *item, JsonBuilder *builder) {
	char buf[item->iov[1].iov_len + 1];
	memcpy(buf, item->iov[1].iov_base, item->iov[1].iov_len);
	buf[item->iov[1].iov_len] = '\0';
	json_builder_add_string_value(builder, buf);
	return TRUE;
}

gboolean bencode_collapse_json_dict(bencode_item_t *item, JsonBuilder *builder) {
	json_builder_begin_object(builder);
	bencode_item_t *val;
	for (bencode_item_t *key = item->child; key; key = val->sibling) {
		val = key->sibling;
		if (key->type != BENCODE_STRING)
			return FALSE;

		char buf[key->iov[1].iov_len + 1];
		memcpy(buf, key->iov[1].iov_base, key->iov[1].iov_len);
		buf[key->iov[1].iov_len] = '\0';

		json_builder_set_member_name(builder, buf);

		if (!bencode_collapse_json_item(val, builder))
			return FALSE;
	}
	json_builder_end_object(builder);
	return TRUE;
}

gboolean bencode_collapse_json_int(bencode_item_t *item, JsonBuilder *builder) {
	json_builder_add_int_value(builder, item->value);
	return TRUE;
}

gboolean bencode_collapse_json_item(bencode_item_t *item, JsonBuilder *builder) {
	switch (item->type) {
		case BENCODE_LIST:
			return bencode_collapse_json_list(item, builder);
		case BENCODE_STRING:
			return bencode_collapse_json_string(item, builder);
		case BENCODE_DICTIONARY:
			return bencode_collapse_json_dict(item, builder);
		case BENCODE_INTEGER:
			return bencode_collapse_json_int(item, builder);
		default:
			return FALSE;
	}
}

str *bencode_collapse_str_json(bencode_item_t *root, str *out) {
	JsonBuilder *builder = json_builder_new();
	if (!bencode_collapse_json_item(root, builder))
		goto err;
	char *result = glib_json_print(builder);
	out->s = result;
	out->len = strlen(result);
	bencode_buffer_destroy_add(root->buffer, free, result);
	return out;

err:
	g_object_unref(builder);
	return NULL;
}
