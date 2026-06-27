# Validation Summary

Date of package preparation: 2026-06-27.

Scope:

- Sequence: OEIS A226188.
- Definition: least positive integer `k` such that `H_k > 2n/3`.
- Certified range: `1 <= n <= 67`.
- Previously known terms replayed: `1 <= n <= 27`.
- New certified terms: `28 <= n <= 67`.
- Largest certified value: `a(67) = 14054172515346621401`.

Build and reproduction commands:

```bash
make -C src
make -C src verify-known
make -C src generate
make -C src test
```

Production telemetry:

- Manifest: `outputs/a226188_manifest.json`.
- Tabular summary: `validation/run_manifest.tsv`.
- Terms written: 67.
- Predicate evaluations: 467.
- Manifest elapsed time: `0.027685` s.
- Local wall time reported in the manuscript: about `0.03` s.
- Local peak RSS reported in the manuscript: about `3184` KB.
- Maximum MPFR precision used in the completed run: 256 bits.

Data checks:

- `data/b226188.txt` has 67 data rows and one final blank line.
- The generated candidate `outputs/b226188_candidate.txt` has the same 67
  `(n, a(n))` rows as `data/b226188.txt`, differing only by the canonical
  final blank line in `data/b226188.txt`.
- `outputs/a226188_terms.tsv` has 67 data rows plus a header.
- `outputs/a226188_validation.tsv` has 67 data rows plus a header.
- All validation rows have `status = CERTIFIED` and
  `checker_status = PASS`.

Last result row:

```text
67 14054172515346621401
```

Checksums:

- Package checksums are listed in `validation/package_checksums.sha256`.
- Generated-output checksums from the production workflow are listed in
  `outputs/a226188_checksums.sha256`.

Release caveat:

This is a raw GitHub-ready package. Final GitHub release metadata, Zenodo DOI,
and the final manuscript update are intentionally deferred until publication.

