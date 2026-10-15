Vendored quirc QR decoder from https://github.com/dlbeer/quirc
Commit: 927d680904dc95fdff4cd9d022eb374b438ff8f2

The core library and ISC license are included. VitaGram adds an opt-in
`quirc_set_rounded_finders()` retry: fit and intersect the straight outer sides
of rounded finder rings instead of treating their arc pixels as QR corners.
The ordinary square-finder path remains the default. The fit uses bounded
central-strip sampling and two refinement passes, with affine neighbour grouping
to avoid extrapolating pixel rounding errors from a small ring. It does not
change decoding or Reed-Solomon correction. Regression coverage is in `tests/run-proxy-tests.sh`.

Built locally by the root CMakeLists.txt; no download or camera library is needed at build time.
