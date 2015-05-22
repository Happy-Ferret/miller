#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lib/mlrutil.h"
#include "lib/mlr_globals.h"
#include "containers/lrec.h"
#include "containers/sllv.h"
#include "input/lrec_readers.h"
#include "mapping/mappers.h"
#include "output/lrec_writers.h"

static int do_file_chained(char* filename, context_t* pctx,
	lrec_reader_t* plrec_reader, sllv_t* pmapper_list, lrec_writer_t* plrec_writer, FILE* output_stream);
static int do_file_chained_mmap(char* filename, context_t* pctx,
	lrec_reader_mmap_t* plrec_reader, sllv_t* pmapper_list, lrec_writer_t* plrec_writer, FILE* output_stream);

static sllv_t* chain_map(lrec_t* pinrec, context_t* pctx, sllve_t* pmapper_list_head);

static void drive_lrec(lrec_t* pinrec, context_t* pctx, sllve_t* pmapper_list_head, lrec_writer_t* plrec_writer, FILE* output_stream);

// ----------------------------------------------------------------
// xxx assert pmapper_list non-empty ...
int do_stream_chained(char** filenames, int use_file_reader_mmap, lrec_reader_t* plrec_reader, lrec_reader_mmap_t* plrec_reader_mmap,
	sllv_t* pmapper_list, lrec_writer_t* plrec_writer, char* ofmt)
{
	FILE* output_stream = stdout;

	context_t ctx = { .nr = 0, .fnr = 0, .filenum = 0, .filename = NULL };
	int ok = 1;
	if (*filenames == NULL) {
		ctx.filenum++;
		ctx.filename = "(stdin)";
		ctx.fnr = 0;
		ok = do_file_chained("-", &ctx, plrec_reader, pmapper_list, plrec_writer, output_stream) && ok;
	} else {
		for (char** pfilename = filenames; *pfilename != NULL; pfilename++) {
			ctx.filenum++;
			ctx.filename = *pfilename;
			ctx.fnr = 0;
			// Start-of-file hook, e.g. expecting CSV headers on input.
			if (use_file_reader_mmap) {
				plrec_reader_mmap->preset_func(plrec_reader_mmap->pvstate);
				ok = do_file_chained_mmap(*pfilename, &ctx, plrec_reader_mmap, pmapper_list, plrec_writer, output_stream) && ok;
			} else {
				plrec_reader->preset_func(plrec_reader->pvstate);
				ok = do_file_chained(*pfilename, &ctx, plrec_reader, pmapper_list, plrec_writer, output_stream) && ok;
			}
		}
	}

	// Mappers and writers receive end-of-stream notifications via null input record.
	// Do that, now that data from all input file(s) have been exhausted.
	drive_lrec(NULL, &ctx, pmapper_list->phead, plrec_writer, output_stream);

	// Drain the pretty-printer.
	plrec_writer->plrec_writer_func(output_stream, NULL, plrec_writer->pvstate);

	return ok;
}

// ----------------------------------------------------------------
static int do_file_chained(char* filename, context_t* pctx,
	lrec_reader_t* plrec_reader, sllv_t* pmapper_list, lrec_writer_t* plrec_writer, FILE* output_stream)
{
	FILE* input_stream = stdin;

	if (!streq(filename, "-")) {
		input_stream = fopen(filename, "r");
		if (input_stream == NULL) {
			fprintf(stderr, "%s: Couldn't open \"%s\" for read.\n", MLR_GLOBALS.argv0, filename);
			perror(filename);
			return 0;
		}
	}

	while (1) {
		lrec_t* pinrec = plrec_reader->plrec_reader_func(input_stream, plrec_reader->pvstate, pctx);
		if (pinrec == NULL)
			break;
		pctx->nr++;
		pctx->fnr++;
		drive_lrec(pinrec, pctx, pmapper_list->phead, plrec_writer, output_stream);
	}

	if (input_stream != stdin)
		fclose(input_stream);

	return 1;
}

// ----------------------------------------------------------------
static int do_file_chained_mmap(char* filename, context_t* pctx,
	lrec_reader_mmap_t* plrec_reader, sllv_t* pmapper_list, lrec_writer_t* plrec_writer, FILE* output_stream)
{
	// xxx communicate error back from open, or rename it to ..._open_or_die
	file_reader_mmap_state_t handle = file_reader_mmap_open(filename);

	while (1) {
		lrec_t* pinrec = plrec_reader->plrec_reader_func(&handle, plrec_reader->pvstate, pctx);
		if (pinrec == NULL)
			break;
		pctx->nr++;
		pctx->fnr++;
		drive_lrec(pinrec, pctx, pmapper_list->phead, plrec_writer, output_stream);
	}

	file_reader_mmap_close(&handle);
	return 1;
}

// ----------------------------------------------------------------
static void drive_lrec(lrec_t* pinrec, context_t* pctx, sllve_t* pmapper_list_head, lrec_writer_t* plrec_writer, FILE* output_stream) {
	sllv_t* outrecs = chain_map(pinrec, pctx, pmapper_list_head);
	if (outrecs != NULL) {
		for (sllve_t* pe = outrecs->phead; pe != NULL; pe = pe->pnext) {
			lrec_t* poutrec = pe->pvdata;
			if (poutrec != NULL)
				plrec_writer->plrec_writer_func(output_stream, poutrec, plrec_writer->pvstate);
			// doc & encode convention that writer frees.
		}
		sllv_free(outrecs); // xxx cmt mem-mgmt
	}
}

// ----------------------------------------------------------------
// Map a single input record (maybe null at end of input stream) to zero or more output record.
// Return: list of lrec_t*. Input: lrec_t* and list of mapper_t*.
// xxx need to figure out mem-mgmt here
static sllv_t* chain_map(lrec_t* pinrec, context_t* pctx, sllve_t* pmapper_list_head) {
	mapper_t* pmapper = pmapper_list_head->pvdata;
	sllv_t* outrecs = pmapper->pmapper_process_func(pinrec, pctx, pmapper->pvstate);
	if (pmapper_list_head->pnext == NULL) {
		return outrecs;
	} else if (outrecs == NULL) { // xxx cmt
		return NULL;
	} else {
		sllv_t* nextrecs = sllv_alloc();

		for (sllve_t* pe = outrecs->phead; pe != NULL; pe = pe->pnext) {
			lrec_t* poutrec = pe->pvdata;
			sllv_t* nextrecsi = chain_map(poutrec, pctx, pmapper_list_head->pnext);
			nextrecs = sllv_append(nextrecs, nextrecsi);
		}
		sllv_free(outrecs);

		return nextrecs;
	}
}
