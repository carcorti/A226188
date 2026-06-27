CC ?= gcc
PKG_CONFIG ?= pkg-config

MAKEFILE_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
ROOT := $(abspath $(MAKEFILE_DIR)/..)

SRC := $(ROOT)/src/a226188.c
BIN := $(ROOT)/build/a226188
OUTPUTS := $(ROOT)/outputs/b226188_candidate.txt \
	$(ROOT)/outputs/a226188_terms.tsv \
	$(ROOT)/outputs/a226188_validation.tsv \
	$(ROOT)/outputs/a226188_manifest.json

CPPFLAGS ?= -D_POSIX_C_SOURCE=200809L
CFLAGS ?= -std=c17 -O2 -Wall -Wextra -Wshadow -Wconversion -Wdouble-promotion -Wformat=2 -pedantic
MPFR_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags mpfr gmp 2>/dev/null)
MPFR_LIBS ?= $(shell $(PKG_CONFIG) --libs mpfr gmp 2>/dev/null)
ifeq ($(strip $(MPFR_LIBS)),)
$(error pkg-config could not find mpfr/gmp; install libmpfr-dev and libgmp-dev, or set MPFR_CFLAGS and MPFR_LIBS explicitly)
endif
LDLIBS := $(MPFR_LIBS) -lm

.PHONY: all verify-known generate test clean clean-outputs distclean

all: $(BIN)

$(BIN): $(SRC) | $(ROOT)/build
	$(CC) $(CPPFLAGS) $(MPFR_CFLAGS) $(CFLAGS) $< -o $@ $(LDLIBS)

$(ROOT)/build:
	mkdir -p $@

verify-known: $(BIN)
	cd $(ROOT) && ./build/a226188 verify-known

generate: $(BIN)
	cd $(ROOT) && ./build/a226188 generate
	cd $(ROOT) && tmp=$$(mktemp outputs/.a226188_checksums.sha256.XXXXXX) && \
		trap 'rm -f "$$tmp"' EXIT INT TERM && \
		sha256sum outputs/b226188_candidate.txt outputs/a226188_terms.tsv outputs/a226188_validation.tsv outputs/a226188_manifest.json > "$$tmp" && \
		chmod 0644 "$$tmp" && \
		sync -f "$$tmp" && \
		mv "$$tmp" outputs/a226188_checksums.sha256 && \
		sync -f outputs && \
		trap - EXIT INT TERM

test: verify-known generate
	tmpdir=$$(mktemp -d) && \
		trap 'rm -rf "$$tmpdir"' EXIT INT TERM && \
		head -n 27 $(ROOT)/outputs/b226188_candidate.txt > "$$tmpdir/generated.txt" && \
		head -n 27 $(ROOT)/data/b226188.txt > "$$tmpdir/reference.txt" && \
		cmp "$$tmpdir/generated.txt" "$$tmpdir/reference.txt" && \
		trap - EXIT INT TERM && \
		rm -rf "$$tmpdir"

clean:
	rm -f $(BIN)

clean-outputs:
	rm -f $(OUTPUTS) $(ROOT)/outputs/a226188_checksums.sha256

distclean: clean clean-outputs
