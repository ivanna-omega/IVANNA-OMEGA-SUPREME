"""Tests de seguridad del cliente LLM (tools/evolution/llm_client.py).

La API key de OmniRoute solo existe para el gateway LOCAL (localhost:20128).
Si la URL del gateway apuntara a otro host, la key se exfiltraría: `_split_url`
es el punto de estrangulamiento y debe rechazar cualquier host fuera de la
allowlist ANTES de abrir el socket. Ningún test toca la red real.
"""
import os
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import llm_client  # noqa: E402
from llm_client import LlmError  # noqa: E402


class TestHostAllowlist(unittest.TestCase):
    def test_localhost_allowed(self):
        self.assertEqual(("http", "localhost", 20128),
                         llm_client._split_url("http://localhost:20128/v1"))

    def test_loopback_ip_allowed(self):
        self.assertEqual(("http", "127.0.0.1", 20128),
                         llm_client._split_url("http://127.0.0.1:20128/v1"))

    def test_ipv6_loopback_allowed(self):
        scheme, host, port = llm_client._split_url("http://[::1]:20128/v1")
        self.assertEqual(("http", "::1", 20128), (scheme, host, port))

    def test_custom_local_port_allowed(self):
        # La variable puede cambiar puerto/ruta, nunca el host.
        self.assertEqual(("http", "localhost", 9999),
                         llm_client._split_url("http://localhost:9999/otra/ruta"))

    def test_evil_host_blocked(self):
        with self.assertRaises(LlmError):
            llm_client._split_url("https://evil.example/v1")

    def test_github_host_blocked(self):
        # Ni siquiera un host legítimo: la key no es para GitHub.
        with self.assertRaises(LlmError):
            llm_client._split_url("https://github.com/v1")

    def test_localhost_subdomain_blocked(self):
        # "evil-localhost.com" no es localhost; el match es exacto.
        with self.assertRaises(LlmError):
            llm_client._split_url("http://evil-localhost.com/v1")
        with self.assertRaises(LlmError):
            llm_client._split_url("http://localhost.evil.example/v1")

    def test_unsupported_scheme_blocked(self):
        with self.assertRaises(LlmError):
            llm_client._split_url("ftp://localhost:21/v1")

    def test_chat_never_connects_to_foreign_host(self):
        # IVANNA_OMNIROUTE_URL ajena: chat() debe fallar en _split_url,
        # sin abrir ningún socket (sin red, sin exfiltración de la key).
        os.environ["IVANNA_OMNIROUTE_URL"] = "https://evil.example/v1"
        try:
            with self.assertRaises(LlmError) as ctx:
                llm_client.chat([{"role": "user", "content": "hola"}],
                                max_tokens=4, read_timeout_s=5)
            self.assertIn("fuera de dominio", str(ctx.exception))
        finally:
            del os.environ["IVANNA_OMNIROUTE_URL"]


if __name__ == "__main__":
    unittest.main()
