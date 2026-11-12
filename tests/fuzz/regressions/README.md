# Fuzz regression inputs

Inputs that once made a fuzz target stop. `tools/dev/native-checks.sh` runs
each of them through its target before fuzzing.

| File | Target | History |
|---|---|---|
| `record_stream/m1-invariant-too-strict` | `rwsrt_fuzz_record_stream` | Found in M1. The code was right; an invariant in the fuzz target was too strict and was corrected. Kept so that the corrected invariant stays exercised |
