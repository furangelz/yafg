#include "git.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void
die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

static void
usage(void)
{
	fprintf(stderr,
		"usage: yafg <command> [<args>]\n\n"
		"commands:\n"
		"  init [<path>]\n"
		"  add <path>\n"
		"  clone <url> [<dir>]\n"
		"  hash-object [-w] <file>\n"
		"  cat-file -t|-s|-p <ref|hex>\n"
		"  write-tree [<dir>]\n"
		"  commit [-m <msg>] [-p <parent>] [<tree>]\n"
		"  remote add <name> <url>\n"
		"  push [<remote>] [<branch>]\n"
		"  pkt-encode <string>\n"
		"  pkt-decode\n"
		"  serve-refs [<path>]\n");
	exit(1);
}

static int
cmd_init(int argc, char **argv)
{
	const char *path = argc > 0 ? argv[0] : ".";
	if (repo_init(path) < 0)
		die("failed to initialize repository at '%s'", path);
	printf("Initialized empty Git repository in %s/.git/\n", path);
	return 0;
}

static int
cmd_add(int argc, char **argv)
{
	const char *path = argc > 0 ? argv[0] : ".";
	if (index_add(".", path) < 0)
		die("failed to add '%s' to index", path);
	return 0;
}

static int
cmd_clone(int argc, char **argv)
{
	if (argc < 1) usage();
	const char *url = argv[0];
	const char *dir = argc > 1 ? argv[1] : NULL;

	printf("Cloning repository '%s'...\n", url);
	if (clone_repo(url, dir) < 0)
		die("failed to clone repository");
	printf("Cloned successfully.\n");
	return 0;
}

static int
cmd_hash_object(int argc, char **argv)
{
	int write_flag = 0;
	const char *filepath = NULL;
	size_t fsz = 0;
	void *buf;
	char hex[41];
	GitObject obj;

	for (int i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "-w"))
			write_flag = 1;
		else
			filepath = argv[i];
	}

	if (!filepath) usage();

	buf = read_file(filepath, &fsz);
	if (!buf && fsz > 0) die("cannot read '%s'", filepath);

	if (write_flag) {
		if (object_write(".", OBJ_BLOB, buf, fsz, hex) < 0) {
			free(buf);
			die("failed to write object");
		}
	} else {
		if (object_hash(OBJ_BLOB, buf, fsz, &obj) < 0) {
			free(buf);
			die("failed to hash object");
		}
		strcpy(hex, obj.hex);
		object_free(&obj);
	}

	free(buf);
	puts(hex);
	return 0;
}

static int
cmd_cat_file(int argc, char **argv)
{
	GitObject obj;
	char hex[41];

	if (argc < 2) usage();

	if (ref_resolve(".", argv[1], hex) < 0)
		die("could not resolve ref/SHA '%s'", argv[1]);

	if (object_read(".", hex, &obj) < 0)
		die("could not read object '%s'", hex);

	if (!strcmp(argv[0], "-t"))
		puts(type_name(obj.type));
	else if (!strcmp(argv[0], "-s"))
		printf("%zu\n", obj.size);
	else if (!strcmp(argv[0], "-p")) {
		if (obj.data && obj.size)
			fwrite(obj.data, 1, obj.size, stdout);
	} else {
		object_free(&obj);
		usage();
	}

	object_free(&obj);
	return 0;
}

static int
cmd_write_tree(int argc, char **argv)
{
	char hex[41];
	const char *dir = argc > 0 ? argv[0] : ".";

	if (tree_write(".", dir, hex) < 0)
		die("failed to write tree for '%s'", dir);
	puts(hex);
	return 0;
}

static int
cmd_commit(int argc, char **argv)
{
	const char *tree_param = NULL, *msg = "commit";
	char tree_hex[41], parent_hex[41], commit_hex[41];
	char *parent = NULL;

	for (int i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "-p") && i + 1 < argc)
			parent = argv[++i];
		else if (!strcmp(argv[i], "-m") && i + 1 < argc)
			msg = argv[++i];
		else if (!tree_param && argv[i][0] != '-')
			tree_param = argv[i];
	}

	if (tree_param) {
		if (ref_resolve(".", tree_param, tree_hex) < 0)
			die("could not resolve tree '%s'", tree_param);
	} else {
		if (index_write_tree(".", tree_hex) < 0)
			die("failed to write tree");
	}

	if (parent) {
		if (ref_resolve(".", parent, parent_hex) == 0)
			parent = parent_hex;
	} else {
		if (ref_resolve(".", "HEAD", parent_hex) == 0)
			parent = parent_hex;
	}

	if (commit_create(".", tree_hex, parent, "User", "user@localhost", msg, commit_hex) < 0)
		die("failed to create commit");

	ref_update(".", "refs/heads/main", commit_hex);
	puts(commit_hex);
	return 0;
}

static int
cmd_remote(int argc, char **argv)
{
	if (argc < 3 || strcmp(argv[0], "add")) usage();
	if (remote_add(".", argv[1], argv[2]) < 0)
		die("failed to add remote '%s'", argv[1]);
	return 0;
}

static int
cmd_push(int argc, char **argv)
{
	const char *remote = argc > 0 ? argv[0] : "origin";
	const char *branch = argc > 1 ? argv[1] : "main";

	printf("Pushing to remote '%s' branch '%s'...\n", remote, branch);
	if (push_repo(".", remote, branch) < 0)
		die("failed to push to remote '%s'", remote);
	printf("Push successful.\n");
	return 0;
}

static int
cmd_pkt_encode(int argc, char **argv)
{
	if (argc < 1) usage();
	pkt_write_str(STDOUT_FILENO, argv[0]);
	return 0;
}

static int
cmd_pkt_decode(int argc, char **argv)
{
	char buf[65536];
	int len;
	(void)argc; (void)argv;

	while ((len = pkt_read(STDIN_FILENO, buf, sizeof(buf))) >= 0) {
		if (len == 0)
			puts("[PKT-FLUSH]");
		else
			printf("[PKT-LINE %d bytes] %s\n", len, buf);
	}
	return 0;
}

static int
cmd_serve_refs(int argc, char **argv)
{
	return proto_serve_refs(STDIN_FILENO, STDOUT_FILENO, argc > 0 ? argv[0] : ".");
}

int
main(int argc, char **argv)
{
	if (argc < 2) usage();

	if (!strcmp(argv[1], "init"))        return cmd_init(argc - 2, argv + 2);
	if (!strcmp(argv[1], "add"))         return cmd_add(argc - 2, argv + 2);
	if (!strcmp(argv[1], "clone"))       return cmd_clone(argc - 2, argv + 2);
	if (!strcmp(argv[1], "hash-object")) return cmd_hash_object(argc - 2, argv + 2);
	if (!strcmp(argv[1], "cat-file"))    return cmd_cat_file(argc - 2, argv + 2);
	if (!strcmp(argv[1], "write-tree"))  return cmd_write_tree(argc - 2, argv + 2);
	if (!strcmp(argv[1], "commit") || !strcmp(argv[1], "commit-tree"))
		return cmd_commit(argc - 2, argv + 2);
	if (!strcmp(argv[1], "remote"))      return cmd_remote(argc - 2, argv + 2);
	if (!strcmp(argv[1], "push"))        return cmd_push(argc - 2, argv + 2);
	if (!strcmp(argv[1], "pkt-encode"))  return cmd_pkt_encode(argc - 2, argv + 2);
	if (!strcmp(argv[1], "pkt-decode"))  return cmd_pkt_decode(argc - 2, argv + 2);
	if (!strcmp(argv[1], "serve-refs"))  return cmd_serve_refs(argc - 2, argv + 2);

	usage();
	return 1;
}