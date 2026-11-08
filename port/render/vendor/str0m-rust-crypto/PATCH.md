# str0m-rust-crypto 0.7.0: certificate generation without AWS-LC

Upstream: https://github.com/algesten/str0m (crate str0m-rust-crypto 0.7.0),
MIT OR Apache-2.0; upstream source and licences retained.

Only Cargo.toml and src/dtls.rs differ. Disable dimpl's rcgen feature (it forces AWS-LC),
generate the P-256 self-signed certificate with rcgen's ring provider already used by dhd_net,
and explicitly select dimpl's RustCrypto provider. The key and certificate stay ephemeral.
AWS-LC's Windows objects embed absolute build paths and fail the unchanged archive gate.
No security checks or certificate validation are relaxed. Finding 158 records validation.

## Reapplying on an update

Checked upstream main and latest published str0m 0.25.0 / adapter 0.7.0 / dimpl 0.9.0
on 2026-11-06. Upstream still enables dimpl `rcgen`, which unconditionally enables
AWS-LC. Cargo features are additive: a downstream feature choice cannot disable it.
Sources: [adapter manifest](https://github.com/algesten/str0m/blob/main/crypto/rust-crypto/Cargo.toml),
[dimpl manifest](https://github.com/algesten/dimpl/blob/main/Cargo.toml).
Platform crypto providers would split the native implementation; OpenSSL adds a new native
library. A scoped AWS-LC archive exception is possible, but accepting its absolute source paths
would retain machine details in release artifacts. The existing ring exception rewrites known
upstream paths; AWS-LC would need its own verified normalization of source/debug strings and
checksums, rather than an allowlist alone. No such clean build was verified here. Keep this
small patch until upstream separates certificate generation from its crypto provider or that
normalization is implemented and validated.

1. Replace this directory with the new published adapter crate; retain both licences.
   Current upstream revision: `5f2183ce7c67117cd1960dfeb11bed11fc188b66`, `crypto/rust-crypto`.
2. In Cargo.toml disable dimpl default features and remove only `rcgen` from its features;
   retain `rust-crypto`. Add rcgen with default features disabled, `ring` enabled
   (currently pinned to 0.14.10). Retain the existing time dependency.
3. In src/dtls.rs replace generate_certificate with rcgen KeyPair::generate_for
   (PKCS_ECDSA_P256_SHA256), CertificateParams::new(empty SAN list), validity now to
   now + 365 days, self_signed, returning cert DER and key.serialize_der().
4. Add `.with_crypto_provider(dimpl::crypto::rust_crypto::default_provider())` to the
   dimpl Config builder. Keep fingerprint verification and all other upstream behavior.
5. Update the workspace patch version and lockfile through rustgs.py; inspect the resolved
   graph for AWS-LC/OpenSSL. Run dhd_net native_channels and full library unit tests through
   rustgs.py, then check.py port, including the unchanged native archive gate.
