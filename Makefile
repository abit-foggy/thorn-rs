# thorn-rs - zero-cargo #![no_std] Rust static libraries for thorn
CC     ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Wpedantic
PITH_INC ?= vendor/pith/include

# The plugin unit renames the brain's shared helpers so dist/plugin.o
# never collides with thorn's own engine symbols at link time.
RS_RENAME = -Ddiag=rs_diag \
	-Dstrlist_push=rs_strlist_push \
	-Dstrlist_free=rs_strlist_free \
	-Dhas_whitespace=rs_has_whitespace \
	-Dpath_safe=rs_path_safe \
	-Drust_crate_init=rs_crate_init \
	-Drust_crate_free=rs_crate_free \
	-Dscan_crate=rs_scan_crate \
	-Dplan_scrub_env=rs_scrub_env \
	-Dplan_locate_rustc=rs_locate_rustc \
	-Dplan_probe_version=rs_probe_version \
	-Dplan_rustflags=rs_rustflags \
	-Dplan_command=rs_plan_command \
	-Dverify_archive=rs_verify_archive \
	-Dverify_command=rs_verify_command \
	-Dverify_expected=rs_verify_expected \
	-Dverify_edge_command=rs_verify_edge_command

SRC = src/main.c src/rust.c src/scan.c src/plan.c \
      src/emit_ninja.c src/emit_make.c src/verify.c
HDR = src/include/rust.h src/include/scan.h \
      src/include/plan.h src/include/emit.h src/include/verify.h
BIN = bin/thorn-rs

.PHONY: all test check clean

all: $(BIN) dist/thorn-rs.tpkg

$(BIN): $(SRC) $(HDR)
	@mkdir -p bin
	$(CC) $(CFLAGS) -I src -o $@ $(SRC)

dist/plugin.o: src/plugin.c $(SRC) $(HDR)
	@mkdir -p dist
	$(CC) -std=c99 -O2 -Wall -Wextra $(RS_RENAME) -I src -I $(PITH_INC) \
		-c src/plugin.c -o $@

dist/manifest: dist/plugin.o
	printf 'author = "abit-foggy"\nmodule = "rust"\n' > $@

dist/thorn-rs.tpkg: dist/plugin.o dist/manifest
	tar -cf $@ -C dist plugin.o manifest

test check: $(BIN)
	./tests/verify.sh

clean:
	rm -rf bin out out-ninja out-make tests/out dist .thorn-rs
