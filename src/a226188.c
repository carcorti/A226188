#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <mpfr.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SEQ_ID "A226188"
#define KNOWN_COUNT 27U
#define GENERATE_MAX_N 67U
#define DIRECT_SUM_LIMIT 10000ULL
#define PRECISION_COUNT 3U
#define CHECKSUM_PATH "outputs/a226188_checksums.sha256"
#define PRECISION_BITS_0 256
#define PRECISION_BITS_1 384
#define PRECISION_BITS_2 512

_Static_assert(GENERATE_MAX_N <= 67U,
               "A226188 uint64_t implementation is certified only through n=67");
_Static_assert(DIRECT_SUM_LIMIT <= 10000ULL,
               "Direct-sum loop audit assumes DIRECT_SUM_LIMIT <= 10000");
_Static_assert(PRECISION_COUNT == 3U,
               "Manifest precision-policy serialization assumes three tiers");
_Static_assert(PRECISION_BITS_0 >= 64,
               "MPFR precision must exactly represent every uint64_t k");
_Static_assert(UINTMAX_MAX >= UINT64_MAX,
               "mpfr_set_uj conversion path must cover uint64_t");
_Static_assert(ULONG_MAX >= (2UL * (unsigned long)GENERATE_MAX_N),
               "threshold scaling uses unsigned long and must cover 2*n");

static const uint64_t known_terms[KNOWN_COUNT + 1U] = {
    0ULL,
    1ULL, 2ULL, 4ULL, 8ULL, 16ULL, 31ULL, 60ULL, 116ULL, 227ULL,
    441ULL, 859ULL, 1674ULL, 3260ULL, 6349ULL, 12367ULL, 24088ULL,
    46916ULL, 91380ULL, 177984ULL, 346666ULL, 675214ULL, 1315136ULL,
    2561536ULL, 4989191ULL, 9717617ULL, 18927334ULL, 36865412ULL
};

static const mpfr_prec_t precision_policy[PRECISION_COUNT] = {
    PRECISION_BITS_0, PRECISION_BITS_1, PRECISION_BITS_2
};

typedef enum {
    PRED_FALSE = 0,
    PRED_TRUE = 1,
    PRED_UNRESOLVED = 2
} Predicate;

typedef struct {
    int sign;
    uint32_t numerator;
    uint32_t denominator;
    uint32_t power;
} EmTerm;

typedef struct {
    uint32_t numerator;
    uint32_t denominator;
    uint32_t power;
} RemainderBound;

static const EmTerm em_terms[] = {
    {-1, 1U, 12U, 2U},
    { 1, 1U, 120U, 4U},
    {-1, 1U, 252U, 6U},
    { 1, 1U, 240U, 8U},
    {-1, 1U, 132U, 10U},
    { 1, 691U, 32760U, 12U}
};

typedef struct {
    uint32_t n;
    uint64_t k;
    uint64_t seed_k0;
    uint64_t bracket_low_false;
    uint64_t bracket_high_true;
    mpfr_prec_t precision_bits;
    unsigned int bracket_expansions;
    unsigned int predicate_evaluations;
    bool known_reference;
    bool checker_ok;
    double elapsed_ms;
    char margin_prev_ge[96];
    char margin_k_gt[96];
    char harmonic_method[32];
} TermResult;

static double monotonic_seconds(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0.0;
    }
    return (double)ts.tv_sec + ((double)ts.tv_nsec / 1000000000.0);
}

static void log_event(const char *event, uint32_t n, const char *status) {
    fprintf(stderr, "seq_id=%s event=%s n=%" PRIu32 " status=%s\n",
            SEQ_ID, event, n, status);
}

static bool ensure_outputs_dir(void) {
    if (mkdir("outputs", 0775) == 0) {
        return true;
    }
    if (errno != EEXIST) {
        return false;
    }
    struct stat st;
    if (stat("outputs", &st) != 0) {
        return false;
    }
    return S_ISDIR(st.st_mode);
}

static bool make_temp_output_path(const char *final_path,
                                  char *tmp_path,
                                  size_t tmp_path_size) {
    const char *slash = strrchr(final_path, '/');
    if (slash == NULL) {
        return false;
    }
    const int dir_len = (int)(slash - final_path);
    const char *base = slash + 1;
    const int written = snprintf(tmp_path, tmp_path_size, "%.*s/.%s.tmp.XXXXXX",
                                 dir_len, final_path, base);
    return written > 0 && (size_t)written < tmp_path_size;
}

static bool fsync_outputs_dir(void) {
    int fd = open("outputs", O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        return false;
    }
    const bool ok = fsync(fd) == 0;
    if (close(fd) != 0) {
        return false;
    }
    return ok;
}

static FILE *open_atomic_output(const char *final_path,
                                char *tmp_path,
                                size_t tmp_path_size) {
    if (!make_temp_output_path(final_path, tmp_path, tmp_path_size)) {
        return NULL;
    }
    int fd = mkstemp(tmp_path);
    if (fd < 0) {
        tmp_path[0] = '\0';
        return NULL;
    }
    if (fchmod(fd, 0644) != 0) {
        close(fd);
        remove(tmp_path);
        tmp_path[0] = '\0';
        return NULL;
    }
    FILE *fp = fdopen(fd, "w");
    if (fp == NULL) {
        close(fd);
        remove(tmp_path);
        tmp_path[0] = '\0';
        return NULL;
    }
    return fp;
}

static bool abort_atomic_output(FILE *fp, const char *tmp_path) {
    if (fp != NULL) {
        fclose(fp);
    }
    if (tmp_path != NULL && tmp_path[0] != '\0') {
        remove(tmp_path);
    }
    return false;
}

static bool commit_atomic_output(FILE *fp,
                                 const char *tmp_path,
                                 const char *final_path) {
    if (fflush(fp) != 0) {
        return abort_atomic_output(fp, tmp_path);
    }
    if (fsync(fileno(fp)) != 0) {
        return abort_atomic_output(fp, tmp_path);
    }
    if (fclose(fp) != 0) {
        remove(tmp_path);
        return false;
    }
    if (rename(tmp_path, final_path) != 0) {
        remove(tmp_path);
        return false;
    }
    return fsync_outputs_dir();
}

static bool checked_add_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (UINT64_MAX - a < b) {
        return false;
    }
    *out = a + b;
    return true;
}

static uint64_t seed_for_n(uint32_t n) {
    static const long double euler_gamma =
        0.57721566490153286060651209008240243104215933593992L;
    const long double x = ((2.0L * (long double)n) / 3.0L) - euler_gamma;
    const long double y = expl(x);
    if (!isfinite(y) || y < 1.0L) {
        return 1ULL;
    }
    if (y > (long double)UINT64_MAX) {
        return UINT64_MAX;
    }
    return (uint64_t)floorl(y);
}

static void positive_rational_power(mpfr_t out,
                                    uint32_t numerator,
                                    uint32_t denominator,
                                    uint32_t power,
                                    uint64_t k,
                                    mpfr_rnd_t rnd) {
    mpfr_t kv;
    mpfr_init2(kv, mpfr_get_prec(out));
    mpfr_set_ui(out, numerator, rnd);
    mpfr_div_ui(out, out, denominator, rnd);
    mpfr_set_uj(kv, (uintmax_t)k, rnd);
    for (uint32_t i = 0; i < power; i++) {
        mpfr_div(out, out, kv, rnd);
    }
    mpfr_clear(kv);
}

static RemainderBound remainder_for_term_count(unsigned int term_count) {
    if (term_count == 4U) {
        const RemainderBound r = {1U, 132U, 10U};
        return r;
    }
    if (term_count == 6U) {
        const RemainderBound r = {1U, 12U, 14U};
        return r;
    }
    const RemainderBound r = {0U, 1U, 0U};
    return r;
}

static void harmonic_interval_direct(uint64_t k, mpfr_t lower, mpfr_t upper) {
    mpfr_t term;
    mpfr_t denom;
    mpfr_inits2(mpfr_get_prec(lower), term, denom, (mpfr_ptr)0);
    mpfr_set_zero(lower, 1);
    mpfr_set_zero(upper, 1);
    for (uint64_t i = 1; i <= k; i++) {
        mpfr_set_ui(denom, (unsigned long)i, MPFR_RNDN);
        mpfr_ui_div(term, 1UL, denom, MPFR_RNDD);
        mpfr_add(lower, lower, term, MPFR_RNDD);
        mpfr_ui_div(term, 1UL, denom, MPFR_RNDU);
        mpfr_add(upper, upper, term, MPFR_RNDU);
    }
    mpfr_clears(term, denom, (mpfr_ptr)0);
}

static bool harmonic_interval_em(uint64_t k,
                                 unsigned int term_count,
                                 mpfr_t lower,
                                 mpfr_t upper) {
    if (term_count > (sizeof(em_terms) / sizeof(em_terms[0]))) {
        return false;
    }

    mpfr_t kv_low;
    mpfr_t kv_high;
    mpfr_t gamma_low;
    mpfr_t gamma_high;
    mpfr_t term;
    mpfr_t rem;
    const mpfr_prec_t prec = mpfr_get_prec(lower);

    mpfr_inits2(prec, kv_low, kv_high, gamma_low, gamma_high, term, rem,
                (mpfr_ptr)0);

    mpfr_set_uj(kv_low, (uintmax_t)k, MPFR_RNDD);
    mpfr_set_uj(kv_high, (uintmax_t)k, MPFR_RNDU);
    mpfr_log(lower, kv_low, MPFR_RNDD);
    mpfr_log(upper, kv_high, MPFR_RNDU);
    mpfr_const_euler(gamma_low, MPFR_RNDD);
    mpfr_const_euler(gamma_high, MPFR_RNDU);
    mpfr_add(lower, lower, gamma_low, MPFR_RNDD);
    mpfr_add(upper, upper, gamma_high, MPFR_RNDU);

    positive_rational_power(term, 1U, 2U, 1U, k, MPFR_RNDD);
    mpfr_add(lower, lower, term, MPFR_RNDD);
    positive_rational_power(term, 1U, 2U, 1U, k, MPFR_RNDU);
    mpfr_add(upper, upper, term, MPFR_RNDU);

    for (unsigned int i = 0; i < term_count; i++) {
        if (em_terms[i].sign > 0) {
            positive_rational_power(term, em_terms[i].numerator,
                                    em_terms[i].denominator,
                                    em_terms[i].power, k, MPFR_RNDD);
            mpfr_add(lower, lower, term, MPFR_RNDD);
            positive_rational_power(term, em_terms[i].numerator,
                                    em_terms[i].denominator,
                                    em_terms[i].power, k, MPFR_RNDU);
            mpfr_add(upper, upper, term, MPFR_RNDU);
        } else {
            positive_rational_power(term, em_terms[i].numerator,
                                    em_terms[i].denominator,
                                    em_terms[i].power, k, MPFR_RNDU);
            mpfr_sub(lower, lower, term, MPFR_RNDD);
            positive_rational_power(term, em_terms[i].numerator,
                                    em_terms[i].denominator,
                                    em_terms[i].power, k, MPFR_RNDD);
            mpfr_sub(upper, upper, term, MPFR_RNDU);
        }
    }

    const RemainderBound rb = remainder_for_term_count(term_count);
    if (rb.numerator == 0U) {
        mpfr_clears(kv_low, kv_high, gamma_low, gamma_high, term, rem,
                    (mpfr_ptr)0);
        return false;
    }
    positive_rational_power(rem, rb.numerator, rb.denominator, rb.power,
                            k, MPFR_RNDU);
    mpfr_sub(lower, lower, rem, MPFR_RNDD);
    mpfr_add(upper, upper, rem, MPFR_RNDU);

    mpfr_clears(kv_low, kv_high, gamma_low, gamma_high, term, rem,
                (mpfr_ptr)0);
    return true;
}

static bool harmonic_interval(uint64_t k,
                              mpfr_prec_t precision,
                              unsigned int term_count,
                              mpfr_t lower,
                              mpfr_t upper,
                              const char **method) {
    mpfr_set_prec(lower, precision);
    mpfr_set_prec(upper, precision);
    if (k == 0ULL) {
        mpfr_set_zero(lower, 1);
        mpfr_set_zero(upper, 1);
        *method = "zero";
        return true;
    }
    if (k <= DIRECT_SUM_LIMIT) {
        harmonic_interval_direct(k, lower, upper);
        *method = "direct-mpfr";
        return true;
    }
    *method = (term_count == 4U) ? "em4-mpfr" : "em6-mpfr";
    return harmonic_interval_em(k, term_count, lower, upper);
}

static Predicate compare_harmonic_gt_threshold(uint32_t n,
                                               uint64_t k,
                                               mpfr_prec_t precision,
                                               unsigned int term_count,
                                               unsigned int *evaluations) {
    mpfr_t lower;
    mpfr_t upper;
    mpfr_t scaled;
    const char *method = NULL;
    Predicate result = PRED_UNRESOLVED;

    if (evaluations != NULL) {
        (*evaluations)++;
    }

    mpfr_inits2(precision, lower, upper, scaled, (mpfr_ptr)0);
    if (!harmonic_interval(k, precision, term_count, lower, upper, &method)) {
        mpfr_clears(lower, upper, scaled, (mpfr_ptr)0);
        return PRED_UNRESOLVED;
    }

    mpfr_mul_ui(scaled, lower, 3UL, MPFR_RNDD);
    if (mpfr_cmp_ui(scaled, 2UL * (unsigned long)n) > 0) {
        result = PRED_TRUE;
    } else {
        mpfr_mul_ui(scaled, upper, 3UL, MPFR_RNDU);
        if (mpfr_cmp_ui(scaled, 2UL * (unsigned long)n) <= 0) {
            result = PRED_FALSE;
        }
    }

    mpfr_clears(lower, upper, scaled, (mpfr_ptr)0);
    return result;
}

static bool final_certificate(uint32_t n,
                              uint64_t k,
                              mpfr_prec_t precision,
                              unsigned int term_count,
                              char *margin_prev,
                              size_t margin_prev_size,
                              char *margin_k,
                              size_t margin_k_size,
                              char *method_out,
                              size_t method_out_size) {
    mpfr_t lower_prev;
    mpfr_t upper_prev;
    mpfr_t lower_k;
    mpfr_t upper_k;
    mpfr_t scaled;
    mpfr_t margin;
    const char *method_prev = NULL;
    const char *method_k = NULL;
    bool ok = false;

    mpfr_inits2(precision, lower_prev, upper_prev, lower_k, upper_k,
                scaled, margin, (mpfr_ptr)0);

    if (!harmonic_interval(k - 1ULL, precision, term_count,
                           lower_prev, upper_prev, &method_prev) ||
        !harmonic_interval(k, precision, term_count,
                           lower_k, upper_k, &method_k)) {
        goto done;
    }

    mpfr_mul_ui(scaled, upper_prev, 3UL, MPFR_RNDU);
    if (mpfr_cmp_ui(scaled, 2UL * (unsigned long)n) > 0) {
        goto done;
    }

    mpfr_mul_ui(scaled, lower_k, 3UL, MPFR_RNDD);
    if (mpfr_cmp_ui(scaled, 2UL * (unsigned long)n) <= 0) {
        goto done;
    }

    if (margin_prev != NULL && margin_prev_size > 0U) {
        mpfr_mul_ui(scaled, upper_prev, 3UL, MPFR_RNDU);
        mpfr_ui_sub(margin, 2UL * (unsigned long)n, scaled, MPFR_RNDD);
        mpfr_div_ui(margin, margin, 3UL, MPFR_RNDD);
        mpfr_snprintf(margin_prev, margin_prev_size, "%.32Re", margin);
    }
    if (margin_k != NULL && margin_k_size > 0U) {
        mpfr_mul_ui(scaled, lower_k, 3UL, MPFR_RNDD);
        mpfr_sub_ui(margin, scaled, 2UL * (unsigned long)n, MPFR_RNDD);
        mpfr_div_ui(margin, margin, 3UL, MPFR_RNDD);
        mpfr_snprintf(margin_k, margin_k_size, "%.32Re", margin);
    }
    if (method_out != NULL && method_out_size > 0U) {
        if (strcmp(method_prev, method_k) == 0) {
            (void)snprintf(method_out, method_out_size, "%s", method_k);
        } else {
            (void)snprintf(method_out, method_out_size, "%s/%s",
                           method_prev, method_k);
        }
    }
    ok = true;

done:
    mpfr_clears(lower_prev, upper_prev, lower_k, upper_k, scaled, margin,
                (mpfr_ptr)0);
    return ok;
}

static int find_term_at_precision(uint32_t n,
                                  mpfr_prec_t precision,
                                  TermResult *result) {
    const uint64_t seed = seed_for_n(n);
    uint64_t radius = 16ULL;
    uint64_t low_false = 0ULL;
    uint64_t high_true = 0ULL;
    unsigned int evaluations = 0U;
    unsigned int expansions = 0U;
    bool bracketed = false;

    while (radius <= (UINT64_MAX / 2ULL)) {
        uint64_t high = 0ULL;
        if (!checked_add_u64(seed, radius, &high)) {
            return -1;
        }
        const uint64_t low_candidate = (seed > radius) ? (seed - radius) : 1ULL;
        const uint64_t low = low_candidate - 1ULL;

        const Predicate low_pred =
            compare_harmonic_gt_threshold(n, low, precision, 4U, &evaluations);
        const Predicate high_pred =
            compare_harmonic_gt_threshold(n, high, precision, 4U, &evaluations);

        if (low_pred == PRED_UNRESOLVED || high_pred == PRED_UNRESOLVED) {
            return 1;
        }
        if (low_pred == PRED_FALSE && high_pred == PRED_TRUE) {
            low_false = low;
            high_true = high;
            bracketed = true;
            break;
        }

        radius *= 2ULL;
        expansions++;
    }

    if (!bracketed) {
        return -1;
    }

    while (high_true - low_false > 1ULL) {
        const uint64_t mid = low_false + ((high_true - low_false) / 2ULL);
        const Predicate pred =
            compare_harmonic_gt_threshold(n, mid, precision, 4U, &evaluations);
        if (pred == PRED_UNRESOLVED) {
            return 1;
        }
        if (pred == PRED_TRUE) {
            high_true = mid;
        } else {
            low_false = mid;
        }
    }

    if (!final_certificate(n, high_true, precision, 4U,
                           result->margin_prev_ge, sizeof(result->margin_prev_ge),
                           result->margin_k_gt, sizeof(result->margin_k_gt),
                           result->harmonic_method,
                           sizeof(result->harmonic_method))) {
        return 1;
    }

    if (!final_certificate(n, high_true, precision + 128U, 6U,
                           NULL, 0U, NULL, 0U, NULL, 0U)) {
        return 1;
    }

    result->n = n;
    result->k = high_true;
    result->seed_k0 = seed;
    result->bracket_low_false = low_false;
    result->bracket_high_true = high_true;
    result->precision_bits = precision;
    result->bracket_expansions = expansions;
    result->predicate_evaluations = evaluations;
    result->known_reference = n <= KNOWN_COUNT;
    result->checker_ok = true;
    return 0;
}

static int find_term(uint32_t n, TermResult *result) {
    memset(result, 0, sizeof(*result));
    const double start = monotonic_seconds();
    for (unsigned int i = 0; i < PRECISION_COUNT; i++) {
        const int status = find_term_at_precision(n, precision_policy[i], result);
        if (status == 0) {
            const double end = monotonic_seconds();
            result->elapsed_ms = (end - start) * 1000.0;
            return 0;
        }
        if (status < 0) {
            return status;
        }
    }
    return 1;
}

static bool compute_known_replay(TermResult *results) {
    for (uint32_t n = 1U; n <= KNOWN_COUNT; n++) {
        log_event("compute", n, "start");
        if (find_term(n, &results[n]) != 0) {
            log_event("compute", n, "unresolved");
            return false;
        }
        if (results[n].k != known_terms[n]) {
            fprintf(stderr,
                    "seq_id=%s event=known_replay n=%" PRIu32
                    " status=fail expected=%" PRIu64 " observed=%" PRIu64 "\n",
                    SEQ_ID, n, known_terms[n], results[n].k);
            return false;
        }
        log_event("known_replay", n, "pass");
    }
    return true;
}

static bool compute_generation(TermResult *results) {
    if (!compute_known_replay(results)) {
        return false;
    }
    for (uint32_t n = KNOWN_COUNT + 1U; n <= GENERATE_MAX_N; n++) {
        log_event("compute", n, "start");
        if (find_term(n, &results[n]) != 0) {
            log_event("compute", n, "unresolved");
            return false;
        }
        log_event("certify", n, "pass");
    }
    return true;
}

static bool write_candidate_bfile(const TermResult *results, uint32_t max_n) {
    const char *final_path = "outputs/b226188_candidate.txt";
    char tmp_path[128];
    FILE *fp = open_atomic_output(final_path, tmp_path, sizeof(tmp_path));
    if (fp == NULL) {
        return false;
    }
    for (uint32_t n = 1U; n <= max_n; n++) {
        if (fprintf(fp, "%" PRIu32 " %" PRIu64 "\n", n, results[n].k) < 0) {
            return abort_atomic_output(fp, tmp_path);
        }
    }
    return commit_atomic_output(fp, tmp_path, final_path);
}

static bool write_terms_tsv(const TermResult *results, uint32_t max_n) {
    const char *final_path = "outputs/a226188_terms.tsv";
    char tmp_path[128];
    FILE *fp = open_atomic_output(final_path, tmp_path, sizeof(tmp_path));
    if (fp == NULL) {
        return false;
    }
    if (fprintf(fp, "seq_id\tn\tk\tknown_reference\tseed_k0\tprecision_bits\telapsed_ms\n") < 0) {
        return abort_atomic_output(fp, tmp_path);
    }
    for (uint32_t n = 1U; n <= max_n; n++) {
        if (fprintf(fp, "%s\t%" PRIu32 "\t%" PRIu64 "\t%s\t%" PRIu64
                    "\t%lu\t%.3f\n",
                    SEQ_ID, n, results[n].k,
                    results[n].known_reference ? "true" : "false",
                    results[n].seed_k0,
                    (unsigned long)results[n].precision_bits,
                    results[n].elapsed_ms) < 0) {
            return abort_atomic_output(fp, tmp_path);
        }
    }
    return commit_atomic_output(fp, tmp_path, final_path);
}

static bool write_validation_tsv(const TermResult *results, uint32_t max_n) {
    const char *final_path = "outputs/a226188_validation.tsv";
    char tmp_path[128];
    FILE *fp = open_atomic_output(final_path, tmp_path, sizeof(tmp_path));
    if (fp == NULL) {
        return false;
    }
    if (fprintf(fp,
                "seq_id\tn\tcandidate_k\tseed_k0\tbracket_low_false\t"
                "bracket_high_true\tmpfr_precision_bits\tharmonic_method\t"
                "upper_H_k_minus_1_cmp_threshold\tlower_H_k_cmp_threshold\t"
                "margin_threshold_minus_upper_H_k_minus_1\t"
                "margin_lower_H_k_minus_threshold\tchecker_status\t"
                "bracket_expansions\tpredicate_evaluations\telapsed_ms\tstatus\n") < 0) {
        return abort_atomic_output(fp, tmp_path);
    }
    for (uint32_t n = 1U; n <= max_n; n++) {
        if (fprintf(fp,
                    "%s\t%" PRIu32 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64
                    "\t%" PRIu64 "\t%lu\t%s\tUPPER_LE_THRESHOLD\t"
                    "LOWER_GT_THRESHOLD\t%s\t%s\t%s\t%u\t%u\t%.3f\tCERTIFIED\n",
                    SEQ_ID, n, results[n].k, results[n].seed_k0,
                    results[n].bracket_low_false, results[n].bracket_high_true,
                    (unsigned long)results[n].precision_bits,
                    results[n].harmonic_method,
                    results[n].margin_prev_ge, results[n].margin_k_gt,
                    results[n].checker_ok ? "PASS" : "FAIL",
                    results[n].bracket_expansions,
                    results[n].predicate_evaluations,
                    results[n].elapsed_ms) < 0) {
            return abort_atomic_output(fp, tmp_path);
        }
    }
    return commit_atomic_output(fp, tmp_path, final_path);
}

static bool write_manifest(const TermResult *results,
                           uint32_t max_n,
                           double elapsed_seconds) {
    const char *final_path = "outputs/a226188_manifest.json";
    char tmp_path[128];
    FILE *fp = open_atomic_output(final_path, tmp_path, sizeof(tmp_path));
    if (fp == NULL) {
        return false;
    }

    mpfr_prec_t max_precision = 0;
    uint64_t max_k = 0ULL;
    unsigned int total_evaluations = 0U;
    for (uint32_t n = 1U; n <= max_n; n++) {
        if (results[n].precision_bits > max_precision) {
            max_precision = results[n].precision_bits;
        }
        if (results[n].k > max_k) {
            max_k = results[n].k;
        }
        total_evaluations += results[n].predicate_evaluations;
    }

    const int rc = fprintf(fp,
        "{\n"
        "  \"seq_id\": \"%s\",\n"
        "  \"source\": \"src/a226188.c\",\n"
        "  \"mode\": \"generate\",\n"
        "  \"status\": \"complete\",\n"
        "  \"definition\": \"least positive integer k such that H_k > 2n/3\",\n"
        "  \"certification_predicate\": \"H_{k-1} <= 2n/3 < H_k\",\n"
        "  \"range\": {\"n_min\": 1, \"n_max\": %" PRIu32 "},\n"
        "  \"known_replay\": {\"n_min\": 1, \"n_max\": %" PRIu32 ", \"status\": \"pass\"},\n"
        "  \"new_terms\": {\"n_min\": %" PRIu32 ", \"n_max\": %" PRIu32 ", \"status\": \"certified\"},\n"
        "  \"max_k\": %" PRIu64 ",\n"
        "  \"mpfr_version\": \"%s\",\n"
        "  \"gmp_version\": \"%s\",\n"
        "  \"precision_policy_bits\": [%u, %u, %u],\n"
        "  \"max_precision_used_bits\": %lu,\n"
        "  \"harmonic_methods\": [\"direct-mpfr\", \"em4-mpfr\", \"em6-mpfr checker\"],\n"
        "  \"direct_sum_limit\": %" PRIu64 ",\n"
        "  \"asymptotic_seed\": \"floor(exp(2n/3 - EulerGamma)); locator only\",\n"
        "  \"search\": \"expanding bracket plus binary search\",\n"
        "  \"uint64_limit\": {\"max_supported_n\": 67, \"n_68_status\": \"requires wider integer type\"},\n"
        "  \"checksum_policy\": \"src/Makefile writes outputs/a226188_checksums.sha256 after generate through mktemp, chmod, sync -f, mv, and sync -f outputs; binary generate invalidates any stale checksum before writing outputs; data artifacts are written through mkstemp, fsync, rename, and directory fsync\",\n"
        "  \"terms_written\": %" PRIu32 ",\n"
        "  \"predicate_evaluations\": %u,\n"
        "  \"elapsed_seconds\": %.6f\n"
        "}\n",
        SEQ_ID, max_n, KNOWN_COUNT, KNOWN_COUNT + 1U, max_n, max_k,
        mpfr_get_version(), gmp_version,
        PRECISION_BITS_0, PRECISION_BITS_1, PRECISION_BITS_2,
        (unsigned long)max_precision,
        (uint64_t)DIRECT_SUM_LIMIT, max_n, total_evaluations,
        elapsed_seconds);

    if (rc < 0) {
        return abort_atomic_output(fp, tmp_path);
    }
    return commit_atomic_output(fp, tmp_path, final_path);
}

static bool write_outputs(const TermResult *results,
                          uint32_t max_n,
                          double elapsed_seconds) {
    return ensure_outputs_dir() &&
           write_candidate_bfile(results, max_n) &&
           write_terms_tsv(results, max_n) &&
           write_validation_tsv(results, max_n) &&
           write_manifest(results, max_n, elapsed_seconds);
}

static int run_verify_known(void) {
    TermResult results[GENERATE_MAX_N + 1U];
    memset(results, 0, sizeof(results));
    const double start = monotonic_seconds();
    const bool ok = compute_known_replay(results);
    const double end = monotonic_seconds();
    if (!ok) {
        fprintf(stderr, "seq_id=%s event=verify_known status=fail\n", SEQ_ID);
        return EXIT_FAILURE;
    }
    printf("seq_id=%s mode=verify-known known_terms=%u status=pass elapsed_seconds=%.6f\n",
           SEQ_ID, KNOWN_COUNT, end - start);
    return EXIT_SUCCESS;
}

static int run_generate(void) {
    TermResult results[GENERATE_MAX_N + 1U];
    memset(results, 0, sizeof(results));
    const double start = monotonic_seconds();

    if (remove(CHECKSUM_PATH) != 0 && errno != ENOENT) {
        fprintf(stderr, "seq_id=%s event=remove_stale_checksum status=fail errno=%d\n",
                SEQ_ID, errno);
        return EXIT_FAILURE;
    }

    if (!compute_generation(results)) {
        fprintf(stderr, "seq_id=%s event=generate status=fail_closed\n", SEQ_ID);
        return EXIT_FAILURE;
    }

    const double end = monotonic_seconds();
    if (!write_outputs(results, GENERATE_MAX_N, end - start)) {
        fprintf(stderr, "seq_id=%s event=write_outputs status=fail errno=%d\n",
                SEQ_ID, errno);
        return EXIT_FAILURE;
    }

    printf("seq_id=%s mode=generate n_max=%u status=complete elapsed_seconds=%.6f\n",
           SEQ_ID, GENERATE_MAX_N, end - start);
    return EXIT_SUCCESS;
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s verify-known\n"
            "       %s generate\n",
            argv0, argv0);
}

int main(int argc, char **argv) {
    int rc = EXIT_FAILURE;
    if (argc != 2) {
        usage(argv[0]);
        mpfr_free_cache();
        return rc;
    }
    if (strcmp(argv[1], "verify-known") == 0) {
        rc = run_verify_known();
    } else if (strcmp(argv[1], "generate") == 0) {
        rc = run_generate();
    } else {
        usage(argv[0]);
    }
    mpfr_free_cache();
    return rc;
}
