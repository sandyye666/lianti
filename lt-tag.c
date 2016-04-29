#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "kvec.h"

/********************
 * Global variables *
 ********************/

const char *lt_bind     = "GGGAGATGTGTATAAGAGACAG"; // including the leading GGG
const char *lt_bind_rev = "CTGTCTCTTATACACATCT"; // excluding the reverse of GGG
const char *lt_promoter = "GAACAGAATTTAATACGACTCACTATA"; // T7 promoter sequence
const char *lt_adapter  = "AGATCGGAAGAGCACACGTCTGAACTCCAGTCAC"; // Illumina 3'-end adapter

enum lt_type_e {
	LT_UNKNOWN = 0,
	LT_AMBI_BASE = 1,
	LT_NO_BINDING = 2,
	LT_TOO_MANY_BINDING = 3,
	LT_POST_PROMOTER = 4,
	LT_MULTI_MERGE = 11,
	LT_NO_MERGE = 12,
	LT_MERGED = 21,
	LT_REST = 99
};

typedef struct {
} lt_opt_t;

/******************
 * K-mer matching *
 ******************/

#include "khash.h"
KHASH_SET_INIT_INT64(s64)
typedef khash_t(s64) lt_seqcloud1_t;

typedef struct {
	int l;
	uint64_t s;
	lt_seqcloud1_t *mm;
} lt_seqcloud_t;

unsigned char seq_nt4_table[256] = {
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4, 
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4, 
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4 /*'-'*/, 4, 4,
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4, 
	4, 0, 4, 1,  4, 4, 4, 2,  4, 4, 4, 4,  4, 4, 4, 4, 
	4, 4, 4, 4,  3, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4, 
	4, 0, 4, 1,  4, 4, 4, 2,  4, 4, 4, 4,  4, 4, 4, 4, 
	4, 4, 4, 4,  3, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4, 
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4, 
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4, 
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4, 
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4, 
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4, 
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4, 
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4, 
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4
};

static lt_seqcloud_t *lt_sc_init(void)
{
	lt_seqcloud_t *sc;
	sc = (lt_seqcloud_t*)calloc(1, sizeof(lt_seqcloud_t));
	sc->mm = kh_init(s64);
	return sc;
}

void lt_sc_destroy(lt_seqcloud_t *sc)
{
	kh_destroy(s64, sc->mm);
	free(sc);
}

void lt_sc_add_core(lt_seqcloud_t *sc, uint64_t s)
{
	int i, absent;
	sc->s = s = s & ((1ULL<<sc->l*2) - 1);
	for (i = 0; i < sc->l; ++i) {
		int i2 = i * 2, a, c = s>>i2&3;
		for (a = 1; a < 4; ++a) {
			uint64_t x = (s & ~(3ULL << i2)) | (uint64_t)((a+c)&3) << i2;
			kh_put(s64, sc->mm, x, &absent);
		}
	}
}

lt_seqcloud_t *lt_sc_gen(const char *s)
{
	lt_seqcloud_t *sc;
	uint64_t x = 0;
	int i;
	sc = lt_sc_init();
	sc->l = strlen(s);
	for (i = 0; s[i] && i < sc->l; ++i) {
		int c = seq_nt4_table[(uint8_t)s[i]];
		if (c > 3) {
			lt_sc_destroy(sc);
			return 0;
		}
		x = x << 2 | c;
	}
	lt_sc_add_core(sc, x);
	return sc;
}

typedef struct {
	uint32_t pos:30, type:2;
} lt_sc_hit_t;

int lt_sc_test(const lt_seqcloud_t *sc, const char *seq, int max_hits, lt_sc_hit_t *hits)
{
	int i, l, n = 0;
	uint64_t x = 0, mask = (1ULL << sc->l*2) - 1;
	for (i = l = 0; seq[i]; ++i) {
		int c = seq_nt4_table[(uint8_t)seq[i]];
		if (c < 4) {
			x = (x << 2 | c) & mask;
			if (++l >= sc->l) {
				if (x == sc->s || kh_get(s64, sc->mm, x) != kh_end(sc->mm)) {
					hits[n].pos = i - (sc->l - 1);
					hits[n++].type = x == sc->s? 0 : 1;
					if (n == max_hits) return n;
				}
			}
		} else l = 0, x = 0;
	}
	return n;
}

/**********************
 * Reverse complement *
 **********************/

char comp_tab[] = {
	  0,   1,	2,	 3,	  4,   5,	6,	 7,	  8,   9,  10,	11,	 12,  13,  14,	15,
	 16,  17,  18,	19,	 20,  21,  22,	23,	 24,  25,  26,	27,	 28,  29,  30,	31,
	 32,  33,  34,	35,	 36,  37,  38,	39,	 40,  41,  42,	43,	 44,  45,  46,	47,
	 48,  49,  50,	51,	 52,  53,  54,	55,	 56,  57,  58,	59,	 60,  61,  62,	63,
	 64, 'T', 'V', 'G', 'H', 'E', 'F', 'C', 'D', 'I', 'J', 'M', 'L', 'K', 'N', 'O',
	'P', 'Q', 'Y', 'S', 'A', 'A', 'B', 'W', 'X', 'R', 'Z',	91,	 92,  93,  94,	95,
	 64, 't', 'v', 'g', 'h', 'e', 'f', 'c', 'd', 'i', 'j', 'm', 'l', 'k', 'n', 'o',
	'p', 'q', 'y', 's', 'a', 'a', 'b', 'w', 'x', 'r', 'z', 123, 124, 125, 126, 127
};

void lt_seq_rev(int l, const char *f, char *r)
{
	int i;
	for (i = 0; i < l; ++i)
		r[l - i - 1] = f[i];
	r[l] = 0;
}

void lt_seq_revcomp(int l, const char *f, char *r)
{
	int i;
	for (i = 0; i < l; ++i)
		r[l - i - 1] = (uint8_t)f[i] >= 128? 'N' : comp_tab[(uint8_t)f[i]];
	r[l] = 0;
}

/**********************
 * Ungapped extension *
 **********************/

#define LT_QUAL_THRES 53 // =33+20
#define LT_HIGH_PEN 3
#define LT_LOW_PEN  1

int lt_ue_for1(const char *s1, const char *q1, const char *s2, const char *q2, int max_pen)
{
	int i, pen = 0;
	for (i = 0; s1[i] && s2[i]; ++i) {
		if (s1[i] != s2[i]) {
			pen += q1[i] >= LT_QUAL_THRES && q2[i] >= LT_QUAL_THRES? LT_HIGH_PEN : LT_LOW_PEN;
			if (pen > max_pen) break;
		}
	}
	return i;
}

int lt_ue_rev1(int l1, const char *s1, const char *q1, int l2, const char *s2, const char *q2, int max_pen)
{
	int i, pen = 0;
	for (i = 0; i < l1 && i < l2; ++i) {
		if (s1[l1-1-i] != s2[l2-1-i]) {
			pen += q1[l1-1-i] >= LT_QUAL_THRES && q2[l2-1-i] >= LT_QUAL_THRES? LT_HIGH_PEN : LT_LOW_PEN;
			if (pen > max_pen) break;
		}
	}
	return i;
}

int lt_ue_for(int l1, const char *s1, const char *q1, int l2, const char *s2, const char *q2, int max_pen, int min_len, int max_pos, uint64_t *pos)
{
	int i, n = 0;
	for (i = min_len; i <= l1; ++i) {
		int l;
		l = lt_ue_for1(s1 + l1 - i, q1 + l1 - i, s2, q2, max_pen);
		if (l >= min_len && (l == i || l == l2)) {
			pos[n++] = (uint64_t)(l1 - i) << 32 | l;
			if (n == max_pos) return n;
		}
	}
	return n;
}

int lt_ue_rev(int l1, const char *s1, const char *q1, int l2, const char *s2, const char *q2, int max_pen, int min_len, int max_pos, uint64_t *pos)
{
	int i, n = 0;
	for (i = min_len; i <= l1; ++i) {
		int l;
		l = lt_ue_rev1(i, s1, q1, l2, s2, q2, max_pen);
		if (l >= min_len && (l == i || l == l2)) {
			pos[n++] = (uint64_t)i << 32 | l;
			if (n == max_pos) return n;
		}
	}
	return n;
}

int lt_ue_contained(int l1, const char *s1, const char *q1, int l2, const char *s2, const char *q2, int max_pen, int max_pos, uint64_t *pos)
{
	int i, n = 0;
	for (i = 1; i < l2 - l1; ++i) {
		int l;
		l = lt_ue_for1(s1, q1, s2 + i, q2 + i, max_pen);
		if (l == l1) {
			pos[n++] = (uint64_t)i << 32 | l;
			if (n == max_pos) return n;
		}
	}
	return n;
}

/*************
 * FASTQ I/O *
 *************/

#include <zlib.h>
#include "kseq.h"
KSEQ_INIT(gzFile, gzread)

typedef struct {
	int l_seq;
	enum lt_type_e type;
	char *name, *seq, *qual;
} bseq1_t;

bseq1_t *bseq_read(kseq_t *ks, int chunk_size, int *n_)
{
	int size = 0, m, n;
	bseq1_t *seqs;
	m = n = 0; seqs = 0;
	while (kseq_read(ks) >= 0) {
		bseq1_t *s;
		if (n >= m) {
			m = m? m<<1 : 256;
			seqs = realloc(seqs, m * sizeof(bseq1_t));
		}
		s = &seqs[n];
		s->name = strdup(ks->name.s);
		s->seq = strdup(ks->seq.s);
		s->qual = ks->qual.l? strdup(ks->qual.s) : 0;
		s->l_seq = ks->seq.l;
		size += seqs[n++].l_seq;
		if (size >= chunk_size && (n&1) == 0) break;
	}
	*n_ = n;
	return seqs;
}

/*******************
 *******************/

typedef struct {
	int n_threads;
	int chunk_size;
	lt_seqcloud_t *sc_bind, *sc_prom;
	kseq_t *ks;
} lt_global_t;

void lt_global_init(lt_global_t *g)
{
	memset(g, 0, sizeof(lt_global_t));
	g->n_threads = 1;
	g->chunk_size = 10000000;
}

#define MAX_HITS 3

void lt_process(const lt_global_t *g, bseq1_t s[2], int max_pen, int min_len)
{
	int i, k, n_hits[2], mlen;
	lt_sc_hit_t hits[2][MAX_HITS];
	char *rseq, *rqual;

	mlen = s[0].l_seq > s[1].l_seq? s[0].l_seq : s[1].l_seq;
	rseq = (char*)alloca(mlen + 1);
	rqual = (char*)alloca(mlen + 1);
	s[0].type = s[1].type = LT_UNKNOWN;

	for (k = 0; k < 2; ++k) {
		bseq1_t *sk = &s[k];
		for (i = sk->l_seq - 1; i >= 0; --i) // trim trailing "N"
			if (sk->seq[i] != 'N') break;
		sk->l_seq = i + 1;
		for (i = 0; i < sk->l_seq; ++i) // trim heading "N"
			if (sk->seq[i] != 'N') break;
		memmove(sk->seq, sk->seq + i, sk->l_seq - i);
		sk->seq[sk->l_seq] = 0;
		if (sk->qual) sk->qual[sk->l_seq] = 0;
		for (i = 0; i < sk->l_seq; ++i) // test if there are "N"s in the middle
			if (sk->seq[i] == 'N') break;
		if (i != sk->l_seq) {
			s[0].type = s[1].type = LT_AMBI_BASE;
			return;
		}
		n_hits[k] = lt_sc_test(g->sc_bind, s[k].seq, MAX_HITS, hits[k]);
	}
	if (n_hits[0] + n_hits[1] == 0) {
		s[0].type = s[1].type = LT_NO_BINDING;
	} else if (n_hits[0] == MAX_HITS || n_hits[1] == MAX_HITS) {
		s[0].type = s[1].type = LT_TOO_MANY_BINDING;
	} else if (n_hits[0] == 0 || n_hits[1] == 0) {
		int f, r, fpos, n_fh, n_rh, n_ch, bpos;
		uint64_t fh[2], rh[2], ch[2];
		lt_sc_hit_t hits_prom;

		f = n_hits[0]? 0 : 1;
		r = f^1;
		bpos = hits[f][n_hits[f] - 1].pos;
		fpos = bpos + g->sc_bind->l;
		if (lt_sc_test(g->sc_prom, &s[f].seq[fpos], 1, &hits_prom) > 0) {
			s[0].type = s[1].type = LT_POST_PROMOTER;
		} else {
			lt_seq_revcomp(s[r].l_seq, s[r].seq, rseq);
			lt_seq_rev(s[r].l_seq, s[r].qual, rqual);
			n_fh = lt_ue_for(s[f].l_seq - bpos, &s[f].seq[bpos], &s[f].qual[bpos], s[r].l_seq, rseq, rqual, max_pen, min_len, 2, fh);
			n_rh = lt_ue_rev(s[f].l_seq - bpos, &s[f].seq[bpos], &s[f].qual[bpos], s[r].l_seq, rseq, rqual, max_pen, min_len, 2, rh);
			n_ch = lt_ue_contained(s[f].l_seq - bpos, &s[f].seq[bpos], &s[f].qual[bpos], s[r].l_seq, rseq, rqual, max_pen, 2, ch);
//			fprintf(stderr, "%d\t%d\t%d\n%s\n%s\n", n_fh, n_rh, n_ch, &s[f].seq[bpos], rseq);
			if (n_fh + n_rh + n_ch > 1) {
				s[0].type = s[1].type = LT_MULTI_MERGE;
			} else if (n_fh + n_rh + n_ch == 0) {
				s[0].type = s[1].type = LT_NO_MERGE;
			} else {
				s[0].type = s[1].type = LT_MERGED;
			}
		}
	} else {
		s[0].type = s[1].type = LT_REST;
	}
}

/**********************
 * Callback functions *
 **********************/

void kt_for(int n_threads, void (*func)(void*,long,int), void *data, long n);
void kt_pipeline(int n_threads, void *(*func)(void*, int, void*), void *shared_data, int n_steps);

typedef struct {
	int n_seqs;
	bseq1_t *seqs;
	lt_global_t *g;
} data_for_t;

static void worker_for(void *_data, long i, int tid)
{
	data_for_t *data = (data_for_t*)_data;
	lt_process(data->g, &data->seqs[i<<1], 4, 8);
}

static void *worker_pipeline(void *shared, int step, void *_data)
{
	int i;
	lt_global_t *g = (lt_global_t*)shared;
	if (step == 0) {
		data_for_t *ret;
		ret = calloc(1, sizeof(data_for_t));
		ret->seqs = bseq_read(g->ks, g->chunk_size, &ret->n_seqs);
		assert((ret->n_seqs&1) == 0);
		ret->g = g;
		if (ret->seqs) return ret;
		else free(ret);
	} else if (step == 1) {
		data_for_t *data = (data_for_t*)_data;
		kt_for(g->n_threads, worker_for, data, data->n_seqs>>1);
		return data;
	} else if (step == 2) {
		data_for_t *data = (data_for_t*)_data;
		for (i = 0; i < data->n_seqs; i += 2) {
			bseq1_t *s = &data->seqs[i];
			printf("%s\t%d\n", s->name, s->type);
			/*
			putchar(s->qual? '@' : '>'); puts(s->name);
			puts(s->seq);
			if (s->qual) {
				puts("+"); puts(s->qual);
			}
			*/
		}
		for (i = 0; i < data->n_seqs; ++i) {
			bseq1_t *s = &data->seqs[i];
			free(s->seq); free(s->qual); free(s->name);
		}
		free(data->seqs); free(data);
	}
	return 0;
}

#include <unistd.h>

int main(int argc, char *argv[])
{
	int c;
	lt_global_t g;
	gzFile fp;

	lt_global_init(&g);
	while ((c = getopt(argc, argv, "t:")) >= 0) {
		if (c == 't') g.n_threads = atoi(optarg);
	}
	if (argc - optind < 1) {
		fprintf(stderr, "Usage: lt-tag [options] <interleaved.fq>\n");
		return 1;
	}

	fp = strcmp(argv[optind], "-")? gzopen(argv[optind], "r") : gzdopen(fileno(stdin), "r");
	g.ks = kseq_init(fp);
	g.sc_bind = lt_sc_gen(lt_bind);
	g.sc_prom = lt_sc_gen(lt_promoter);

	kt_pipeline(2, worker_pipeline, &g, 3);

	lt_sc_destroy(g.sc_prom);
	lt_sc_destroy(g.sc_bind);
	kseq_destroy(g.ks);
	gzclose(fp);
	return 0;
}