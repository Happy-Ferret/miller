#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "lib/mlr_globals.h"
#include "lib/mlrutil.h"
#include "lib/string_builder.h"
#include "input/file_reader_mmap.h"
#include "input/lrec_readers.h"
#include "input/peek_file_reader.h"
#include "containers/rslls.h"
#include "containers/lhmslv.h"
#include "containers/parse_trie.h"

// Idea of pheader_keepers: each header_keeper object retains the input-line backing
// and the slls_t for a CSV header line which is used by one or more CSV data
// lines.  Meanwhile some mappers retain input records from the entire data
// stream, including header-schema changes in the input stream. This means we
// need to keep headers intact as long as any lrecs are pointing to them.  One
// option is reference-counting which I experimented with; it was messy and
// error-prone. The approach used here is to keep a hash map from header-schema
// to header_keeper object. The current pheader_keeper is a pointer into one of
// those.  Then when the reader is freed, all the header-keepers are freed.

// ----------------------------------------------------------------
#define STRING_BUILDER_INIT_SIZE 1024

// AKA "token"
#define EOF_STRIDX           0x2000
#define IRS_STRIDX           0x2001
#define IFS_EOF_STRIDX       0x2002
#define IFS_STRIDX           0x2003
#define DQUOTE_STRIDX        0x2004
#define DQUOTE_IRS_STRIDX    0x2005
#define DQUOTE_IFS_STRIDX    0x2006
#define DQUOTE_EOF_STRIDX    0x2007
#define DQUOTE_DQUOTE_STRIDX 0x2008

// ----------------------------------------------------------------
typedef struct _lrec_reader_mmap_csv_state_t {
	// Input line number is not the same as the record-counter in context_t,
	// which counts records.
	long long  ilno;

	char* eof;
	char* irs;
	char* ifs_eof;
	char* ifs;
	char* dquote;
	char* dquote_irs;
	char* dquote_ifs;
	char* dquote_eof;
	char* dquote_dquote;

	int   dquotelen;

	rslls_t*            pfields;
	string_builder_t*   psb;

	parse_trie_t*       pno_dquote_parse_trie;
	parse_trie_t*       pdquote_parse_trie;

	int                 expect_header_line_next;
	int                 use_implicit_header;
	header_keeper_t*    pheader_keeper;
	lhmslv_t*           pheader_keepers;

} lrec_reader_mmap_csv_state_t;

static void    lrec_reader_mmap_csv_free(lrec_reader_t* preader);
static void    lrec_reader_mmap_csv_sof(void* pvstate);
static void*   lrec_reader_mmap_csv_vopen(void* pvstate, char* prepipe, char* file_name);
static lrec_t* lrec_reader_mmap_csv_process(void* pvstate, void* pvhandle, context_t* pctx);
static int     lrec_reader_mmap_csv_get_fields(lrec_reader_mmap_csv_state_t* pstate,
	rslls_t* pfields, file_reader_mmap_state_t* phandle);
static lrec_t* paste_indices_and_data(lrec_reader_mmap_csv_state_t* pstate, rslls_t* pdata_fields, context_t* pctx);
static lrec_t* paste_header_and_data(lrec_reader_mmap_csv_state_t* pstate, rslls_t* pdata_fields, context_t* pctx);

// ----------------------------------------------------------------
lrec_reader_t* lrec_reader_mmap_csv_alloc(char* irs, char* ifs, int use_implicit_header) {
	lrec_reader_t* plrec_reader = mlr_malloc_or_die(sizeof(lrec_reader_t));

	lrec_reader_mmap_csv_state_t* pstate = mlr_malloc_or_die(sizeof(lrec_reader_mmap_csv_state_t));
	pstate->ilno          = 0LL;

	pstate->eof           = "\xff";
	pstate->irs           = irs;
	pstate->ifs           = ifs;
	pstate->ifs_eof       = mlr_paste_2_strings(pstate->ifs, "\xff");
	pstate->dquote        = "\"";

	pstate->dquote_irs    = mlr_paste_2_strings("\"", pstate->irs);
	pstate->dquote_ifs    = mlr_paste_2_strings("\"", pstate->ifs);
	pstate->dquote_eof    = "\"\xff";
	pstate->dquote_dquote = "\"\"";

	pstate->dquotelen     = strlen(pstate->dquote);

	pstate->pno_dquote_parse_trie = parse_trie_alloc();
	parse_trie_add_string(pstate->pno_dquote_parse_trie, pstate->eof,     EOF_STRIDX);
	parse_trie_add_string(pstate->pno_dquote_parse_trie, pstate->irs,     IRS_STRIDX);
	parse_trie_add_string(pstate->pno_dquote_parse_trie, pstate->ifs_eof, IFS_EOF_STRIDX);
	parse_trie_add_string(pstate->pno_dquote_parse_trie, pstate->ifs,     IFS_STRIDX);
	parse_trie_add_string(pstate->pno_dquote_parse_trie, pstate->dquote,  DQUOTE_STRIDX);

	pstate->pdquote_parse_trie = parse_trie_alloc();
	parse_trie_add_string(pstate->pdquote_parse_trie, pstate->eof,           EOF_STRIDX);
	parse_trie_add_string(pstate->pdquote_parse_trie, pstate->dquote_irs,    DQUOTE_IRS_STRIDX);
	parse_trie_add_string(pstate->pdquote_parse_trie, pstate->dquote_ifs,    DQUOTE_IFS_STRIDX);
	parse_trie_add_string(pstate->pdquote_parse_trie, pstate->dquote_eof,    DQUOTE_EOF_STRIDX);
	parse_trie_add_string(pstate->pdquote_parse_trie, pstate->dquote_dquote, DQUOTE_DQUOTE_STRIDX);

	pstate->pfields = rslls_alloc();
	pstate->psb = sb_alloc(STRING_BUILDER_INIT_SIZE);

	pstate->expect_header_line_next   = use_implicit_header ? FALSE : TRUE;
	pstate->use_implicit_header       = use_implicit_header;
	pstate->pheader_keeper            = NULL;
	pstate->pheader_keepers           = lhmslv_alloc();

	plrec_reader->pvstate       = (void*)pstate;
	plrec_reader->popen_func    = lrec_reader_mmap_csv_vopen;
	plrec_reader->pclose_func   = file_reader_mmap_vclose;
	plrec_reader->pprocess_func = lrec_reader_mmap_csv_process;
	plrec_reader->psof_func     = lrec_reader_mmap_csv_sof;
	plrec_reader->pfree_func    = lrec_reader_mmap_csv_free;

	return plrec_reader;
}

// ----------------------------------------------------------------
static void lrec_reader_mmap_csv_free(lrec_reader_t* preader) {
	lrec_reader_mmap_csv_state_t* pstate = preader->pvstate;
	for (lhmslve_t* pe = pstate->pheader_keepers->phead; pe != NULL; pe = pe->pnext) {
		header_keeper_t* pheader_keeper = pe->pvvalue;
		header_keeper_free(pheader_keeper);
	}

	// header-fields lists are doubly referenced: as hashmap keys in
	// pstate->pheader_keepers, and within the header_keeper objects.
	// Nullify the keys here to avoid a double free.
	// This could be refactored to be more elegant.
	for (lhmslve_t* pe = pstate->pheader_keepers->phead; pe != NULL; pe = pe->pnext) {
		pe->key = NULL;
	}
	lhmslv_free(pstate->pheader_keepers);

	parse_trie_free(pstate->pno_dquote_parse_trie);
	parse_trie_free(pstate->pdquote_parse_trie);
	rslls_free(pstate->pfields);
	sb_free(pstate->psb);
	free(pstate->ifs_eof);
	free(pstate->dquote_irs);
	free(pstate->dquote_ifs);
	free(pstate);
	free(preader);
}

// ----------------------------------------------------------------
static void lrec_reader_mmap_csv_sof(void* pvstate) {
	lrec_reader_mmap_csv_state_t* pstate = pvstate;
	pstate->ilno = 0LL;
	pstate->expect_header_line_next = pstate->use_implicit_header ? FALSE : TRUE;
}

// ----------------------------------------------------------------
static void* lrec_reader_mmap_csv_vopen(void* pvstate, char* prepipe, char* file_name) {
	void* pvhandle = file_reader_mmap_open(prepipe, file_name);
	file_reader_mmap_state_t* phandle = pvhandle;
	*phandle->eof = EOF;
	return pvhandle;
}

// ----------------------------------------------------------------
static lrec_t* lrec_reader_mmap_csv_process(void* pvstate, void* pvhandle, context_t* pctx) {
	lrec_reader_mmap_csv_state_t* pstate = pvstate;
	file_reader_mmap_state_t* phandle = pvhandle;

	if (pstate->expect_header_line_next) {
		if (!lrec_reader_mmap_csv_get_fields(pstate, pstate->pfields, phandle))
			return NULL;
		pstate->ilno++;

		slls_t* pheader_fields = slls_alloc();
		int i = 0;
		for (rsllse_t* pe = pstate->pfields->phead; i < pstate->pfields->length && pe != NULL; pe = pe->pnext, i++) {
			if (*pe->value == 0) {
				fprintf(stderr, "%s: unacceptable empty CSV key at file \"%s\" line %lld.\n",
					MLR_GLOBALS.argv0, pctx->filename, pstate->ilno);
				exit(1);
			}
			// Transfer pointer-free responsibility from the rslls to the
			// header fields in the header keeper
			slls_add(pheader_fields, pe->value, pe->free_flag);
			pe->free_flag = 0;
		}
		rslls_reset(pstate->pfields);

		pstate->pheader_keeper = lhmslv_get(pstate->pheader_keepers, pheader_fields);
		if (pstate->pheader_keeper == NULL) {
			pstate->pheader_keeper = header_keeper_alloc(NULL, pheader_fields);
			lhmslv_put(pstate->pheader_keepers, pheader_fields, pstate->pheader_keeper);
		} else { // Re-use the header-keeper in the header cache
			slls_free(pheader_fields);
		}

		pstate->expect_header_line_next = FALSE;
	}
	int rc = lrec_reader_mmap_csv_get_fields(pstate, pstate->pfields, phandle);
	pstate->ilno++;
	if (rc == FALSE) // EOF
		return NULL;
	else {
		lrec_t* prec = pstate->use_implicit_header
			? paste_indices_and_data(pstate, pstate->pfields, pctx)
			: paste_header_and_data(pstate, pstate->pfields, pctx);
		rslls_reset(pstate->pfields);
		return prec;
	}
}

static int lrec_reader_mmap_csv_get_fields(lrec_reader_mmap_csv_state_t* pstate,
	rslls_t* pfields, file_reader_mmap_state_t* phandle)
{
	int rc, stridx, matchlen, record_done, field_done;
	string_builder_t* psb = pstate->psb;

	if (phandle->sol >= phandle->eof)
		return FALSE;

	char* p = phandle->sol;
	char* e = p;

	// loop over fields in record
	record_done = FALSE;
	while (!record_done) {
		// Assumption is dquote is "\""
		if (*e != pstate->dquote[0]) { // start of non-quoted field

			// Loop over characters in field
			field_done = FALSE;
			while (!field_done) {
				rc = parse_trie_match(pstate->pno_dquote_parse_trie, e, phandle->eof, &stridx, &matchlen);
				if (rc) {
					switch(stridx) {
					case EOF_STRIDX: // end of record
						*e = 0;
						rslls_add_no_free(pfields, p);
						p = e + matchlen;
						field_done  = TRUE;
						record_done = TRUE;
						break;
					case IFS_EOF_STRIDX:
						fprintf(stderr, "%s: syntax error: record-ending field separator at line %lld.\n",
							MLR_GLOBALS.argv0, pstate->ilno);
						exit(1);
						break;
					case IFS_STRIDX: // end of field
						*e = 0;
						rslls_add_no_free(pfields, p);
						p = e + matchlen;
						field_done  = TRUE;
						break;
					case IRS_STRIDX: // end of record
						*e = 0;
						rslls_add_no_free(pfields, p);
						p = e + matchlen;
						field_done  = TRUE;
						record_done = TRUE;
						break;
					case DQUOTE_STRIDX: // CSV syntax error: fields containing quotes must be fully wrapped in quotes
						fprintf(stderr, "%s: syntax error: unwrapped double quote at line %lld.\n",
							MLR_GLOBALS.argv0, pstate->ilno);
						exit(1);
						break;
					default:
						fprintf(stderr, "%s: internal coding error: unexpected token %d at line %lld.\n",
							MLR_GLOBALS.argv0, stridx, pstate->ilno);
						exit(1);
						break;
					}
					e += matchlen;
				} else if (e >= phandle->eof) {
					// Note: for the mmap reader, this is awkward duplication with respect to EOF_STRIDX.
					// (In the stdio-reader case there is no duplication.)
					*e = 0;
					rslls_add_no_free(pfields, p);
					p = e + matchlen;
					field_done  = TRUE;
					record_done = TRUE;
					break;
				} else {
					e++;
				}
			}

		} else { // start of quoted field
			e += pstate->dquotelen;
			p = e;

			// loop over characters in field
			field_done = FALSE;
			int contiguous = TRUE;
			// If there are no embedded double-double quotes, then the field value is a contiguous
			// array of bytes between the start and end double-quotes (non-inclusive). E.g. "ab,c"
			// has contents ab,c. In that case we can point the rslls at that range of bytes
			// with no data-copying. However, if there are embedded double-double quotes, then
			// we use the string-build logic to build up a dynamically allocated string. E.g.
			// "ab""c" becomes ab"c.
			while (!field_done) {

				rc = parse_trie_match(pstate->pdquote_parse_trie, e, phandle->eof, &stridx, &matchlen);

				if (rc) {
					switch(stridx) {
					case EOF_STRIDX: // end of record
						fprintf(stderr, "%s: imbalanced double-quote at line %lld.\n",
							MLR_GLOBALS.argv0, pstate->ilno);
						exit(1);
						break;
					case DQUOTE_EOF_STRIDX: // end of record
						*e = 0;
						if (contiguous)
							rslls_add_no_free(pfields, p);
						else
							rslls_add_with_free(pfields, sb_finish(psb));
						p = e + matchlen;
						field_done  = TRUE;
						record_done = TRUE;
						break;
					case DQUOTE_IFS_STRIDX: // end of field
						*e = 0;
						if (contiguous)
							rslls_add_no_free(pfields, p);
						else
							rslls_add_with_free(pfields, sb_finish(psb));
						p = e + matchlen;
						field_done  = TRUE;
						break;
					case DQUOTE_IRS_STRIDX: // end of record
						*e = 0;
						if (contiguous)
							rslls_add_no_free(pfields, p);
						else
							rslls_add_with_free(pfields, sb_finish(psb));
						p = e + matchlen;
						field_done  = TRUE;
						record_done = TRUE;
						break;
					case DQUOTE_DQUOTE_STRIDX: // RFC-4180 CSV: "" inside a dquoted field is an escape for "
						if (contiguous) { // not anymore it isn't
							sb_append_char_range(psb, p, e);
							contiguous = FALSE;
						} else {
							sb_append_char(psb, pstate->dquote[0]);
						}
						break;
					default:
						fprintf(stderr, "%s: internal coding error: unexpected token %d at line %lld.\n",
							MLR_GLOBALS.argv0, stridx, pstate->ilno);
						exit(1);
						break;
					}
					e += matchlen;
				} else {
					if (!contiguous)
						sb_append_char(psb, *e);
					e++;
				}
			}
		}
	}
	phandle->sol = e;

	return TRUE;
}

// ----------------------------------------------------------------
static lrec_t* paste_indices_and_data(lrec_reader_mmap_csv_state_t* pstate, rslls_t* pdata_fields, context_t* pctx) {
	int idx = 0;
	lrec_t* prec = lrec_unbacked_alloc();
	for (rsllse_t* pd = pdata_fields->phead; idx < pdata_fields->length && pd != NULL; pd = pd->pnext) {
		idx++;
		char free_flags = pd->free_flag;
		char* key = make_nidx_key(idx, &free_flags);
		// Transfer pointer-free responsibility from the rslls to the lrec object
		lrec_put(prec, key, pd->value, free_flags);
		pd->free_flag = 0;
	}
	return prec;
}

// ----------------------------------------------------------------
static lrec_t* paste_header_and_data(lrec_reader_mmap_csv_state_t* pstate, rslls_t* pdata_fields, context_t* pctx) {
	if (pstate->pheader_keeper->pkeys->length != pdata_fields->length) {
		fprintf(stderr, "%s: Header/data length mismatch (%d != %d) at file \"%s\" line %lld.\n",
			MLR_GLOBALS.argv0, pstate->pheader_keeper->pkeys->length, pdata_fields->length,
			pctx->filename, pstate->ilno);
		exit(1);
	}
	lrec_t* prec = lrec_unbacked_alloc();
	sllse_t* ph  = pstate->pheader_keeper->pkeys->phead;
	rsllse_t* pd = pdata_fields->phead;
	for ( ; ph != NULL && pd != NULL; ph = ph->pnext, pd = pd->pnext) {
		// Transfer pointer-free responsibility from the rslls to the lrec object
		lrec_put(prec, ph->value, pd->value, pd->free_flag);
		pd->free_flag = 0;
	}
	return prec;
}
