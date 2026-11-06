# str0m-rust-crypto 0.7.0: certificate generation without AWS-LC

Upstream: https://github.com/algesten/str0m (crate str0m-rust-crypto 0.7.0),
MIT OR Apache-2.0; upstream source and licences retained.

Only Cargo.toml and src/dtls.rs differ. Disable dimpl's rcgen feature (it forces AWS-LC),
generate the P-256 self-signed certificate with rcgen's ring provider already used by dhd_net,
and explicitly select dimpl's RustCrypto provider. The key and certificate stay ephemeral.
AWS-LC's Windows objects embed absolute build paths and fail the unchanged archive gate.
No security checks or certificate validation are relaxed. Finding 158 records validation.
