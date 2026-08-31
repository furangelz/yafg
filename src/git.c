#define _DEFAULT_SOURCE
#include "git.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/sha.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

void
sha1_to_hex(const unsigned char *sha, char *hex)
{
	for (int i = 0; i < 20; i++)
		sprintf(hex + i * 2, "%02x", sha[i]);
	hex[40] = '\0';
}

int
hex_to_sha1(const char *hex, unsigned char *sha)
{
	if (!hex || strlen(hex) < 40) return -1;
	for (int i = 0; i < 20; i++) {
		unsigned int v;
		if (sscanf(hex + i * 2, "%02x", &v) != 1) return -1;
		sha[i] = v;
	}
	return 0;
}

const char *
type_name(int type)
{
	switch (type) {
	case OBJ_BLOB:   return "blob";
	case OBJ_TREE:   return "tree";
	case OBJ_COMMIT: return "commit";
	case OBJ_TAG:    return "tag";
	default:         return "unknown";
	}
}

int
type_from_name(const char *name)
{
	if (!name) return OBJ_NONE;
	if (!strcmp(name, "blob"))   return OBJ_BLOB;
	if (!strcmp(name, "tree"))   return OBJ_TREE;
	if (!strcmp(name, "commit")) return OBJ_COMMIT;
	if (!strcmp(name, "tag"))    return OBJ_TAG;
	return OBJ_NONE;
}

void *
read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz < 0) { fclose(f); return NULL; }

	void *buf = malloc(sz ? sz : 1);
	if (!buf || (sz > 0 && fread(buf, 1, sz, f) != (size_t)sz)) {
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	if (len) *len = sz;
	return buf;
}

int
write_file(const char *path, const void *buf, size_t len)
{
	FILE *f = fopen(path, "wb");
	if (!f) return -1;
	if (len && fwrite(buf, 1, len, f) != len) {
		fclose(f);
		return -1;
	}
	fclose(f);
	return 0;
}

int
zcompress(const void *src, size_t slen, void **out, size_t *olen)
{
	uLong dlen = compressBound((uLong)slen);
	unsigned char *buf = malloc(dlen);

	if (!buf) return -1;
	if (compress(buf, &dlen, (const Bytef *)src, (uLong)slen) != Z_OK) {
		free(buf);
		return -1;
	}
	*out = buf;
	*olen = (size_t)dlen;
	return 0;
}

int
zdecompress_buf(const void *src, size_t slen, void **out, size_t *olen, size_t *consumed)
{
	z_stream st;
	memset(&st, 0, sizeof(st));
	st.next_in = (Bytef *)src;
	st.avail_in = (uInt)slen;

	size_t cap = 16384;
	unsigned char *buf = malloc(cap);
	if (!buf) return -1;

	if (inflateInit(&st) != Z_OK) {
		free(buf);
		return -1;
	}

	int r = Z_OK;
	while (r == Z_OK) {
		if (st.total_out >= cap) {
			size_t new_cap = cap * 2;
			unsigned char *new_buf = realloc(buf, new_cap);
			if (!new_buf) {
				free(buf);
				inflateEnd(&st);
				return -1;
			}
			buf = new_buf;
			cap = new_cap;
		}

		st.next_out = buf + st.total_out;
		st.avail_out = (uInt)(cap - st.total_out);

		r = inflate(&st, Z_NO_FLUSH);
	}

	if (r != Z_STREAM_END) {
		free(buf);
		inflateEnd(&st);
		return -1;
	}

	if (consumed) *consumed = (size_t)st.total_in;
	*olen = (size_t)st.total_out;
	inflateEnd(&st);
	*out = buf;
	return 0;
}

int
zdecompress(const void *src, size_t slen, void **out, size_t *olen)
{
	return zdecompress_buf(src, slen, out, olen, NULL);
}

int
object_hash(int type, const void *data, size_t len, GitObject *obj)
{
	char hdr[64];
	if (!obj) return -1;
	int hlen = snprintf(hdr, sizeof(hdr), "%s %zu", type_name(type), len) + 1;
	size_t flen = hlen + len;

	unsigned char *buf = malloc(flen);
	if (!buf) return -1;

	memcpy(buf, hdr, hlen);
	if (len && data) memcpy(buf + hlen, data, len);

	SHA1(buf, flen, obj->sha1);
	sha1_to_hex(obj->sha1, obj->hex);
	free(buf);

	obj->type = type;
	obj->size = len;
	if (len && data) {
		if (!(obj->data = malloc(len))) return -1;
		memcpy(obj->data, data, len);
	} else {
		obj->data = NULL;
	}
	return 0;
}

void
object_free(GitObject *obj)
{
	if (obj) {
		free(obj->data);
		obj->data = NULL;
	}
}

static int
mkdir_p(const char *path)
{
	char tmp[1024], *p;
	snprintf(tmp, sizeof(tmp), "%s", path);
	for (p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(tmp, 0755);
			*p = '/';
		}
	}
	return (mkdir(tmp, 0755) && errno != EEXIST) ? -1 : 0;
}

int
repo_init(const char *path)
{
	char base[1024], p[1024];

	snprintf(base, sizeof(base), "%s/.git", path ? path : ".");
	snprintf(p, sizeof(p), "%s/objects", base);    if (mkdir_p(p)) return -1;
	snprintf(p, sizeof(p), "%s/refs/heads", base); if (mkdir_p(p)) return -1;
	snprintf(p, sizeof(p), "%s/refs/tags", base);  if (mkdir_p(p)) return -1;

	snprintf(p, sizeof(p), "%s/HEAD", base);
	return write_file(p, "ref: refs/heads/main\n", 20);
}

int
object_write(const char *repo, int type, const void *data, size_t len, char *hex)
{
	GitObject obj;
	char dir[1024], path[1024], hdr[64];
	size_t hlen, ulen, clen;
	unsigned char *raw;
	void *cbuf;

	if (object_hash(type, data, len, &obj) < 0) return -1;
	if (hex) strcpy(hex, obj.hex);

	snprintf(dir, sizeof(dir), "%s/.git/objects/%.2s", repo ? repo : ".", obj.hex);
	snprintf(path, sizeof(path), "%s/.git/objects/%.2s/%s", repo ? repo : ".", obj.hex, obj.hex + 2);

	if (access(path, F_OK) == 0) {
		object_free(&obj);
		return 0;
	}

	if (mkdir_p(dir)) { object_free(&obj); return -1; }

	hlen = snprintf(hdr, sizeof(hdr), "%s %zu", type_name(type), len) + 1;
	ulen = hlen + len;
	if (!(raw = malloc(ulen))) { object_free(&obj); return -1; }

	memcpy(raw, hdr, hlen);
	if (len && data) memcpy(raw + hlen, data, len);

	if (zcompress(raw, ulen, &cbuf, &clen) < 0) {
		free(raw);
		object_free(&obj);
		return -1;
	}
	free(raw);

	int res = write_file(path, cbuf, clen);
	free(cbuf);
	object_free(&obj);
	return res;
}

int
object_read(const char *repo, const char *hex, GitObject *obj)
{
	char path[1024], *ptr, *sp, *nul;
	void *cbuf, *ubuf;
	size_t clen, ulen, hlen, plen;

	if (!hex || strlen(hex) != 40 || !obj) return -1;
	snprintf(path, sizeof(path), "%s/.git/objects/%.2s/%s", repo ? repo : ".", hex, hex + 2);

	if (!(cbuf = read_file(path, &clen))) return -1;
	if (zdecompress(cbuf, clen, &ubuf, &ulen) < 0) {
		free(cbuf);
		return -1;
	}
	free(cbuf);

	ptr = ubuf;
	if (!(sp = strchr(ptr, ' '))) { free(ubuf); return -1; }
	*sp = '\0';
	obj->type = type_from_name(ptr);

	ptr = sp + 1;
	if (!(nul = strchr(ptr, '\0'))) { free(ubuf); return -1; }
	obj->size = strtoul(ptr, NULL, 10);

	hlen = (nul - (char *)ubuf) + 1;
	if (ulen < hlen) { free(ubuf); return -1; }
	plen = ulen - hlen;
	obj->size = plen;

	strcpy(obj->hex, hex);
	hex_to_sha1(hex, obj->sha1);

	if (!(obj->data = malloc(plen + 1))) { free(ubuf); return -1; }
	if (plen > 0)
		memcpy(obj->data, (unsigned char *)ubuf + hlen, plen);
	((char *)obj->data)[plen] = '\0';

	free(ubuf);
	return 0;
}

static int
tree_entry_cmp(const void *a, const void *b)
{
	const TreeEntry *ea = (const TreeEntry *)a;
	const TreeEntry *eb = (const TreeEntry *)b;

	if (!ea || !ea->name) return -1;
	if (!eb || !eb->name) return 1;

	size_t len1 = strlen(ea->name);
	size_t len2 = strlen(eb->name);
	size_t len = len1 < len2 ? len1 : len2;

	int cmp = memcmp(ea->name, eb->name, len);
	if (cmp != 0) return cmp;

	int c1 = (len1 > len) ? (unsigned char)ea->name[len] : ((ea->mode == 040000 || S_ISDIR(ea->mode)) ? '/' : 0);
	int c2 = (len2 > len) ? (unsigned char)eb->name[len] : ((eb->mode == 040000 || S_ISDIR(eb->mode)) ? '/' : 0);

	return (c1 < c2) ? -1 : (c1 > c2) ? 1 : 0;
}

static int
tree_add(Tree *t, mode_t mode, const char *name, const unsigned char *sha)
{
	if (t->count >= t->cap) {
		t->cap = t->cap ? t->cap * 2 : 16;
		TreeEntry *e = realloc(t->entries, t->cap * sizeof(TreeEntry));
		if (!e) return -1;
		t->entries = e;
	}
	t->entries[t->count].mode = mode;
	t->entries[t->count].name = strdup(name);
	memcpy(t->entries[t->count].sha1, sha, 20);
	t->count++;
	return 0;
}

int
tree_write(const char *repo, const char *dir, char *hex)
{
	DIR *d;
	struct dirent *de;
	struct stat st;
	Tree tree = {0};
	char path[1024], subhex[41], blobhex[41];
	unsigned char sha[20];
	void *buf, *cbuf;
	size_t fsz, tlen = 0, off = 0;

	if (!(d = opendir(dir ? dir : "."))) return -1;

	while ((de = readdir(d))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..") || !strcmp(de->d_name, ".git"))
			continue;

		snprintf(path, sizeof(path), "%s/%s", dir ? dir : ".", de->d_name);
		if (stat(path, &st)) continue;

		if (S_ISDIR(st.st_mode)) {
			if (!tree_write(repo, path, subhex)) {
				hex_to_sha1(subhex, sha);
				tree_add(&tree, 040000, de->d_name, sha);
			}
		} else if (S_ISREG(st.st_mode)) {
			cbuf = read_file(path, &fsz);
			if (cbuf || fsz == 0) {
				if (!object_write(repo, OBJ_BLOB, cbuf, fsz, blobhex)) {
					hex_to_sha1(blobhex, sha);
					mode_t mode = (st.st_mode & S_IXUSR) ? 0100755 : 0100644;
					tree_add(&tree, mode, de->d_name, sha);
				}
				free(cbuf);
			}
		}
	}
	closedir(d);

	if (tree.count > 1)
		qsort(tree.entries, tree.count, sizeof(TreeEntry), tree_entry_cmp);

	for (size_t i = 0; i < tree.count; i++)
		tlen += snprintf(path, sizeof(path), "%o %s", tree.entries[i].mode, tree.entries[i].name) + 1 + 20;

	if (!(buf = malloc(tlen))) {
		for (size_t i = 0; i < tree.count; i++) free(tree.entries[i].name);
		free(tree.entries);
		return -1;
	}

	for (size_t i = 0; i < tree.count; i++) {
		int hlen = snprintf((char *)buf + off, tlen - off, "%o %s", tree.entries[i].mode, tree.entries[i].name) + 1;
		off += hlen;
		memcpy((char *)buf + off, tree.entries[i].sha1, 20);
		off += 20;
		free(tree.entries[i].name);
	}
	free(tree.entries);

	int r = object_write(repo, OBJ_TREE, buf, tlen, hex);
	free(buf);
	return r;
}

int
tree_checkout(const char *repo, const char *tree_hex, const char *dest)
{
	GitObject obj;
	if (object_read(repo, tree_hex, &obj) < 0) return -1;
	if (obj.type != OBJ_TREE) { object_free(&obj); return -1; }

	char *ptr = obj.data, *end = (char *)obj.data + obj.size;
	while (ptr < end) {
		mode_t mode = strtoul(ptr, &ptr, 8);
		if (*ptr == ' ') ptr++;
		char *name = ptr;
		ptr = strchr(ptr, '\0') + 1;
		char entry_hex[41];
		sha1_to_hex((unsigned char *)ptr, entry_hex);
		ptr += 20;

		char path[1024];
		snprintf(path, sizeof(path), "%s/%s", dest, name);

		if (S_ISDIR(mode) || mode == 040000) {
			mkdir(path, 0755);
			tree_checkout(repo, entry_hex, path);
		} else {
			GitObject blob;
			if (object_read(repo, entry_hex, &blob) == 0) {
				write_file(path, blob.data, blob.size);
				if (mode & 0111) chmod(path, 0755);
				object_free(&blob);
			}
		}
	}
	object_free(&obj);
	return 0;
}

int
commit_create(const char *repo, const char *tree, const char *parent,
              const char *author, const char *email, const char *msg, char *hex)
{
	char buf[2048];
	time_t now = time(NULL);

	int len = snprintf(buf, sizeof(buf),
		"tree %s\n"
		"%s%s%s"
		"author %s <%s> %ld +0000\n"
		"committer %s <%s> %ld +0000\n\n"
		"%s\n",
		tree,
		(parent && *parent) ? "parent " : "", (parent && *parent) ? parent : "", (parent && *parent) ? "\n" : "",
		author, email, (long)now,
		author, email, (long)now,
		msg);

	return object_write(repo, OBJ_COMMIT, buf, len, hex);
}

int
ref_update(const char *repo, const char *ref, const char *commit)
{
	char path[1024], buf[64], *p;

	snprintf(path, sizeof(path), "%s/.git/%s", repo ? repo : ".", ref);
	if ((p = strrchr(path, '/'))) {
		*p = '\0';
		mkdir_p(path);
		*p = '/';
	}

	int len = snprintf(buf, sizeof(buf), "%s\n", commit);
	return write_file(path, buf, len);
}

int
ref_resolve(const char *repo, const char *name, char *out_hex)
{
	char path[1024], *buf;
	size_t len;

	if (!name || !out_hex) return -1;
	if (strlen(name) == 40) {
		strcpy(out_hex, name);
		return 0;
	}

	if (!strcmp(name, "HEAD")) {
		snprintf(path, sizeof(path), "%s/.git/HEAD", repo ? repo : ".");
		if ((buf = read_file(path, &len))) {
			if (!strncmp(buf, "ref: ", 5)) {
				char ref_name[256];
				if (sscanf((char *)buf + 5, "%255s", ref_name) == 1) {
					free(buf);
					return ref_resolve(repo, ref_name, out_hex);
				}
			}
			if (len >= 40) {
				memcpy(out_hex, buf, 40);
				out_hex[40] = '\0';
				free(buf);
				return 0;
			}
			free(buf);
		}
		return -1;
	}

	if (strncmp(name, "refs/", 5) != 0)
		snprintf(path, sizeof(path), "%s/.git/refs/heads/%s", repo ? repo : ".", name);
	else
		snprintf(path, sizeof(path), "%s/.git/%s", repo ? repo : ".", name);

	if ((buf = read_file(path, &len))) {
		if (len >= 40) {
			memcpy(out_hex, buf, 40);
			out_hex[40] = '\0';
			free(buf);
			return 0;
		}
		free(buf);
	}

	return -1;
}

static int
delta_apply(const unsigned char *base, size_t base_sz,
            const unsigned char *delta, size_t delta_sz,
            void **out_data, size_t *out_sz)
{
	size_t pos = 0, base_len = 0, target_len = 0, shift = 0;
	unsigned char b;

	do {
		if (pos >= delta_sz) return -1;
		b = delta[pos++];
		base_len |= (size_t)(b & 0x7f) << shift;
		shift += 7;
	} while (b & 0x80);

	shift = 0;
	do {
		if (pos >= delta_sz) return -1;
		b = delta[pos++];
		target_len |= (size_t)(b & 0x7f) << shift;
		shift += 7;
	} while (b & 0x80);

	unsigned char *target = malloc(target_len ? target_len : 1);
	if (!target) return -1;

	size_t out_pos = 0;
	while (pos < delta_sz) {
		unsigned char cmd = delta[pos++];
		if (cmd & 0x80) {
			size_t cp_off = 0, cp_sz = 0;
			if (cmd & 0x01) cp_off |= delta[pos++];
			if (cmd & 0x02) cp_off |= (size_t)delta[pos++] << 8;
			if (cmd & 0x04) cp_off |= (size_t)delta[pos++] << 16;
			if (cmd & 0x08) cp_off |= (size_t)delta[pos++] << 24;

			if (cmd & 0x10) cp_sz |= delta[pos++];
			if (cmd & 0x20) cp_sz |= (size_t)delta[pos++] << 8;
			if (cmd & 0x40) cp_sz |= (size_t)delta[pos++] << 16;
			if (cp_sz == 0) cp_sz = 0x10000;

			if (cp_off + cp_sz > base_sz || out_pos + cp_sz > target_len) {
				free(target);
				return -1;
			}
			memcpy(target + out_pos, base + cp_off, cp_sz);
			out_pos += cp_sz;
		} else if (cmd > 0) {
			if (pos + cmd > delta_sz || out_pos + cmd > target_len) {
				free(target);
				return -1;
			}
			memcpy(target + out_pos, delta + pos, cmd);
			pos += cmd;
			out_pos += cmd;
		}
	}

	*out_data = target;
	*out_sz = target_len;
	return 0;
}

typedef struct {
	size_t offset;
	char hex[41];
	int type;
} PackObjInfo;

int
pack_unpack(const void *buf, size_t len, const char *repo)
{
	const unsigned char *p = buf;
	size_t pos = 0;

	while (pos + 12 <= len && memcmp(p + pos, "PACK", 4) != 0)
		pos++;

	if (pos + 12 > len) return -1;
	pos += 4;

	uint32_t ver = (p[pos] << 24) | (p[pos+1] << 16) | (p[pos+2] << 8) | p[pos+3];
	pos += 4;
	if (ver != 2 && ver != 3) return -1;

	uint32_t count = (p[pos] << 24) | (p[pos+1] << 16) | (p[pos+2] << 8) | p[pos+3];
	pos += 4;

	PackObjInfo *info = calloc(count, sizeof(PackObjInfo));
	if (!info) return -1;

	for (uint32_t i = 0; i < count; i++) {
		size_t obj_pos = pos;
		info[i].offset = obj_pos;

		if (pos >= len) break;
		unsigned char c = p[pos++];
		int type = (c >> 4) & 7;
		size_t shift = 4;
		while (c & 0x80) {
			if (pos >= len) break;
			c = p[pos++];
			shift += 7;
		}
		info[i].type = type;

		if (type >= OBJ_COMMIT && type <= OBJ_TAG) {
			void *data;
			size_t dsize, consumed;
			if (zdecompress_buf(p + pos, len - pos, &data, &dsize, &consumed) == 0) {
				pos += consumed;
				object_write(repo, type, data, dsize, info[i].hex);
				free(data);
			}
		} else if (type == OBJ_OFS_DELTA) {
			c = p[pos++];
			while (c & 0x80) c = p[pos++];
			void *delta;
			size_t dsize, consumed;
			if (zdecompress_buf(p + pos, len - pos, &delta, &dsize, &consumed) == 0) {
				pos += consumed;
				free(delta);
			}
		} else if (type == OBJ_REF_DELTA) {
			pos += 20;
			void *delta;
			size_t dsize, consumed;
			if (zdecompress_buf(p + pos, len - pos, &delta, &dsize, &consumed) == 0) {
				pos += consumed;
				free(delta);
			}
		}
	}

	int resolved = 1;
	while (resolved) {
		resolved = 0;
		for (uint32_t i = 0; i < count; i++) {
			if (info[i].hex[0] != '\0') continue;

			size_t cur = info[i].offset;
			unsigned char c = p[cur++];
			int type = (c >> 4) & 7;
			while (c & 0x80) c = p[cur++];

			char base_hex[41] = {0};

			if (type == OBJ_OFS_DELTA) {
				size_t ofs = c & 0x7f;
				while (c & 0x80) {
					c = p[cur++];
					ofs = ((ofs + 1) << 7) | (c & 0x7f);
				}
				size_t base_off = info[i].offset - ofs;
				for (uint32_t j = 0; j < count; j++) {
					if (info[j].offset == base_off && info[j].hex[0] != '\0') {
						strcpy(base_hex, info[j].hex);
						break;
					}
				}
			} else if (type == OBJ_REF_DELTA) {
				sha1_to_hex(p + cur, base_hex);
				cur += 20;
			}

			if (base_hex[0] != '\0') {
				GitObject base_obj;
				if (object_read(repo, base_hex, &base_obj) == 0) {
					void *delta_data;
					size_t dsize, consumed;
					if (zdecompress_buf(p + cur, len - cur, &delta_data, &dsize, &consumed) == 0) {
						void *target_data;
						size_t target_sz;
						if (delta_apply(base_obj.data, base_obj.size, delta_data, dsize, &target_data, &target_sz) == 0) {
							object_write(repo, base_obj.type, target_data, target_sz, info[i].hex);
							free(target_data);
							resolved = 1;
						}
						free(delta_data);
					}
					object_free(&base_obj);
				}
			}
		}
	}

	free(info);
	return 0;
}

int
clone_repo(const char *url, const char *dir)
{
	char cmd[2048], repo_url[1024], target_dir[512], head_sha[41] = {0};
	char req_file[512], pack_file[512], line[1024], req_body[256];
	void *pack_buf = NULL;
	size_t pack_sz = 0;
	FILE *p;

	if (!url) return -1;

	snprintf(repo_url, sizeof(repo_url), "%s", url);
	if (strlen(repo_url) > 4 && strcmp(repo_url + strlen(repo_url) - 4, ".git"))
		strcat(repo_url, ".git");

	if (dir && *dir) {
		snprintf(target_dir, sizeof(target_dir), "%s", dir);
	} else {
		const char *slash = strrchr(url, '/');
		snprintf(target_dir, sizeof(target_dir), "%s", slash ? slash + 1 : url);
		char *dot = strstr(target_dir, ".git");
		if (dot) *dot = '\0';
	}

	mkdir(target_dir, 0755);
	if (repo_init(target_dir) < 0) return -1;

	snprintf(cmd, sizeof(cmd), "curl -s -L \"%s/info/refs?service=git-upload-pack\"", repo_url);
	if (!(p = popen(cmd, "r"))) return -1;

	while (fgets(line, sizeof(line), p)) {
		char sha[41], ref[256];
		if (sscanf(line + 4, "%40s %255s", sha, ref) == 2 || sscanf(line, "%40s %255s", sha, ref) == 2) {
			if (!strcmp(ref, "HEAD") || !strcmp(ref, "refs/heads/main") || !strcmp(ref, "refs/heads/master")) {
				if (strlen(sha) == 40) {
					strcpy(head_sha, sha);
					if (!strcmp(ref, "HEAD")) break;
				}
			}
		}
	}
	pclose(p);

	if (!head_sha[0]) return -1;

	snprintf(req_file, sizeof(req_file), "/tmp/yafg_req_%d", getpid());
	snprintf(pack_file, sizeof(pack_file), "/tmp/yafg_pack_%d", getpid());

	int req_len = snprintf(req_body, sizeof(req_body), "0032want %s\n00000009done\n", head_sha);
	write_file(req_file, req_body, req_len);

	snprintf(cmd, sizeof(cmd),
		"curl -s -L -H \"Content-Type: application/x-git-upload-pack-request\" "
		"--data-binary \"@%s\" \"%s/git-upload-pack\" > \"%s\"",
		req_file, repo_url, pack_file);

	int res = system(cmd);
	(void)res;
	unlink(req_file);

	pack_buf = read_file(pack_file, &pack_sz);
	unlink(pack_file);
	if (!pack_buf) return -1;

	pack_unpack(pack_buf, pack_sz, target_dir);
	free(pack_buf);

	ref_update(target_dir, "refs/heads/main", head_sha);

	GitObject commit_obj;
	if (object_read(target_dir, head_sha, &commit_obj) == 0) {
		char tree_hex[41] = {0};
		if (sscanf((char *)commit_obj.data, "tree %40s", tree_hex) == 1)
			tree_checkout(target_dir, tree_hex, target_dir);
		object_free(&commit_obj);
	}

	return 0;
}

int
pkt_write(int fd, const void *data, size_t len)
{
	char hdr[5];
	if (len + 4 > 65524) return -1;
	snprintf(hdr, sizeof(hdr), "%04zx", len + 4);
	if (write(fd, hdr, 4) != 4) return -1;
	if (len && data && write(fd, data, len) != (ssize_t)len) return -1;
	return 0;
}

int
pkt_write_str(int fd, const char *str)
{
	return str ? pkt_write(fd, str, strlen(str)) : -1;
}

int
pkt_flush(int fd)
{
	return write(fd, "0000", 4) == 4 ? 0 : -1;
}

int
pkt_read(int fd, char *buf, size_t maxlen)
{
	char hdr[5] = {0};
	unsigned int len;
	size_t got = 0;
	ssize_t n;

	if (read(fd, hdr, 4) != 4) return -1;
	if (sscanf(hdr, "%04x", &len) != 1) return -1;
	if (len == 0) return 0;
	if (len < 4 || (len - 4) >= maxlen) return -1;

	len -= 4;
	while (got < len) {
		if ((n = read(fd, buf + got, len - got)) <= 0) return -1;
		got += n;
	}
	buf[got] = '\0';
	return (int)got;
}

int
proto_serve_refs(int in_fd, int out_fd, const char *repo)
{
	(void)in_fd;
	char sha[41] = "0000000000000000000000000000000000000000", line[512];

	ref_resolve(repo, "HEAD", sha);

	size_t hlen = snprintf(line, sizeof(line), "%s HEAD%cmulti_ack side-band-64k agent=yafg/1.0\n", sha, '\0');
	hlen += strlen("multi_ack side-band-64k agent=yafg/1.0\n");
	pkt_write(out_fd, line, hlen);

	if (strcmp(sha, "0000000000000000000000000000000000000000")) {
		snprintf(line, sizeof(line), "%s refs/heads/main\n", sha);
		pkt_write_str(out_fd, line);
	}

	return pkt_flush(out_fd);
}

static int
index_read(const char *repo, Index *idx)
{
	char path[1024], *buf, *line, *saveptr;
	size_t len;

	memset(idx, 0, sizeof(*idx));
	snprintf(path, sizeof(path), "%s/.git/index", repo ? repo : ".");

	buf = read_file(path, &len);
	if (!buf) return 0;

	line = strtok_r(buf, "\n", &saveptr);
	while (line) {
		mode_t mode;
		size_t size;
		char sha_hex[41], entry_path[1024];

		if (sscanf(line, "%o %zu %40s %[^\n]", &mode, &size, sha_hex, entry_path) == 4) {
			if (idx->count >= idx->cap) {
				idx->cap = idx->cap ? idx->cap * 2 : 16;
				idx->entries = realloc(idx->entries, idx->cap * sizeof(IndexEntry));
			}
			idx->entries[idx->count].mode = mode;
			idx->entries[idx->count].size = size;
			idx->entries[idx->count].path = strdup(entry_path);
			hex_to_sha1(sha_hex, idx->entries[idx->count].sha1);
			idx->count++;
		}
		line = strtok_r(NULL, "\n", &saveptr);
	}

	free(buf);
	return 0;
}

static int
index_save(const char *repo, const Index *idx)
{
	char path[1024];
	size_t cap = 4096, len = 0;
	char *buf = malloc(cap);
	if (!buf) return -1;

	snprintf(path, sizeof(path), "%s/.git/index", repo ? repo : ".");

	for (size_t i = 0; i < idx->count; i++) {
		char sha_hex[41];
		sha1_to_hex(idx->entries[i].sha1, sha_hex);
		char line[2048];
		int n = snprintf(line, sizeof(line), "%o %zu %s %s\n",
			idx->entries[i].mode, idx->entries[i].size, sha_hex, idx->entries[i].path);

		if (len + n + 1 >= cap) {
			cap *= 2;
			buf = realloc(buf, cap);
		}
		memcpy(buf + len, line, n);
		len += n;
	}

	int res = write_file(path, buf, len);
	free(buf);
	return res;
}

static void
index_free(Index *idx)
{
	for (size_t i = 0; i < idx->count; i++)
		free(idx->entries[i].path);
	free(idx->entries);
	memset(idx, 0, sizeof(*idx));
}

static int
index_add_single(const char *repo, const char *rel_path)
{
	struct stat st;
	if (stat(rel_path, &st) || !S_ISREG(st.st_mode)) return -1;

	size_t fsz = 0;
	void *buf = read_file(rel_path, &fsz);
	if (!buf && fsz > 0) return -1;

	char hex[41];
	if (object_write(repo, OBJ_BLOB, buf, fsz, hex) < 0) {
		free(buf);
		return -1;
	}
	free(buf);

	Index idx;
	index_read(repo, &idx);

	int updated = 0;
	unsigned char sha[20];
	hex_to_sha1(hex, sha);
	mode_t mode = (st.st_mode & S_IXUSR) ? 0100755 : 0100644;

	for (size_t i = 0; i < idx.count; i++) {
		if (!strcmp(idx.entries[i].path, rel_path)) {
			idx.entries[i].mode = mode;
			idx.entries[i].size = fsz;
			memcpy(idx.entries[i].sha1, sha, 20);
			updated = 1;
			break;
		}
	}

	if (!updated) {
		if (idx.count >= idx.cap) {
			idx.cap = idx.cap ? idx.cap * 2 : 16;
			idx.entries = realloc(idx.entries, idx.cap * sizeof(IndexEntry));
		}
		idx.entries[idx.count].mode = mode;
		idx.entries[idx.count].size = fsz;
		idx.entries[idx.count].path = strdup(rel_path);
		memcpy(idx.entries[idx.count].sha1, sha, 20);
		idx.count++;
	}

	int res = index_save(repo, &idx);
	index_free(&idx);
	return res;
}

static int
index_add_dir(const char *repo, const char *dir)
{
	DIR *d = opendir(dir);
	if (!d) return -1;
	struct dirent *de;
	char path[1024];

	while ((de = readdir(d))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..") || !strcmp(de->d_name, ".git"))
			continue;

		snprintf(path, sizeof(path), "%s/%s", (!strcmp(dir, ".") ? "" : dir), de->d_name);
		const char *p = path[0] == '/' ? path + 1 : path;
		if (path[0] == '.' && path[1] == '/') p = path + 2;

		struct stat st;
		if (stat(p, &st) == 0) {
			if (S_ISDIR(st.st_mode)) {
				index_add_dir(repo, p);
			} else if (S_ISREG(st.st_mode)) {
				index_add_single(repo, p);
			}
		}
	}
	closedir(d);
	return 0;
}

int
index_add(const char *repo, const char *path)
{
	struct stat st;
	const char *target = (path && *path) ? path : ".";
	if (stat(target, &st)) return -1;

	if (S_ISDIR(st.st_mode))
		return index_add_dir(repo, target);
	else
		return index_add_single(repo, target);
}

static int
index_write_tree_prefix(const char *repo, const Index *idx, const char *prefix, char *out_hex)
{
	size_t plen = strlen(prefix);
	Tree tree = {0};

	for (size_t i = 0; i < idx->count; i++) {
		const char *path = idx->entries[i].path;
		if (plen > 0 && strncmp(path, prefix, plen) != 0)
			continue;

		const char *rel = path + plen;
		char *slash = strchr(rel, '/');

		if (!slash) {
			tree_add(&tree, idx->entries[i].mode, rel, idx->entries[i].sha1);
		} else {
			size_t dlen = slash - rel;
			char dirname[256];
			if (dlen >= sizeof(dirname)) dlen = sizeof(dirname) - 1;
			memcpy(dirname, rel, dlen);
			dirname[dlen] = '\0';

			int processed = 0;
			for (size_t j = 0; j < tree.count; j++) {
				if (!strcmp(tree.entries[j].name, dirname)) {
					processed = 1;
					break;
				}
			}

			if (!processed) {
				char sub_prefix[1024], subhex[41];
				snprintf(sub_prefix, sizeof(sub_prefix), "%s%s/", prefix, dirname);

				if (index_write_tree_prefix(repo, idx, sub_prefix, subhex) == 0) {
					unsigned char sha[20];
					hex_to_sha1(subhex, sha);
					tree_add(&tree, 040000, dirname, sha);
				}
			}
		}
	}

	if (tree.count > 1)
		qsort(tree.entries, tree.count, sizeof(TreeEntry), tree_entry_cmp);

	size_t tlen = 0, off = 0;
	char path_buf[1024];
	for (size_t i = 0; i < tree.count; i++)
		tlen += snprintf(path_buf, sizeof(path_buf), "%o %s", tree.entries[i].mode, tree.entries[i].name) + 1 + 20;

	void *buf = malloc(tlen ? tlen : 1);
	if (!buf) {
		for (size_t i = 0; i < tree.count; i++) free(tree.entries[i].name);
		free(tree.entries);
		return -1;
	}

	for (size_t i = 0; i < tree.count; i++) {
		int hlen = snprintf((char *)buf + off, tlen - off, "%o %s", tree.entries[i].mode, tree.entries[i].name) + 1;
		off += hlen;
		memcpy((char *)buf + off, tree.entries[i].sha1, 20);
		off += 20;
		free(tree.entries[i].name);
	}
	free(tree.entries);

	int res = object_write(repo, OBJ_TREE, buf, tlen, out_hex);
	free(buf);
	return res;
}

int
index_write_tree(const char *repo, char *out_hex)
{
	Index idx;
	index_read(repo, &idx);

	if (idx.count == 0) {
		index_free(&idx);
		return tree_write(repo, ".", out_hex);
	}

	int res = index_write_tree_prefix(repo, &idx, "", out_hex);
	index_free(&idx);

	char idx_path[1024];
	snprintf(idx_path, sizeof(idx_path), "%s/.git/index", repo ? repo : ".");
	unlink(idx_path);

	return res;
}

int
remote_add(const char *repo, const char *name, const char *url)
{
	char path[1024], buf[2048];
	if (!name || !url) return -1;

	snprintf(path, sizeof(path), "%s/.git/remotes", repo ? repo : ".");
	mkdir_p(path);

	snprintf(path, sizeof(path), "%s/.git/remotes/%s", repo ? repo : ".", name);
	int len = snprintf(buf, sizeof(buf), "%s\n", url);
	return write_file(path, buf, len);
}

int
remote_get_url(const char *repo, const char *name, char *url_out, size_t maxlen)
{
	char path[1024], *buf;
	size_t len;

	if (!name || !url_out) return -1;

	snprintf(path, sizeof(path), "%s/.git/remotes/%s", repo ? repo : ".", name);
	if ((buf = read_file(path, &len))) {
		char *nl = strchr(buf, '\n');
		if (nl) *nl = '\0';
		snprintf(url_out, maxlen, "%s", buf);
		free(buf);
		return 0;
	}

	snprintf(path, sizeof(path), "%s/.git/config", repo ? repo : ".");
	if ((buf = read_file(path, &len))) {
		char section[256];
		snprintf(section, sizeof(section), "[remote \"%s\"]", name);
		char *s = strstr(buf, section);
		if (!s && !strcmp(name, "origin"))
			s = strstr(buf, "[remote \"origin\"]");

		if (s) {
			char *u = strstr(s, "url = ");
			if (u) {
				u += 6;
				char *nl = strchr(u, '\n');
				if (nl) *nl = '\0';
				while (*u == ' ' || *u == '\t') u++;
				snprintf(url_out, maxlen, "%s", u);
				free(buf);
				return 0;
			}
		}
		free(buf);
	}

	return -1;
}

typedef struct {
	char hex[41];
	int type;
	void *data;
	size_t size;
} PackObject;

static void
collect_commit_objects(const char *repo, const char *hex, PackObject **objs, size_t *count, size_t *cap)
{
	if (!hex || strlen(hex) != 40) return;

	for (size_t i = 0; i < *count; i++) {
		if (!strcmp((*objs)[i].hex, hex)) return;
	}

	GitObject obj;
	if (object_read(repo, hex, &obj) < 0) return;

	if (*count >= *cap) {
		*cap = *cap ? *cap * 2 : 16;
		*objs = realloc(*objs, *cap * sizeof(PackObject));
	}

	strcpy((*objs)[*count].hex, obj.hex);
	(*objs)[*count].type = obj.type;
	(*objs)[*count].size = obj.size;
	(*objs)[*count].data = obj.data;
	(*count)++;

	if (obj.type == OBJ_COMMIT) {
		char tree_hex[41] = {0}, parent_hex[41] = {0};
		if (sscanf((char *)obj.data, "tree %40s", tree_hex) == 1)
			collect_commit_objects(repo, tree_hex, objs, count, cap);
		char *p = strstr((char *)obj.data, "parent ");
		if (p && sscanf(p, "parent %40s", parent_hex) == 1)
			collect_commit_objects(repo, parent_hex, objs, count, cap);
	} else if (obj.type == OBJ_TREE) {
		char *ptr = obj.data, *end = (char *)obj.data + obj.size;
		while (ptr && ptr < end) {
			strtoul(ptr, &ptr, 8);
			if (ptr < end && *ptr == ' ') ptr++;
			char *nul = memchr(ptr, '\0', end - ptr);
			if (!nul) break;
			ptr = nul + 1;
			if (ptr + 20 > end) break;
			char entry_hex[41];
			sha1_to_hex((unsigned char *)ptr, entry_hex);
			ptr += 20;
			collect_commit_objects(repo, entry_hex, objs, count, cap);
		}
	}
}

int
pack_create(const char *repo, const char *local_sha, const char *remote_sha, void **out_buf, size_t *out_len)
{
	(void)remote_sha;
	PackObject *objs = NULL;
	size_t count = 0, cap = 0;

	collect_commit_objects(repo, local_sha, &objs, &count, &cap);

	size_t buf_cap = 65536, buf_len = 0;
	unsigned char *buf = malloc(buf_cap);
	if (!buf) return -1;

	memcpy(buf, "PACK", 4);
	buf[4] = 0; buf[5] = 0; buf[6] = 0; buf[7] = 2;
	buf[8] = (count >> 24) & 0xff;
	buf[9] = (count >> 16) & 0xff;
	buf[10] = (count >> 8) & 0xff;
	buf[11] = count & 0xff;
	buf_len = 12;

	for (size_t i = 0; i < count; i++) {
		unsigned char header[32];
		size_t hlen = 0;
		size_t size = objs[i].size;
		int type = objs[i].type;

		unsigned char c = ((type & 7) << 4) | (size & 15);
		size >>= 4;
		if (size) c |= 0x80;
		header[hlen++] = c;

		while (size) {
			c = size & 0x7f;
			size >>= 7;
			if (size) c |= 0x80;
			header[hlen++] = c;
		}

		void *cbuf;
		size_t clen;
		if (zcompress(objs[i].data, objs[i].size, &cbuf, &clen) < 0) continue;

		if (buf_len + hlen + clen + 20 >= buf_cap) {
			buf_cap = (buf_len + hlen + clen + 20) * 2;
			buf = realloc(buf, buf_cap);
		}

		memcpy(buf + buf_len, header, hlen);
		buf_len += hlen;

		memcpy(buf + buf_len, cbuf, clen);
		buf_len += clen;
		free(cbuf);
		free(objs[i].data);
	}
	free(objs);

	unsigned char sha[20];
	SHA1(buf, buf_len, sha);
	memcpy(buf + buf_len, sha, 20);
	buf_len += 20;

	*out_buf = buf;
	*out_len = buf_len;
	return 0;
}

int
push_repo(const char *repo, const char *remote_name, const char *branch)
{
	char url[1024], repo_url[1024], cmd[2048], line[1024], local_sha[41] = {0}, remote_sha[41];
	memset(remote_sha, '0', 40);
	remote_sha[40] = '\0';
	char req_file[512], target_ref[256];
	FILE *p;

	const char *rname = (remote_name && *remote_name) ? remote_name : "origin";
	const char *bname = (branch && *branch) ? branch : "main";

	if (remote_get_url(repo, rname, url, sizeof(url)) < 0) {
		fprintf(stderr, "error: no remote URL found for '%s'\n", rname);
		return -1;
	}

	snprintf(repo_url, sizeof(repo_url), "%s", url);
	if (strlen(repo_url) > 4 && strcmp(repo_url + strlen(repo_url) - 4, ".git") != 0) {
		if (!strstr(repo_url, ".git")) strcat(repo_url, ".git");
	}

	snprintf(target_ref, sizeof(target_ref), "refs/heads/%s", bname);
	if (ref_resolve(repo, bname, local_sha) < 0 && ref_resolve(repo, target_ref, local_sha) < 0) {
		if (ref_resolve(repo, "HEAD", local_sha) < 0) {
			fprintf(stderr, "error: cannot resolve local ref for branch '%s'\n", bname);
			return -1;
		}
	}

	char auth_flags[512] = {0};
	const char *tok = getenv("GITHUB_TOKEN");
	if (!tok) tok = getenv("GIT_TOKEN");
	if (tok) snprintf(auth_flags, sizeof(auth_flags), "-u \"x-access-token:%s\" ", tok);

	snprintf(cmd, sizeof(cmd), "curl -s -f -L %s\"%s/info/refs?service=git-receive-pack\"", auth_flags, repo_url);
	if ((p = popen(cmd, "r"))) {
		while (fgets(line, sizeof(line), p)) {
			char *rptr = strstr(line, target_ref);
			if (rptr && rptr > line + 40) {
				char *sptr = rptr - 41;
				if (sptr >= line && sptr[40] == ' ') {
					memcpy(remote_sha, sptr, 40);
					remote_sha[40] = '\0';
					break;
				}
			}
		}
		pclose(p);
	}

	void *pack_buf = NULL;
	size_t pack_sz = 0;
	if (pack_create(repo, local_sha, remote_sha, &pack_buf, &pack_sz) < 0) return -1;

	snprintf(req_file, sizeof(req_file), "/tmp/yafg_push_%d", getpid());
	FILE *rf = fopen(req_file, "wb");
	if (!rf) { free(pack_buf); return -1; }

	char req_line[512];
	int n1 = snprintf(req_line, sizeof(req_line), "%s %s %s", remote_sha, local_sha, target_ref);
	req_line[n1] = '\0';
	int n2 = snprintf(req_line + n1 + 1, sizeof(req_line) - (n1 + 1), "report-status agent=yafg/1.0\n");
	size_t rlen = n1 + 1 + n2;

	char hdr[5];
	snprintf(hdr, sizeof(hdr), "%04zx", rlen + 4);
	fwrite(hdr, 1, 4, rf);
	fwrite(req_line, 1, rlen, rf);
	fwrite("0000", 1, 4, rf);

	fwrite(pack_buf, 1, pack_sz, rf);
	fclose(rf);
	free(pack_buf);

	snprintf(cmd, sizeof(cmd),
		"curl -s -f -L %s-H \"Content-Type: application/x-git-receive-pack-request\" "
		"-H \"Accept: application/x-git-receive-pack-result\" "
		"--data-binary \"@%s\" \"%s/git-receive-pack\"",
		auth_flags, req_file, repo_url);

	FILE *resp = popen(cmd, "r");
	int status = -1;
	if (resp) {
		char rbuf[4096];
		size_t nread = fread(rbuf, 1, sizeof(rbuf) - 1, resp);
		if (nread > 0) {
			rbuf[nread] = '\0';
			if (memmem(rbuf, nread, "unpack ok", 9) || memmem(rbuf, nread, "ok refs/heads", 13)) {
				status = 0;
			} else {
				fwrite(rbuf, 1, nread, stdout);
			}
		}
		int pcode = pclose(resp);
		if (pcode == 0 && status != 0 && nread > 0)
			status = 0;
	}
	unlink(req_file);
	return status;
}