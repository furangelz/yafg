#ifndef GIT_H
#define GIT_H

#include <stddef.h>
#include <sys/types.h>

#define SHA1_HEX 40
#define SHA1_BIN 20

enum {
	OBJ_NONE = 0,
	OBJ_COMMIT,
	OBJ_TREE,
	OBJ_BLOB,
	OBJ_TAG,
	OBJ_OFS_DELTA = 6,
	OBJ_REF_DELTA = 7
};

typedef struct {
	int type;
	size_t size;
	unsigned char sha1[20];
	char hex[41];
	void *data;
} GitObject;

typedef struct {
	mode_t mode;
	char *name;
	unsigned char sha1[20];
} TreeEntry;

typedef struct {
	TreeEntry *entries;
	size_t count, cap;
} Tree;

void sha1_to_hex(const unsigned char *sha, char *hex);
int hex_to_sha1(const char *hex, unsigned char *sha);
const char *type_name(int type);
int type_from_name(const char *name);

void *read_file(const char *path, size_t *len);
int write_file(const char *path, const void *buf, size_t len);

int zcompress(const void *src, size_t slen, void **out, size_t *olen);
int zdecompress(const void *src, size_t slen, void **out, size_t *olen);
int zdecompress_buf(const void *src, size_t slen, void **out, size_t *olen, size_t *consumed);

int object_hash(int type, const void *data, size_t len, GitObject *obj);
void object_free(GitObject *obj);
int repo_init(const char *path);
int object_write(const char *repo, int type, const void *data, size_t len, char *hex);
int object_read(const char *repo, const char *hex, GitObject *obj);

int tree_write(const char *repo, const char *dir, char *hex);
int tree_checkout(const char *repo, const char *tree_hex, const char *dest);
int commit_create(const char *repo, const char *tree, const char *parent,
                  const char *author, const char *email, const char *msg, char *hex);
int ref_update(const char *repo, const char *ref, const char *commit);
int ref_resolve(const char *repo, const char *name, char *out_hex);

int pack_unpack(const void *buf, size_t len, const char *repo);
int clone_repo(const char *url, const char *dir);

typedef struct {
	mode_t mode;
	size_t size;
	unsigned char sha1[20];
	char *path;
} IndexEntry;

typedef struct {
	IndexEntry *entries;
	size_t count, cap;
} Index;

int index_add(const char *repo, const char *path);
int index_write_tree(const char *repo, char *out_hex);

int remote_add(const char *repo, const char *name, const char *url);
int remote_get_url(const char *repo, const char *name, char *url_out, size_t maxlen);
int pack_create(const char *repo, const char *local_sha, const char *remote_sha, void **out_buf, size_t *out_len);
int push_repo(const char *repo, const char *remote_name, const char *branch);

int pkt_write(int fd, const void *data, size_t len);
int pkt_write_str(int fd, const char *str);
int pkt_flush(int fd);
int pkt_read(int fd, char *buf, size_t maxlen);

int proto_serve_refs(int in_fd, int out_fd, const char *repo);

#endif