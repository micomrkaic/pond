#!/bin/sh
# usage: ./push_this.sh ~/Downloads/pond-0.5-src.tar.gz "GPU caustics and HOS"
set -e
cd ~/work/c_progs/pond
tarball=${1:?tarball path}
msg=${2:-"update from tarball"}

tar xzf "$tarball" --strip-components=1 pond/src pond/web pond/tests pond/Makefile pond/README.md
make && make test
git add -A
git commit -m "$msg"
make web WEB_DIR=docs
git add docs
git commit -m "Web build"
git push
