#include <stdlib.h>
#include "lib/mlrutil.h"
#include "containers/lrec_parsers.h"
#include "input/file_reader_mmap.h"
#include "input/lrec_readers.h"

typedef struct _lrec_reader_nidx_mmap_state_t {
	char irs;
	char ifs;
	int  allow_repeat_ifs;
} lrec_reader_nidx_mmap_state_t;

// ----------------------------------------------------------------
static lrec_t* lrec_reader_nidx_mmap_func(file_reader_mmap_state_t* phandle, void* pvstate, context_t* pctx) {
	lrec_reader_nidx_mmap_state_t* pstate = pvstate;
	if (phandle->sol >= phandle->eof) // xxx encapsulate a method for this ...
		return NULL;
	else
		return lrec_parse_nidx_mmap(phandle, pstate->irs, pstate->ifs, pstate->allow_repeat_ifs);
}

// No-op for stateless readers such as this one.
static void reset_nidx_func(void* pvstate) {
}

lrec_reader_mmap_t* lrec_reader_nidx_mmap_alloc(char irs, char ifs, int allow_repeat_ifs) {
	lrec_reader_mmap_t* plrec_reader = mlr_malloc_or_die(sizeof(lrec_reader_mmap_t));

	lrec_reader_nidx_mmap_state_t* pstate = mlr_malloc_or_die(sizeof(lrec_reader_nidx_mmap_state_t));
	pstate->irs              = irs;
	pstate->ifs              = ifs;
	pstate->allow_repeat_ifs = allow_repeat_ifs;
	plrec_reader->pvstate         = (void*)pstate;

	plrec_reader->plrec_reader_func = &lrec_reader_nidx_mmap_func;
	plrec_reader->preset_func  = &reset_nidx_func;

	return plrec_reader;
}
