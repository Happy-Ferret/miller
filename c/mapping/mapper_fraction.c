#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "lib/mlrutil.h"
#include "containers/sllv.h"
#include "containers/slls.h"
#include "containers/lhmslv.h"
#include "containers/lhmsmv.h"
#include "containers/mixutil.h"
#include "containers/mvfuncs.h"
#include "mapping/mappers.h"
#include "cli/argparse.h"

typedef struct _mapper_fraction_state_t {
	ap_state_t* pargp;
	slls_t* pfraction_field_names;
	slls_t* pgroup_by_field_names;
	sllv_t* precords;
	// Two-level map: lhmslv_t -> lhmsv. Group-by field names are the first keyset;
	// the fraction field names are keys into the second.
	lhmslv_t* psums;
	mv_t zero;
} mapper_fraction_state_t;

static void      mapper_fraction_usage(FILE* o, char* argv0, char* verb);
static mapper_t* mapper_fraction_parse_cli(int* pargi, int argc, char** argv,
	cli_reader_opts_t* _, cli_writer_opts_t* __);
static mapper_t* mapper_fraction_alloc(ap_state_t* pargp, slls_t* pfraction_field_names, slls_t* pgroup_by_field_names);
static void      mapper_fraction_free(mapper_t* pmapper, context_t* _);
static sllv_t*   mapper_fraction_process(lrec_t* pinrec, context_t* pctx, void* pvstate);

// ----------------------------------------------------------------
mapper_setup_t mapper_fraction_setup = {
	.verb = "fraction",
	.pusage_func = mapper_fraction_usage,
	.pparse_func = mapper_fraction_parse_cli,
	.ignores_input = FALSE,
};

// ----------------------------------------------------------------
static void mapper_fraction_usage(FILE* o, char* argv0, char* verb) {
	fprintf(o, "Usage: %s %s [options]\n", argv0, verb);
	fprintf(o, "For each each record's value in specified fields, computes the ratio\n");
	fprintf(o, "of that value to the sum of values in all input records.\n");
	fprintf(o, "E.g. with input records x=1  x=2  x=3  and x=4, emits output records\n");
	fprintf(o, "x=1,x_percent=.1  x=2,x_percent=.2  x=3,x_percent=.3  and x=4,x_percent=.4\n");
	fprintf(o, "\n");
	fprintf(o, "Note: this is internally a two-pass algorithm: on the first pass it retains\n");
	fprintf(o, "input records and accumulates sums; on the second pass it computes fractions.\n");
	fprintf(o, "This means it produces no output until all input is read.\n");
	fprintf(o, "\n");
	fprintf(o, "Options:\n");
	fprintf(o, "-f {a,b,c}    Field names for fraction calculation\n");
	fprintf(o, "-g {d,e,f}    Optional group-by-field names for fraction counts\n");
}

static mapper_t* mapper_fraction_parse_cli(int* pargi, int argc, char** argv,
	cli_reader_opts_t* _, cli_writer_opts_t* __)
{
	slls_t* pfraction_field_names = slls_alloc();
	slls_t* pgroup_by_field_names = slls_alloc();

	char* verb = argv[(*pargi)++];

	ap_state_t* pstate = ap_alloc();
	ap_define_string_list_flag(pstate, "-f", &pfraction_field_names);
	ap_define_string_list_flag(pstate, "-g", &pgroup_by_field_names);

	if (!ap_parse(pstate, verb, pargi, argc, argv)) {
		mapper_fraction_usage(stderr, argv[0], verb);
		return NULL;
	}

	if (pfraction_field_names->length == 0) {
		mapper_fraction_usage(stderr, argv[0], verb);
		return NULL;
	}

	return mapper_fraction_alloc(pstate, pfraction_field_names, pgroup_by_field_names);
}

// ----------------------------------------------------------------
static mapper_t* mapper_fraction_alloc(ap_state_t* pargp, slls_t* pfraction_field_names, slls_t* pgroup_by_field_names) {
	mapper_t* pmapper = mlr_malloc_or_die(sizeof(mapper_t));

	mapper_fraction_state_t* pstate = mlr_malloc_or_die(sizeof(mapper_fraction_state_t));

	pstate->pargp                  = pargp;
	pstate->pfraction_field_names   = pfraction_field_names;
	pstate->pgroup_by_field_names  = pgroup_by_field_names;
	pstate->precords               = sllv_alloc();
	pstate->psums                  = lhmslv_alloc();
	pstate->zero                   = mv_from_int(0);

	pmapper->pvstate       = pstate;
	pmapper->pprocess_func = mapper_fraction_process;
	pmapper->pfree_func    = mapper_fraction_free;

	return pmapper;
}

static void mapper_fraction_free(mapper_t* pmapper, context_t* _) {
	mapper_fraction_state_t* pstate = pmapper->pvstate;
	if (pstate->pfraction_field_names != NULL)
		slls_free(pstate->pfraction_field_names);
	if (pstate->pgroup_by_field_names != NULL)
		slls_free(pstate->pgroup_by_field_names);

	// The process method will have emptied out the list. We just need to free the container.
	sllv_free(pstate->precords);

	// lhmslv_free will free the hashmap keys; we need to free the void-star hashmap values.
	for (lhmslve_t* pa = pstate->psums->phead; pa != NULL; pa = pa->pnext) {
		lhmsmv_t* psums_for_group = pa->pvvalue;
		lhmsmv_free(psums_for_group);
	}
	lhmslv_free(pstate->psums);

	ap_free(pstate->pargp);
	free(pstate);
	free(pmapper);
}

// ----------------------------------------------------------------
static sllv_t* mapper_fraction_process(lrec_t* pinrec, context_t* pctx, void* pvstate) {
	mapper_fraction_state_t* pstate = pvstate;
	if (pinrec != NULL) { // Not end of stream; pass 1

		// Append records into a single output list (so that this verb is order-preserving).
		sllv_append(pstate->precords, pinrec);

		// Accumulate sums of fraction-field values grouped by group-by field names
		slls_t* pgroup_by_field_values = mlr_reference_selected_values_from_record(pinrec,
			pstate->pgroup_by_field_names);

		if (pgroup_by_field_values != NULL) {
			lhmsmv_t* psums_for_group = lhmslv_get(pstate->psums, pgroup_by_field_values);
			if (psums_for_group == NULL) {
				psums_for_group = lhmsmv_alloc();
				lhmslv_put(pstate->psums, slls_copy(pgroup_by_field_values),
					psums_for_group, FREE_ENTRY_KEY);
			}

			for (sllse_t* pf = pstate->pfraction_field_names->phead; pf != NULL; pf = pf->pnext) {
				char* fraction_field_name = pf->value;
				char* lrec_string_value = lrec_get(pinrec, fraction_field_name);
				if (lrec_string_value != NULL) {
					mv_t lrec_num_value = mv_scan_number_or_die(lrec_string_value);
					mv_t* psum = lhmsmv_get(psums_for_group, fraction_field_name);
					if (psum == NULL) { // First value for group
						lhmsmv_put(psums_for_group, fraction_field_name, &lrec_num_value, FREE_ENTRY_VALUE);
					} else {
						*psum = x_xx_plus_func(psum, &lrec_num_value);
					}
				}
			}

			slls_free(pgroup_by_field_values);
		}

		return NULL;

	} else { // End of stream; pass 2
		sllv_t* poutrecs = sllv_alloc();

		// Iterate over the retained records, decorating them with fraction fields.
		while (pstate->precords->phead != NULL) {
			lrec_t* poutrec = sllv_pop(pstate->precords);

			slls_t* pgroup_by_field_values = mlr_reference_selected_values_from_record(poutrec,
				pstate->pgroup_by_field_names);
			if (pgroup_by_field_values != NULL) {
				lhmsmv_t* psums_for_group = lhmslv_get(pstate->psums, pgroup_by_field_values);
				MLR_INTERNAL_CODING_ERROR_IF(psums_for_group == NULL); // should have populated on pass 1
				for (sllse_t* pf = pstate->pfraction_field_names->phead; pf != NULL; pf = pf->pnext) {
					char* fraction_field_name = pf->value;
					char* lrec_string_value = lrec_get(poutrec, fraction_field_name);
					if (lrec_string_value != NULL) {
						mv_t lrec_num_value = mv_scan_number_or_die(lrec_string_value);

						char* output_field_name = mlr_paste_2_strings(fraction_field_name, "_fraction");
						mv_t* psum = lhmsmv_get(psums_for_group, fraction_field_name);
						mv_t fraction;
						if (mv_i_nn_ne(&lrec_num_value, &pstate->zero)) {
							fraction = x_xx_divide_func(&lrec_num_value, psum);
						} else {
							fraction = mv_error();
						}
						char* output_value = mv_alloc_format_val(&fraction);
						lrec_put(poutrec, output_field_name, output_value, FREE_ENTRY_KEY|FREE_ENTRY_VALUE);
					}
				}
				slls_free(pgroup_by_field_values);
			}

			sllv_append(poutrecs, poutrec);
		}

		sllv_append(poutrecs, NULL);
		return poutrecs;
	}
}