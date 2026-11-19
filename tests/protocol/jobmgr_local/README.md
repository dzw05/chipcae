These acceptance tests cover I7-jobs-local/1.0, card 1. They were committed
before implementation and retain their original assertions. The manager now
implements snapshots, profiles, persisted state, recovery and local call semantics;
real process execution and desktop comparison tests remain for card 2.

`test_paths` always runs. The six manager executables return 77 only when the
optional local target is absent. Release assertions use explicit checks, so
`NDEBUG` cannot disable them. FakeSpawner substitutes only process operations;
protocol messages, disk snapshots, journals and CAELOG1 records are real.

Recovery necessarily makes every formerly live job terminal. Consequently new
Control calls after restart must reject; cached accepted replies retain their
original uint64 sequence (including values beyond double precision). There is
no public transition through which a recovered run can accept a next command.

The brief's unconditional malformed label-selector rejection is tested also
on an empty root. This is stricter than desktop's validation inside iteration.
Desktop comparison tests and real process execution belong to card 2.
