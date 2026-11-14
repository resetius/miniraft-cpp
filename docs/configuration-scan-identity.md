# Compare full configuration state in history scans: v0.1.0-alpha.53

`TConfigurationScan::HasUncommittedConfiguration()` previously compared only
the configuration index and term. Two independently supplied histories with the
same identity but different voter sets or joint/stable phase could therefore be
reported as having no configuration difference.

The result now compares the full effective configuration: phase, old and new
voter sets, configuration index, and configuration term. Different scan
boundaries alone do not count as a configuration change. Callers still supply
and substantiate the baseline and commit boundary; this helper does not prove
commitment or activate membership in Raft.

Tests cover conflicting stable voter sets and a stable-versus-joint mismatch
with equal configuration index/term, plus the unchanged case.

Self-review completed; no independent reviewer approval is claimed.
