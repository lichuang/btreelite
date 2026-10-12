# btreelite Makefile
#
# Phase 0 build: compile the extracted storage sources in src/ against
# src/btreeliteInt.h.  Sources are compiled unmodified from SQLite at this
# stage; unresolved-symbol analysis (make scan) drives the Phase 1-4
# seam work.
#
CC      ?= cc
AR      ?= ar
CFLAGS  ?= -O2
CFLAGS  += -Isrc -Iinclude -Wall -Wno-unused-parameter -Wno-deprecated-declarations
# Debug build:  make CFLAGS="-O0 -g -DSQLITE_DEBUG"
# Release build keeps NDEBUG (asserts off).

SRCDIR  = src
LIB     = libbtreelite.a

HDRS    = btreeliteInt.h btreeInt.h pager.h wal.h pcache.h pcache1.h \
          os.h os_common.h os_setup.h mutex.h

# Order matters for the final link only; each file compiles independently.
CORE_SOURCES = \
    btreelite_compat.c \
    malloc.c \
    global.c \
    mem1.c \
    fault.c \
    mutex_noop.c \
    status.c \
    random.c \
    mutex.c \
    mutex_unix.c \
    bitvec.c \
    util.c \
    printf.c \
    utf.c \
    memjournal.c \
    os.c \
    os_unix.c \
    pcache.c \
    pcache1.c \
    pager.c \
    wal.c \
    btree.c \
    btreelite_api.c

# The thin public-API layer is added in Phase 4; listed here so that
# "make" fails fast once it exists but is not yet compiled.
API_SOURCES =

SOURCES = $(CORE_SOURCES) $(API_SOURCES)
OBJECTS = $(SOURCES:%.c=$(SRCDIR)/%.o)

all: $(LIB)

# macOS: /usr/bin/libtool produces an archive the system linker accepts.
# GNU ar (homebrew binutils) writes a GNU symbol table that ld rejects.
$(LIB): $(OBJECTS)
	/usr/bin/libtool -static -o $@ $(OBJECTS)

$(SRCDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# Compile-only pass: report every translation unit that fails to compile
# without trying to link.  The error list is the Phase 1-2 work inventory.
compile-only: $(OBJECTS)

.PHONY: $(OBJECTS)
$(OBJECTS): $(SRCDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# Unresolved-symbol scan.  Compiles each object and greps the undefined
# references it makes.  Output feeds docs/phase.md phase planning.
scan: $(OBJECTS)
	@rm -rf scan.out && mkdir -p scan.out
	@for o in $(OBJECTS); do nm -u $$o > scan.out/`basename $$o`.undefined 2>/dev/null; done
	@awk '{print $$2}' scan.out/*.undefined | sort -u | grep -v '^_GLOBAL' \
	  | grep -v '^$$' > scan.out/undefined_symbols.txt
	@wc -l scan.out/undefined_symbols.txt

# SQL-layer coupling census: count references to sqlite3-API symbols that
# only exist in the SQL layer.  These are the seam points to cut in
# Phase 1-2.
census:
	@echo "---- per-file counts of SQL-layer symbols ----"
	@for f in $(SOURCES); do \
	  n=`grep -o 'sqlite3\(Malloc\|Realloc\|Free\|DbMalloc\|DbFree\|PageMalloc\|PageFree\|GlobalConfig\|MutexAlloc\|MutexInit\|MutexEnd\|StatusUp\|StatusDown\|StatusHighwater\|StatusValue\|BeginBenignMalloc\|EndBenignMalloc\|FaultSim\|Randomness\|StrAccum\|str_append\|str_appendf\|str_vappendf\|StrAccumInit\|StrAccumFinish\|InvokeBusyHandler\|ReportError\|MallocInit\|MallocEnd\|PcacheInitialize\|PcacheShutdown\|PcacheSetDefault\|OsInit\|IsNaN\|MemSetDefault\|Memdebug\|malloc64\|realloc64\|mprintf\|snprintf\|AbsInt32\|Get4byte\|Put4byte\|GetVarint\|PutVarint\|GetInt32\|Put2byte\|Get2byte\|Bitvec[A-Z]\|ConnectionBlocked\|TempInMemory\|WritableSchema\|Backup[A-Z]\|_exec\|database_file_object\|create_filename\|filename_database\|uri_[a-z]*\|Strlen30\|IsMemdb\|MemSetArrayInt64\)' $(SRCDIR)/$$f | wc -l`; \
	  echo "$$f: $$n"; \
	done

clean:
	rm -f $(OBJECTS) $(LIB)
	rm -rf scan.out

.PHONY: all compile-only scan census clean
# ----------------------------------------------------------------------
# Tests
# ----------------------------------------------------------------------
TESTBINS = test/t_smoke test/t_kv test/t_big test/t_wal test/t_api test/t_proc test/t_dur test/t_trees test/t_savepos
$(TESTBINS): test/%: test/%.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< $(LIB)

test: $(TESTBINS)
	cd test && ../test/t_smoke && ../test/t_kv && ../test/t_big && ../test/t_wal && ../test/t_api && ../test/t_proc && ../test/t_dur && ../test/t_trees && ../test/t_savepos

.PHONY: test

# ----------------------------------------------------------------------
# Examples: small programs demonstrating the public API (see examples/).
# ----------------------------------------------------------------------
EXAMPLES = examples/kv_demo examples/txn_demo examples/tree_demo
$(EXAMPLES): examples/%: examples/%.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< $(LIB)

examples: $(EXAMPLES)

.PHONY: examples

# ----------------------------------------------------------------------
# Benchmark: KV (btreelite) vs SQL (system libsqlite3) on the same load.
# The two engines are built from the one test/bench.c source.
# ----------------------------------------------------------------------
SQLINC  = /opt/homebrew/opt/sqlite/include
SQLLIB  = -L/opt/homebrew/opt/sqlite/lib -lsqlite3

# btreelite (KV) side.
bench_kv: test/bench.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< $(LIB)

# SQL side: same workload through prepared statements + libsqlite3.
bench_sql: test/bench.c
	$(CC) $(CFLAGS) -DBENCH_SQL -I$(SQLINC) -o $@ $< $(SQLLIB)

bench: bench_kv bench_sql
	./bench_sql $(BENCHARG) ; ./bench_kv $(BENCHARG)

.PHONY: bench

# ----------------------------------------------------------------------
# Fuzz (Target A: stateful API fuzz, docs/fuzz-design.md)
#   fuzz-seeds      regenerate fuzz/seeds/*.bin (checked-in regression corpus)
#   fuzz            replay the corpus twice per stream (sanitized, no deps)
#   fuzz-libfuzzer  build the coverage-guided libFuzzer binary
# The library is rebuilt with sanitizers into fuzz/srclib*/; the plain
# $(LIB) build is untouched.
# ----------------------------------------------------------------------
FUZZ_SAN  = -Isrc -Iinclude -I. -O1 -g -fsanitize=address,undefined \
            -fno-omit-frame-pointer -Wall -Wno-unused-parameter \
            -Wno-deprecated-declarations
FUZZ_FZZ_SAN = -Isrc -Iinclude -I. -O1 -g -fsanitize=address \
            -fno-omit-frame-pointer -Wall -Wno-unused-parameter \
            -Wno-deprecated-declarations
# (libFuzzer + UBSan do not link on macOS: the two runtimes carry
# duplicate symbol-table entries this ld64 rejects; the fuzzer builds
# use ASan only, which is the standard OSS-Fuzz mac configuration.)

fuzz/srclib fuzz/obj:
	mkdir -p fuzz/srclib fuzz/obj fuzz

FUZZSRCLIB_O = $(SOURCES:%.c=fuzz/srclib/%.o)
$(FUZZSRCLIB_O): fuzz/srclib/%.o: src/%.c fuzz/srclib
	$(CC) $(FUZZ_SAN) -c $< -o $@

fuzz/libbt_fz.a: $(FUZZSRCLIB_O)
	/usr/bin/libtool -static -o $@ $(FUZZSRCLIB_O)

FUZZ_HARNESS_OBJS = fuzz/obj/drv.o fuzz/obj/model.o fuzz/obj/target_api.o \
                    fuzz/obj/mkseeds.o fuzz/obj/replay.o fuzz/obj/wrap.o \
                    fuzz/obj/mkfiles.o fuzz/obj/target_file.o fuzz/obj/wrap_file.o
$(FUZZ_HARNESS_OBJS): fuzz/obj/%.o: fuzz/%.c fuzz/obj
	$(CC) $(FUZZ_SAN) -c $< -o $@

fuzz/mkseeds: fuzz/obj/mkseeds.o fuzz/obj/drv.o
	$(CC) $(FUZZ_SAN) -o $@ fuzz/obj/mkseeds.o fuzz/obj/drv.o

fuzz-seeds: fuzz/mkseeds
	./fuzz/mkseeds

# The replay/fuzz/fuzz-libfuzzer targets live in the Target B section
# below, so one `make fuzz` drives both corpora and both binaries.

# libFuzzer binary: coverage instrumentation needs a second library build
# with -fsanitize=fuzzer-no-link; the link adds -fsanitize=fuzzer itself.
fuzz/srclibz fuzz/objz:
	mkdir -p fuzz/srclibz fuzz/objz

FUZZZLIB_O = $(SOURCES:%.c=fuzz/srclibz/%.o)
$(FUZZZLIB_O): fuzz/srclibz/%.o: src/%.c fuzz/srclibz
	$(CC) $(FUZZ_FZZ_SAN) -fsanitize=fuzzer-no-link -c $< -o $@

FUZZZ_A = fuzz/objz/wrap.o fuzz/objz/drv.o fuzz/objz/model.o \
          fuzz/objz/target_api.o
FUZZZ_ALL = fuzz/objz/wrap_file.o fuzz/objz/target_file.o $(FUZZZ_A)
$(FUZZZ_ALL): fuzz/objz/%.o: fuzz/%.c fuzz/objz
	$(CC) $(FUZZ_FZZ_SAN) -fsanitize=fuzzer-no-link -c $< -o $@

# The libFuzzer links use raw objects (macOS libtool -static cannot carry
# fuzzer instrumentation through an archive: ld64 reports
# invalid r_symbolnum).  Each binary links its own wrap: two
# LLVMFuzzerTestOneInput definitions cannot share a link.
fuzz/fz_api: $(FUZZZ_A) $(FUZZZLIB_O)
	$(CC) $(FUZZ_FZZ_SAN) -fsanitize=fuzzer -o $@ $(FUZZZ_A) $(FUZZZLIB_O)

fuzz/fz_file: $(FUZZZ_ALL) $(FUZZZLIB_O)
	$(CC) $(FUZZ_FZZ_SAN) -fsanitize=fuzzer -o $@ \
		$(filter-out fuzz/objz/wrap.o,$(FUZZZ_ALL)) $(FUZZZLIB_O)

# ---- Target B: hostile-file fuzzing (fuzz/target_file.c) ---------------
# Its bases (fuzz/files/) are regenerated per run; the mutation streams
# (fuzz/fileseeds/) are checked in.
fuzz/mkfiles: fuzz/obj/mkfiles.o fuzz/obj/drv.o fuzz/libbt_fz.a
	$(CC) $(FUZZ_SAN) -o $@ fuzz/obj/mkfiles.o fuzz/obj/drv.o fuzz/libbt_fz.a

fuzz-files: fuzz/mkfiles
	./fuzz/mkfiles

fuzz/fuzz_replay: fuzz/obj/replay.o fuzz/obj/drv.o fuzz/obj/model.o \
                 fuzz/obj/target_api.o fuzz/obj/target_file.o fuzz/libbt_fz.a
	$(CC) $(FUZZ_SAN) -o $@ fuzz/obj/replay.o fuzz/obj/drv.o \
		fuzz/obj/model.o fuzz/obj/target_api.o fuzz/obj/target_file.o \
		fuzz/libbt_fz.a

fuzz: fuzz-seeds fuzz-files fuzz/fuzz_replay
	./fuzz/fuzz_replay fuzz/seeds fuzz/fileseeds



fuzz-libfuzzer: fuzz-seeds fuzz-files fuzz/fz_api fuzz/fz_file
	@echo "libFuzzer binaries ready:"
	@echo "  ./fuzz/fz_api  fuzz/seeds     -jobs=4 -max_total_time=3600"
	@echo "  ./fuzz/fz_file fuzz/fileseeds -jobs=4 -max_total_time=3600"

clean-fuzz:
	rm -rf fuzz/srclib fuzz/srclibz fuzz/obj fuzz/objz fuzz/libbt_fz.a \
	       fuzz/mkseeds fuzz/mkfiles fuzz/fuzz_replay fuzz/fz_api \
	       fuzz/fz_file fuzz/files fuzz/fileseeds

.PHONY: fuzz fuzz-seeds fuzz-files fuzz-libfuzzer clean-fuzz \
        fuzz/srclib fuzz/obj fuzz/srclibz fuzz/objz
