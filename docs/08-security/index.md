# 08 — Security

**Domain:** IES-001 §09 · Security, Reliability & Compliance.

**Master document:** [README.md](../../README.md) §09 (Security, Reliability & Compliance).

## What lives here

Security and reliability: posture, fail-closed design, containment of the autonomous system, and the TIDAL contractual truth. This directory is the index for the security domain; the authoritative text is the README master until dedicated security documents are written.

## Authoritative sources (existing)

- [TIDAL contracts (official program vs unofficial endpoint)](../TIDAL_CONTRACTS.md)
- [Privacy and security (Español)](../PRIVACIDAD_Y_SEGURIDAD.md)
- [Security (internal — do not publish details)](../SECURITY_INTERNAL.md)
- [Route B history (dead HAL route; wrapper v2.5.403 forbidden)](../HISTORIA_RUTA_B.md)
- [Security policy](../../SECURITY.md)

## Hard rules

- Fail-closed everywhere. No bypass as a product.
- FusionEngine is never created in `omega_create_effect` (lazy in `omega_process()`; 4th brick, 2026-11-11).
- No dangerous operations without the owner's explicit green light: no audioserver restart, no reboot, no module swap, no install.
- The self-repair domain is ONLY IVANNA-OMEGA-SUPREME. Never disable IntegrityGate, never touch credentials, never buy anything.
- Secrets are never in the repo, never in code, never in messages.

## Standards

References only, not certifications: AES, IEC 60268, ISO/IEC 25010, ISO/IEC 27001, Semantic Versioning, ADRs, CI/CD. No certification is claimed.
