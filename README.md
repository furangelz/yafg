# yafg

a minimal, git-compatible object storage and protocol engine written in posix c.

## features

- [x] `init` — create empty repository structure
- [x] `add` — stage files into index
- [x] `clone` — fetch and unpack remote repos over smart http
- [x] `hash-object` — compute sha-1 hash & store blobs
- [x] `cat-file` — inspect loose object content, type, or size by ref/sha
- [x] `write-tree` — recursively scan directory & construct git tree object
- [x] `commit` — construct commit object with automatic tree & parent sha resolution
- [x] `remote` — configure remote repository URLs (`remote add`)
- [x] `push` — send local commits & packfiles to remote via smart http (`git-receive-pack`)
- [x] `serve-refs` — ref advertisement server for `upload-pack`

## building and installation

### dependencies
- `c99` compiler
- `zlib`
- `openssl` (`libcrypto`)
- `curl`

### compilation
```bash
cd src
make
doas make install
```
(or `sudo make install` depending on your distro)

## usage

```bash
# initialize repository
yafg init myrepo
cd myrepo

# create objects and commit
echo "hello world" > main.c
yafg add .
yafg commit -m "initial commit"

# inspect commit object by ref
yafg cat-file -p HEAD

# add remote and push
yafg remote add origin https://github.com/octocat/Hello-World.git
yafg push origin main

# clone external repository
yafg clone https://github.com/octocat/Hello-World.git
```

## benchmarks

to be measured and compared to official git.

## roadmap

- [ ] `fetch`
- [x] `push`
- [ ] `ls-remote`
- [ ] `status`
- [ ] `log`
- [ ] `diff`
- [ ] `checkout`
- [ ] `branch`
- [ ] `tag`
- [x] `remote`
- [ ] `config`
- [ ] `rm`
- [ ] `mv`
