# AEAD ring security probe

Uses a real `cvm_shared` mapping to test a 1 MiB AES-256-GCM KV chunk. It
demonstrates that authentication rejects modified ciphertext but accepts an
unchanged replay. A minimal receiver keeps the expected sequence number in
ordinary private CVM memory and advances it only after authentication passes.

This is a mechanism experiment, not a production protocol: production needs
session/key establishment, crash recovery, reordering policy, key rotation,
and durable or attested stream identity.

Observed on `cvm26-snp`: authentic chunk accepted, one-bit tamper rejected,
unchanged replay accepted by bare AEAD, replay rejected by private sequence
state, and failed authentication did not advance that state.
