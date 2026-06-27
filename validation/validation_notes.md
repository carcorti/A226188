# Validation Notes

The package records a finite computational certification of OEIS A226188 for
`1 <= n <= 67`.

Public repository: <https://github.com/carcorti/A226188>.
Archived DOI: <https://doi.org/10.5281/zenodo.20960659>.

The mathematical predicate certified for each row is

```text
H_{k-1} <= 2n/3 < H_k.
```

The implementation uses an asymptotic seed only as a locator, then expands a
bracket until it has a certified false endpoint and a certified true endpoint.
It then performs binary search inside that bracket. Final certification uses
directed MPFR interval arithmetic: direct summation for small `k`, an
Euler-Maclaurin interval with controlled remainder for larger `k`, and an EM6
checker pass at higher precision.

The canonical b-file for publication is `data/b226188.txt`. The file
`outputs/b226188_candidate.txt` is the generated b-file candidate emitted by
the program and referenced by the generated checksum record; it is not a
second canonical b-file.

Raw terminal logs are not part of this package. The archival telemetry is the
structured manifest in `outputs/a226188_manifest.json`, the tabular summary in
`validation/run_manifest.tsv`, and the per-row certificates in
`outputs/a226188_validation.tsv`.

The local telemetry in the manuscript reports a wall time of about `0.03` s
and peak RSS of about `3184` KB. These are local measurements, not
hardware-normalized benchmarks.

No claim is made beyond the certified `uint64_t` domain. The next index,
`n = 68`, requires a wider integer type.
